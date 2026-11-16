"""mini-redis 웹 인스펙터: 브라우저 ↔ (HTTP) ↔ 이 브리지 ↔ (TCP, RESP) ↔ mini-redis

실행: python3 inspector/server.py [--port 8080] [--redis-host 127.0.0.1] [--redis-port 6379]
      → 브라우저에서 http://localhost:8080

브라우저는 TCP 소켓을 직접 열 수 없어서 이 브리지가 중계한다. redis-py 를 쓰지 않고 소켓에 RESP 바이트를
직접 써서, 실제로 오간 바이트를 그대로 화면에 보여 준다. 표준 라이브러리만 쓴다.

연결은 두 개다.
  user  : 사용자가 보낸 명령만 지나간다. 이어진 채로 두므로 패킷 나눠 보내기·파이프라이닝 실험이 그대로 된다.
  probe : 화면 갱신용 DEBUG HT 와 대량 입력. 사용자 로그에 섞이지 않는다.

API
  GET  /api/state?max=256       DEBUG HT main / expires 를 JSON 으로
  POST /api/send  {"args":[..]} 인자 배열을 RESP 배열로 만들어 보내고 응답 1개를 기다린다
  POST /api/raw   {"text":".."} 입력한 바이트를 그대로 보낸다(\\r \\n \\t \\\\ \\xHH 해석). 응답이 멈출 때까지 모은다
  POST /api/fill  {"n":1000,"prefix":"key:","ttl_ms":0}  probe 연결로 SET 을 파이프라이닝
  POST /api/reconnect           user 연결을 새로 연다(받다 만 응답도 버린다)
"""
import argparse
import json
import os
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))


# ---- RESP ----


class Incomplete(Exception):
    pass


class ProtocolError(Exception):
    pass


def encode(args):
    out = [b"*%d\r\n" % len(args)]
    for a in args:
        b = a.encode("utf-8") if isinstance(a, str) else a
        out.append(b"$%d\r\n%s\r\n" % (len(b), b))
    return b"".join(out)


def _line(data, pos):
    end = data.find(b"\r\n", pos)
    if end < 0:
        raise Incomplete()
    return data[pos:end], end + 2


def parse_reply(data, pos=0):
    """응답 하나를 읽어 (값, 다음 위치). 값은 {"t": 종류, "v": ...}. 모자라면 Incomplete"""
    if pos >= len(data):
        raise Incomplete()
    kind = data[pos:pos + 1]
    line, nxt = _line(data, pos + 1)
    if kind == b"+":
        return {"t": "simple", "v": text(line)}, nxt
    if kind == b"-":
        return {"t": "error", "v": text(line)}, nxt
    if kind == b":":
        return {"t": "int", "v": int(line)}, nxt
    if kind == b"$":
        n = int(line)
        if n < 0:
            return {"t": "null", "v": None}, nxt
        if len(data) < nxt + n + 2:
            raise Incomplete()
        return {"t": "bulk", "v": text(data[nxt:nxt + n])}, nxt + n + 2
    if kind == b"*":
        n = int(line)
        if n < 0:
            return {"t": "null", "v": None}, nxt
        items = []
        for _ in range(n):
            v, nxt = parse_reply(data, nxt)
            items.append(v)
        return {"t": "array", "v": items}, nxt
    raise ProtocolError("unknown reply type %r" % kind)


def text(b):
    return b.decode("utf-8", errors="replace")


def spans(data):
    """바이트를 RESP 구조대로 나눈다: head(머리줄) / data(내용) / crlf / inline / rest(아직 덜 온 부분).
    화면에서 색을 나눠 보여 주는 용도라 형식이 틀려도 멈추지 않고 나머지를 rest 로 돌린다."""
    out, pos = [], 0
    while pos < len(data):
        c = data[pos:pos + 1]
        end = data.find(b"\r\n", pos)
        if c in b"*$+-:" and c:
            if end < 0:
                out.append(("rest", data[pos:]))
                break
            head = data[pos:end]
            out.append(("head", head))
            out.append(("crlf", b"\r\n"))
            pos = end + 2
            if c == b"$":
                try:
                    n = int(head[1:])
                except ValueError:
                    n = -1
                if n >= 0:
                    chunk = data[pos:pos + n]
                    out.append(("data", chunk))
                    pos += len(chunk)
                    if len(chunk) < n:
                        break
                    tail = data[pos:pos + 2]
                    if tail == b"\r\n":
                        out.append(("crlf", tail))
                        pos += 2
                    elif tail:
                        out.append(("rest", data[pos:]))
                        break
        else:
            lf = data.find(b"\n", pos)
            if lf < 0:
                out.append(("rest", data[pos:]))
                break
            body = data[pos:lf - 1] if lf > pos and data[lf - 1:lf] == b"\r" else data[pos:lf]
            out.append(("inline", body))
            out.append(("crlf", data[pos + len(body):lf + 1]))
            pos = lf + 1
    return [{"k": k, "b": tokens(b)} for k, b in out if b]


def tokens(b):
    """화면용 조각: 일반 글자는 문자열, \\r \\n 과 제어·깨진 바이트는 {"x": "\\r"} 같은 표시로"""
    out, cur = [], bytearray()

    def flush():
        if cur:
            s = bytes(cur).decode("utf-8", errors="surrogateescape")
            run = ""
            for ch in s:
                o = ord(ch)
                if 0xDC80 <= o <= 0xDCFF:
                    if run:
                        out.append(run)
                        run = ""
                    out.append({"x": "\\x%02x" % (o - 0xDC00)})
                else:
                    run += ch
            if run:
                out.append(run)
            cur.clear()

    for byte in b:
        if byte == 13 or byte == 10 or byte == 9 or byte < 32 or byte == 127:
            flush()
            out.append({"x": {13: "\\r", 10: "\\n", 9: "\\t"}.get(byte, "\\x%02x" % byte)})
        else:
            cur.append(byte)
    flush()
    return out


def unescape(s):
    """입력창 글자 → 바이트. \\r \\n \\t \\\\ \\xHH 를 해석하고 나머지는 UTF-8"""
    out, i = bytearray(), 0
    while i < len(s):
        ch = s[i]
        if ch == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n in "rnt\\":
                out += {"r": b"\r", "n": b"\n", "t": b"\t", "\\": b"\\"}[n]
                i += 2
                continue
            hx = s[i + 2:i + 4]
            if n == "x" and len(hx) == 2 and all(c in "0123456789abcdefABCDEF" for c in hx):
                out.append(int(hx, 16))
                i += 4
                continue
        out += ch.encode("utf-8")
        i += 1
    return bytes(out)


# ---- 연결 ----


class Conn:
    """이어진 채로 쓰는 TCP 연결. 받다 만 응답은 pending 에 남겨 다음 호출에서 이어 읽는다"""

    def __init__(self, host, port):
        self.addr = (host, port)
        self.sock = None
        self.pending = b""
        self.lock = threading.Lock()

    def _connect(self):
        if self.sock is None:
            self.sock = socket.create_connection(self.addr, timeout=2)
            self.pending = b""

    def close(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None
        self.pending = b""

    def exchange(self, data, expect=None, idle=0.15, limit=3.0):
        """data 를 보내고 응답을 모은다. expect 개를 다 읽거나, (expect 가 None 이면) idle 초 동안 아무것도
        안 오면 멈춘다. 돌려주는 값: 이번에 받은 바이트, 완성된 응답 목록, 서버가 연결을 끊었는지"""
        with self.lock:
            self._connect()
            self.sock.sendall(data)
            got, replies, closed = b"", [], False
            deadline = time.monotonic() + limit
            while True:
                if expect is not None and len(replies) >= expect:
                    break
                remain = deadline - time.monotonic()
                if remain <= 0:
                    break
                self.sock.settimeout(min(remain, idle if expect is None else remain))
                try:
                    chunk = self.sock.recv(65536)
                except socket.timeout:
                    break
                if not chunk:
                    closed = True
                    break
                got += chunk
                self.pending += chunk
                while True:
                    try:
                        v, used = parse_reply(self.pending)
                    except Incomplete:
                        break
                    except (ValueError, ProtocolError):  # 응답이 RESP 가 아니면 남은 것을 버린다
                        self.pending = b""
                        break
                    replies.append(v)
                    self.pending = self.pending[used:]
            if closed:
                self.close()
            return got, replies, closed


def table_json(arr):
    size, used, buckets = arr["v"]
    return {
        "size": size["v"],
        "used": used["v"],
        "buckets": [[[e["v"][0]["v"], e["v"][1]["v"]] for e in b["v"]] for b in buckets["v"]],
    }


def debug_json(reply):
    if reply["t"] == "error":
        raise RuntimeError(reply["v"])
    items = reply["v"]
    out = {}
    for i in range(0, len(items), 2):
        name, val = items[i]["v"], items[i + 1]
        out[name] = [table_json(t) for t in val["v"]] if name == "tables" else val["v"]
    return out


# ---- HTTP ----


def make_handler(user, probe):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a):
            pass

        def _send(self, code, body, ctype="application/json; charset=utf-8"):
            data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def _body(self):
            n = int(self.headers.get("Content-Length") or 0)
            return json.loads(self.rfile.read(n) or b"{}")

        def do_GET(self):
            u = urlparse(self.path)
            if u.path in ("/", "/index.html"):
                with open(os.path.join(HERE, "index.html"), "rb") as f:
                    return self._send(200, f.read(), "text/html; charset=utf-8")
            if u.path == "/api/state":
                q = parse_qs(u.query)
                mx = q.get("max", ["256"])[0]
                try:
                    _, reps, _ = probe.exchange(encode(["DEBUG", "HT", "main", mx]) + encode(["DEBUG", "HT", "expires", mx]), expect=2)
                    if len(reps) < 2:
                        raise RuntimeError("응답이 오지 않았다")
                    return self._send(200, {"ok": True, "redis": "%s:%d" % probe.addr,
                                            "main": debug_json(reps[0]), "expires": debug_json(reps[1])})
                except (OSError, RuntimeError, KeyError, TypeError, IndexError) as e:
                    probe.close()
                    return self._send(200, {"ok": False, "redis": "%s:%d" % probe.addr, "error": str(e)})
            self._send(404, {"error": "not found"})

        def do_POST(self):
            u = urlparse(self.path)
            try:
                body = self._body()
            except (ValueError, json.JSONDecodeError):
                return self._send(400, {"error": "JSON 본문이 필요하다"})
            try:
                if u.path == "/api/send":
                    args = body.get("args") or []
                    if not args or not all(isinstance(a, str) for a in args):
                        return self._send(400, {"error": "args 는 비어 있지 않은 문자열 배열"})
                    return self._send(200, self._exchange(encode(args), 1))
                if u.path == "/api/raw":
                    data = unescape(body.get("text", ""))
                    if not data:
                        return self._send(400, {"error": "보낼 바이트가 없다"})
                    return self._send(200, self._exchange(data, None))
                if u.path == "/api/fill":
                    return self._send(200, self._fill(body))
                if u.path == "/api/reconnect":
                    user.close()
                    return self._send(200, {"ok": True})
            except OSError as e:
                user.close()
                return self._send(200, {"error": "mini-redis 에 연결할 수 없다: %s" % e})
            self._send(404, {"error": "not found"})

        def _exchange(self, data, expect):
            t0 = time.perf_counter()
            got, replies, closed = user.exchange(data, expect)
            return {
                "sent": spans(data),
                "recv": spans(got),
                "sent_len": len(data),
                "recv_len": len(got),
                "replies": replies,
                "pending": len(user.pending),
                "closed": closed,
                "ms": round((time.perf_counter() - t0) * 1000, 2),
            }

        def _fill(self, body):
            n = max(1, min(int(body.get("n", 100)), 100000))
            prefix = str(body.get("prefix", "key:"))
            ttl = int(body.get("ttl_ms", 0))
            t0 = time.perf_counter()
            parts = []
            for i in range(n):
                args = ["SET", "%s%d" % (prefix, i), "value-%d" % i]
                if ttl > 0:
                    args += ["PX", str(ttl)]
                parts.append(encode(args))
            _, reps, _ = probe.exchange(b"".join(parts), expect=n, limit=10.0)
            errors = [r["v"] for r in reps if r["t"] == "error"]
            return {"n": n, "ok": len(reps) - len(errors), "errors": errors[:3],
                    "ms": round((time.perf_counter() - t0) * 1000, 1)}

    return Handler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--redis-host", default="127.0.0.1")
    ap.add_argument("--redis-port", type=int, default=6379)
    a = ap.parse_args()
    user = Conn(a.redis_host, a.redis_port)
    probe = Conn(a.redis_host, a.redis_port)
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(user, probe))
    print("inspector: http://localhost:%d  →  mini-redis %s:%d" % (a.port, a.redis_host, a.redis_port), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
