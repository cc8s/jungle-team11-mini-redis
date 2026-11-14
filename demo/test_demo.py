"""데모 API 기능 테스트: 외부에서 mini-redis 를 캐시로 쓰는 데 문제가 없는지 (과제 품질 요구사항).

사용법: python3 demo/test_demo.py ./mini-redis      (redis-py 필요)
테스트 ID: TC-DEMO-<번호>
"""
import json
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

import redis

MINI = sys.argv[1] if len(sys.argv) > 1 else "./mini-redis"
DELAY_MS = 100
passed = 0
procs = []


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_port(port):
    for _ in range(200):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.1).close()
            return
        except OSError:
            time.sleep(0.05)
    raise SystemExit(f"{port} 가 뜨지 않음")


def start(args, port):
    p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    procs.append(p)
    wait_port(port)
    return p


def start_app(redis_port, ttl=60, invalidate=True):
    port = free_port()
    args = [sys.executable, "demo/app.py", "--port", str(port), "--redis-port", str(redis_port),
            "--delay-ms", str(DELAY_MS), "--ttl", str(ttl)]
    if not invalidate:
        args.append("--no-invalidate")
    start(args, port)
    return f"http://127.0.0.1:{port}"


def req(base, method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(base + path, data=data, method=method, headers={"Content-Type": "application/json"})
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(r, timeout=10) as resp:
            return resp.status, json.loads(resp.read()), resp.headers.get("X-Cache"), time.perf_counter() - t0
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read()), e.headers.get("X-Cache"), time.perf_counter() - t0


def check(name, cond, detail=""):
    global passed
    if not cond:
        print(f"  FAIL {name} {detail}")
        for p in procs:
            p.kill()
        sys.exit(1)
    passed += 1
    print(f"  ok   {name}")


rport = free_port()
mini = start([MINI, "--port", str(rport)], rport)
r = redis.Redis(port=rport)
try:
    app = start_app(rport, ttl=60)

    # TC-DEMO-001 cache-aside: 처음 MISS(느림) → 다음 HIT(빠름), 같은 내용
    s1, b1, x1, t1 = req(app, "GET", "/users/1")
    s2, b2, x2, t2 = req(app, "GET", "/users/1")
    check(f"TC-DEMO-001 MISS {t1 * 1000:.0f}ms → HIT {t2 * 1000:.1f}ms",
          (s1, x1, s2, x2) == (200, "MISS", 200, "HIT") and b1 == b2 and t1 >= DELAY_MS / 1000 and t2 < 0.05)

    # TC-DEMO-002 redis-py 로 본 캐시: 키와 TTL
    ttl = r.ttl("user:1")
    check(f"TC-DEMO-002 redis-py 로 확인: user:1 TTL {ttl}", r.exists("user:1") == 1 and 0 < ttl <= 60)

    # TC-DEMO-003 무효화: PUT → DEL → 다음 GET 은 새 값 (MISS)
    req(app, "PUT", "/users/1", {"name": "renamed"})
    s, b, x, _ = req(app, "GET", "/users/1")
    check("TC-DEMO-003 PUT 뒤 GET 은 새 값(무효화)", b["name"] == "renamed" and x == "MISS" and b["version"] == 2)

    # TC-DEMO-004 캐시 우회: 매번 원본 (비교용)
    req(app, "POST", "/stats/reset")
    for _ in range(3):
        _, _, x, t = req(app, "GET", "/users/2?cache=0")
    _, st, _, _ = req(app, "GET", "/stats")
    check("TC-DEMO-004 ?cache=0 은 매번 원본", x == "BYPASS" and st["source_reads"] == 3 and t >= DELAY_MS / 1000)

    # TC-DEMO-005 없는 사용자: 404, 캐시에 남기지 않음
    s, _, _, _ = req(app, "GET", "/users/999999")
    check("TC-DEMO-005 없는 사용자 404, 캐시 안 함", s == 404 and r.exists("user:999999") == 0)

    # TC-DEMO-006 TTL 안전망: 무효화를 끈 앱에서는 TTL 이 끝날 때까지 옛 값, 끝나면 새 값
    stale_app = start_app(rport, ttl=1, invalidate=False)
    req(stale_app, "GET", "/users/3")
    req(stale_app, "PUT", "/users/3", {"name": "new3"})
    _, b_stale, x_stale, _ = req(stale_app, "GET", "/users/3")
    time.sleep(1.3)
    _, b_new, _, _ = req(stale_app, "GET", "/users/3")
    check("TC-DEMO-006 무효화 없으면 TTL 동안 옛 값 → 만료 후 새 값",
          b_stale["name"] == "user-3" and x_stale == "HIT" and b_new["name"] == "new3")

    # TC-DEMO-007 캐시 스탬피드: 빈 키에 동시 20개 → 락 없으면 원본 여러 번, SET NX 락이면 1번
    def burst(path, n=20):
        req(app, "POST", "/stats/reset")
        ts = [threading.Thread(target=req, args=(app, "GET", path)) for _ in range(n)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        return req(app, "GET", "/stats")[1]["source_reads"]

    r.delete("user:4", "user:5")
    no_lock = burst("/users/4")
    with_lock = burst("/users/5?lock=1")
    check(f"TC-DEMO-007 스탬피드: 락 없음 원본 {no_lock}번, SET NX 락 원본 {with_lock}번", no_lock > 1 and with_lock == 1)

    # TC-DEMO-008 캐시 서버가 죽어도 API 는 원본으로 응답
    mini.terminate()
    mini.wait(5)
    s, b, x, t = req(app, "GET", "/users/6")
    check(f"TC-DEMO-008 캐시 다운: {s} {x} {t * 1000:.0f}ms", s == 200 and x == "DOWN" and b["id"] == 6)
finally:
    for p in procs:
        p.terminate()
        p.wait(5)
print(f"  ok ({passed} checks)")
