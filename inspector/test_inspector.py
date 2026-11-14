"""웹 인스펙터 브리지 테스트: 진짜 mini-redis 와 브리지를 띄우고 HTTP API 로 확인 (표준 라이브러리만).

사용법: python3 inspector/test_inspector.py ./mini-redis
테스트 ID: TC-INS-<번호>
"""
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server as bridge  # noqa: E402

MINI = sys.argv[1] if len(sys.argv) > 1 else "./mini-redis"
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
    procs.append(subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    wait_port(port)


def check(name, cond, detail=""):
    global passed
    if not cond:
        print(f"  FAIL {name} {detail}")
        for p in procs:
            p.kill()
        sys.exit(1)
    passed += 1


def flat(spans):
    """spans → (종류, 화면 글자) 목록"""
    return [(s["k"], "".join(t if isinstance(t, str) else t["x"] for t in s["b"])) for s in spans]


def main():
    # TC-INS-001 EP/EG: 도우미 함수 — 입력창 escape 해석, RESP 구조 나누기, 덜 온 부분
    check("001 unescape", bridge.unescape(r"a\r\n\t\\\x41\xzz") == b"a\r\n\t\\A\\xzz")
    check("001 unescape utf8", bridge.unescape("한") == "한".encode())
    s = flat(bridge.spans(b"*1\r\n$4\r\nPING\r\n"))
    check("001 spans", s == [("head", "*1"), ("crlf", "\\r\\n"), ("head", "$4"), ("crlf", "\\r\\n"),
                             ("data", "PING"), ("crlf", "\\r\\n")], s)
    s = flat(bridge.spans(b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhel"))
    check("001 spans partial", s[-1] == ("data", "hel") and s[-2] == ("crlf", "\\r\\n"), s)
    check("001 spans inline", flat(bridge.spans(b"PING\r\n")) == [("inline", "PING"), ("crlf", "\\r\\n")])
    check("001 spans nul", flat(bridge.spans(b"$3\r\na\x00b\r\n"))[2] == ("data", "a\\x00b"))
    v, used = bridge.parse_reply(b"*2\r\n$1\r\na\r\n:5\r\n+x")
    check("001 parse", v == {"t": "array", "v": [{"t": "bulk", "v": "a"}, {"t": "int", "v": 5}]} and used == 15)

    rport, hport = free_port(), free_port()
    start([MINI, "--port", str(rport)], rport)
    start([sys.executable, "inspector/server.py", "--port", str(hport), "--redis-port", str(rport)], hport)
    base = f"http://127.0.0.1:{hport}"

    def get(path):
        with urllib.request.urlopen(base + path, timeout=10) as r:
            return r.status, r.read()

    def post(path, body):
        r = urllib.request.Request(base + path, data=json.dumps(body).encode(), method="POST",
                                   headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(r, timeout=15) as resp:
            return json.loads(resp.read())

    def state():
        return json.loads(get("/api/state?max=1024")[1])

    # TC-INS-002 EP: 페이지와 명령 한 번 — 보낸 바이트, 받은 바이트, 해석한 응답
    code, html = get("/")
    check("002 page", code == 200 and b"mini-redis" in html)
    r = post("/api/send", {"args": ["SET", "name", "cc8s", "EX", "60"]})
    check("002 sent", r["sent_len"] == len(b"*5\r\n$3\r\nSET\r\n$4\r\nname\r\n$4\r\ncc8s\r\n$2\r\nEX\r\n$2\r\n60\r\n"), r)
    check("002 recv", flat(r["recv"]) == [("head", "+OK"), ("crlf", "\\r\\n")] and r["replies"] == [{"t": "simple", "v": "OK"}], r)
    r = post("/api/send", {"args": ["GET", "name"]})
    check("002 get", r["replies"] == [{"t": "bulk", "v": "cc8s"}], r)

    # TC-INS-003 EP: 상태 — main 에 값, expires 에 만료 시각
    st = state()
    check("003 ok", st["ok"], st)
    main_keys = {k: v for t in st["main"]["tables"] for b in t["buckets"] for k, v in b}
    exp_keys = {k: w for t in st["expires"]["tables"] for b in t["buckets"] for k, w in b}
    check("003 main", main_keys == {"name": "cc8s"}, main_keys)
    left = exp_keys.get("name", 0) - st["main"]["now_ms"]
    check("003 expires", 59000 < left <= 60000, exp_keys)

    # TC-INS-004 ST: 상태 조회는 rehash 를 진행시키지 않는다 (능동 rehash 끈 상태에서 몇 번 읽어도 rehashidx 그대로)
    post("/api/send", {"args": ["DEBUG", "ACTIVEREHASH", "off"]})
    post("/api/send", {"args": ["FLUSHALL"]})
    for i in range(1, 6):
        post("/api/send", {"args": ["SET", f"k{i}", "v"]})
    a = state()["main"]
    time.sleep(0.3)  # cron 이 몇 번 돌 시간
    b = state()["main"]
    check("004 rehashing", a["rehashidx"] >= 0 and a["tables"][1]["size"] == 8, a)
    check("004 readonly", b["rehashidx"] == a["rehashidx"] and b["active_rehash"] == 0, (a["rehashidx"], b["rehashidx"]))
    post("/api/send", {"args": ["DEBUG", "REHASHSTEP", "main", "100"]})
    check("004 done", state()["main"]["rehashidx"] == -1)
    post("/api/send", {"args": ["DEBUG", "ACTIVEREHASH", "on"]})

    # TC-INS-005 ST: 나눠 보내기 — 앞부분만 보내면 응답이 없고, 나머지를 보내면 +OK
    r = post("/api/raw", {"text": r"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhel"})
    check("005 part1", r["replies"] == [] and r["recv_len"] == 0 and not r["closed"], r)
    r = post("/api/raw", {"text": r"lo\r\n"})
    check("005 part2", r["replies"] == [{"t": "simple", "v": "OK"}], r)

    # TC-INS-006 EP: 파이프라이닝 — 한 번에 보내고 응답 3개
    r = post("/api/raw", {"text": r"*1\r\n$4\r\nPING\r\n*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\n"})
    check("006 pipeline", [x["v"] for x in r["replies"]] == ["PONG", "hi", "hello"], r)

    # TC-INS-007 EG: 프로토콜 오류 — 서버가 에러를 보내고 끊는다. 다음 요청은 새 연결로 된다
    r = post("/api/raw", {"text": r"*1\r\n$x\r\n"})
    check("007 error", r["replies"] and r["replies"][0]["t"] == "error" and r["closed"], r)
    r = post("/api/send", {"args": ["PING"]})
    check("007 reconnect", r["replies"] == [{"t": "simple", "v": "PONG"}], r)

    # TC-INS-008 BVA: 대량 입력 — 1000개, 버킷 1024개까지 커짐. 잘못된 요청은 400
    r = post("/api/fill", {"n": 1000, "prefix": "f:", "ttl_ms": 0})
    check("008 fill", r["ok"] == 1000 and r["errors"] == [], r)
    st = state()["main"]
    total = sum(t["used"] for t in st["tables"])
    check("008 size", total == 1006 and max(t["size"] for t in st["tables"]) == 1024, (total, [t["size"] for t in st["tables"]]))
    try:
        post("/api/send", {"args": []})
        check("008 bad", False)
    except urllib.error.HTTPError as e:
        check("008 bad", e.code == 400)

    # TC-INS-009 EG: mini-redis 가 꺼지면 상태는 ok=false 로 알려 준다(브리지는 죽지 않음)
    procs[0].kill()
    procs[0].wait()
    st = state()
    check("009 down", st["ok"] is False and st["error"], st)

    for p in procs:
        p.kill()
    print(f"  ok ({passed} checks)")


if __name__ == "__main__":
    main()
