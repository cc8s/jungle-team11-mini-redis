"""느린 원본 + mini-redis 캐시 데모 API (R005: cache-aside + 쓰기 시 DEL + TTL 안전망).

실행: python3 demo/app.py [--port 8000] [--redis-port 6379] [--delay-ms 100] [--ttl 60] [--no-invalidate]

엔드포인트
  GET  /users/<id>            캐시 우선 조회. 응답 헤더 X-Cache: HIT | MISS | BYPASS | DOWN
       ?cache=0               캐시를 거치지 않고 원본만 (비교용)
       ?lock=1                캐시가 비었을 때 SET NX 락으로 원본 조회를 한 번만 (캐시 스탬피드 방지, D8)
  PUT  /users/<id>  {"name"}  원본 수정 → 캐시 키 DEL (무효화). --no-invalidate 면 DEL 하지 않음(TTL 만 믿음)
  GET  /stats                 {"source_reads": 원본 조회 횟수}
  POST /stats/reset

원본은 메모리 dict 에 일부러 delay-ms 만큼 지연을 넣어 느린 DB·외부 API 를 흉내 낸다.
캐시 서버가 죽어도 API 는 원본으로 응답한다(X-Cache: DOWN).
"""
import argparse
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import redis

# ---- 느린 원본 ----


class SlowSource:
    def __init__(self, delay_ms, n_users=1000):
        self.delay = delay_ms / 1000
        self.lock = threading.Lock()
        self.reads = 0
        self.users = {i: {"id": i, "name": f"user-{i}", "version": 1} for i in range(1, n_users + 1)}

    def get(self, uid):
        time.sleep(self.delay)
        with self.lock:
            self.reads += 1
            u = self.users.get(uid)
            return dict(u) if u else None

    def update(self, uid, name):
        time.sleep(self.delay)
        with self.lock:
            u = self.users.get(uid)
            if u is None:
                return None
            u["name"] = name
            u["version"] += 1
            return dict(u)


# ---- 캐시 ----


def cache_key(uid):
    return f"user:{uid}"


class Cache:
    def __init__(self, port, ttl, invalidate):
        self.r = redis.Redis(host="127.0.0.1", port=port, socket_timeout=0.2, socket_connect_timeout=0.2)
        self.ttl, self.invalidate = ttl, invalidate

    def read_through(self, src, uid, use_lock):
        """(값, X-Cache). 캐시가 죽었으면 원본으로"""
        key = cache_key(uid)
        try:
            hit = self.r.get(key)
        except redis.exceptions.RedisError:
            return src.get(uid), "DOWN"
        if hit is not None:
            return json.loads(hit), "HIT"
        if use_lock:
            return self._load_with_lock(src, uid, key), "MISS"
        return self._load(src, uid, key), "MISS"

    def _load(self, src, uid, key):
        user = src.get(uid)
        if user is not None:  # 없는 사용자는 캐시하지 않는다(알려진 한계: 캐시 관통)
            try:
                self.r.set(key, json.dumps(user), ex=self.ttl)
            except redis.exceptions.RedisError:
                pass
        return user

    def _load_with_lock(self, src, uid, key):
        """SET lock NX PX: 락을 잡은 요청 하나만 원본을 읽고, 나머지는 캐시가 채워지기를 기다린다"""
        lock = f"lock:{key}"
        if self.r.set(lock, "1", nx=True, px=2000):
            try:
                return self._load(src, uid, key)
            finally:
                self.r.delete(lock)
        deadline = time.time() + 2
        while time.time() < deadline:
            time.sleep(0.01)
            hit = self.r.get(key)
            if hit is not None:
                return json.loads(hit)
        return src.get(uid)  # 락 주인이 실패했으면 직접

    def on_write(self, uid):
        if self.invalidate:
            try:
                self.r.delete(cache_key(uid))
            except redis.exceptions.RedisError:
                pass  # 캐시가 죽었으면 TTL 이 안전망


# ---- HTTP ----


def make_handler(src, cache):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):  # 조용히
            pass

        def _send(self, code, body, x_cache=None):
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            if x_cache:
                self.send_header("X-Cache", x_cache)
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            if u.path == "/stats":
                return self._send(200, {"source_reads": src.reads})
            m = re.fullmatch(r"/users/(\d+)", u.path)
            if not m:
                return self._send(404, {"error": "not found"})
            uid = int(m.group(1))
            if q.get("cache") == ["0"]:
                user, xc = src.get(uid), "BYPASS"
            else:
                user, xc = cache.read_through(src, uid, q.get("lock") == ["1"])
            if user is None:
                return self._send(404, {"error": "no such user"}, xc)
            self._send(200, user, xc)

        def do_PUT(self):
            m = re.fullmatch(r"/users/(\d+)", urlparse(self.path).path)
            if not m:
                return self._send(404, {"error": "not found"})
            n = int(self.headers.get("Content-Length", 0))
            try:
                name = json.loads(self.rfile.read(n))["name"]
            except (ValueError, KeyError, TypeError):
                return self._send(400, {"error": "body must be {\"name\": ...}"})
            uid = int(m.group(1))
            user = src.update(uid, name)
            if user is None:
                return self._send(404, {"error": "no such user"})
            cache.on_write(uid)
            self._send(200, user)

        def do_POST(self):
            if urlparse(self.path).path == "/stats/reset":
                with src.lock:
                    src.reads = 0
                return self._send(200, {"source_reads": 0})
            self._send(404, {"error": "not found"})

    return Handler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8000)
    ap.add_argument("--redis-port", type=int, default=6379)
    ap.add_argument("--delay-ms", type=int, default=100)
    ap.add_argument("--ttl", type=int, default=60)
    ap.add_argument("--no-invalidate", action="store_true")
    a = ap.parse_args()
    src = SlowSource(a.delay_ms)
    cache = Cache(a.redis_port, a.ttl, not a.no_invalidate)
    # 연결 대기열 기본값이 5 라 동시 요청이 몰리면 넘친 SYN 이 버려지고 1초 뒤 재전송된다(bench p99 1005ms 의 원인)
    ThreadingHTTPServer.request_queue_size = 1024
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(src, cache))
    srv.daemon_threads = True
    print(f"demo: http://127.0.0.1:{a.port} (원본 지연 {a.delay_ms}ms, TTL {a.ttl}s, 무효화 {'끔' if a.no_invalidate else '켬'})",
          flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
