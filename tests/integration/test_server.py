"""mini-redis 통합 테스트: 진짜 서버 프로세스를 띄우고 raw 소켓으로 확인한다. 표준 라이브러리만 쓴다.

사용법: python3 tests/integration/test_server.py ./mini-redis
테스트 ID: TC-INT-<번호> (portfolio/testing/test-cases-redis.md)
"""
import os
import socket
import subprocess
import sys
import threading
import time

BIN = sys.argv[1] if len(sys.argv) > 1 else "./mini-redis"
passed = 0


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


PORT = free_port()


def start_server():
    p = subprocess.Popen([BIN, "--port", str(PORT)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.1).close()
            return p
        except OSError:
            time.sleep(0.05)
    p.kill()
    raise SystemExit("서버가 뜨지 않음")


def conn(timeout=5.0):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def cmd(*args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        a = a if isinstance(a, bytes) else str(a).encode()
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out


class Reader:
    """RESP 응답을 하나씩 읽는다."""

    def __init__(self, s):
        self.s, self.buf = s, b""

    def _fill(self):
        chunk = self.s.recv(65536)
        if not chunk:
            raise EOFError
        self.buf += chunk

    def _line(self):
        while b"\r\n" not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def reply(self):
        line = self._line()
        t, rest = line[:1], line[1:]
        if t in (b"+", b"-", b":"):
            return line
        if t == b"$":
            n = int(rest)
            if n < 0:
                return None
            while len(self.buf) < n + 2:
                self._fill()
            data, self.buf = self.buf[:n], self.buf[n + 2:]
            return data
        if t == b"*":
            return [self.reply() for _ in range(int(rest))]
        raise ValueError(line)


def check(name, cond, detail=""):
    global passed
    if not cond:
        print(f"  FAIL {name} {detail}")
        server.kill()
        sys.exit(1)
    passed += 1
    print(f"  ok   {name}")


# TC-INT-001 분할 수신: 명령 두 개를 1바이트씩
def t_split():
    s = conn()
    for b in cmd("SET", "split", "ok") + cmd("GET", "split"):
        s.send(bytes([b]))
    r = Reader(s)
    check("TC-INT-001 1바이트씩 분할", r.reply() == b"+OK" and r.reply() == b"ok")
    s.close()


# TC-INT-002 파이프라이닝: 10000개를 한 번에 보내고 순서대로 응답
def t_pipeline():
    s = conn()
    s.sendall(b"".join(cmd("SET", f"p{i}", i) for i in range(10000)) + cmd("GET", "p9999"))
    r = Reader(s)
    oks = sum(r.reply() == b"+OK" for _ in range(10000))
    check("TC-INT-002 파이프라인 10000개", oks == 10000 and r.reply() == b"9999")
    s.close()


# TC-INT-003 프로토콜 오류: 에러 응답 뒤 연결이 닫힘, 서버는 계속 동작
def t_protocol_error():
    s = conn()
    s.sendall(b"*1\r\n$-5\r\n")
    r = Reader(s)
    first = r.reply()
    closed = False
    try:
        r.reply()
    except EOFError:
        closed = True
    check("TC-INT-003 프로토콜 오류 후 닫힘", first.startswith(b"-ERR Protocol error") and closed)
    s2 = conn()
    s2.sendall(cmd("PING"))
    check("TC-INT-003b 서버는 계속 동작", Reader(s2).reply() == b"+PONG")


# TC-INT-004 동시성: 클라이언트 50개가 동시에 각자 키 1000개 → 50000개 모두 정확 (학습 노트 7장)
def t_concurrency():
    errors = []

    def worker(w):
        try:
            s = conn(10)
            s.sendall(b"".join(cmd("SET", f"c{w}:{i}", f"{w}-{i}") for i in range(1000)))
            r = Reader(s)
            for _ in range(1000):
                if r.reply() != b"+OK":
                    errors.append(("set", w))
            s.sendall(b"".join(cmd("GET", f"c{w}:{i}") for i in range(1000)))
            for i in range(1000):
                if r.reply() != f"{w}-{i}".encode():
                    errors.append(("get", w, i))
            s.close()
        except Exception as e:  # noqa: BLE001
            errors.append(("exc", w, repr(e)))

    ts = [threading.Thread(target=worker, args=(w,)) for w in range(50)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    s = conn()
    s.sendall(cmd("KEYS", "c*"))
    keys = Reader(s).reply()
    check("TC-INT-004 50개 동시 × 1000키", not errors and len(keys) == 50000, str(errors[:3]))


# TC-INT-005 공정성: 한 클라이언트가 쉬지 않고 쏟아붓는 동안 다른 클라이언트의 PING 이 굶지 않는다 (Q16)
def t_fairness():
    stop = threading.Event()
    big = b"".join(cmd("SET", "flood", "x" * 100) for _ in range(2000))

    def flooder():
        s = conn(30)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)

        def drain():  # 응답은 계속 읽어 버린다(출력 버퍼 상한에 걸리지 않게)
            try:
                while not stop.is_set() and s.recv(1 << 20):
                    pass
            except OSError:
                pass

        threading.Thread(target=drain, daemon=True).start()
        while not stop.is_set():
            try:
                s.sendall(big)
            except OSError:
                break
        s.close()

    f = threading.Thread(target=flooder, daemon=True)
    f.start()
    time.sleep(0.3)
    s = conn(10)
    r = Reader(s)
    worst = 0.0
    for _ in range(50):
        t0 = time.perf_counter()
        s.sendall(cmd("PING"))
        r.reply()
        worst = max(worst, time.perf_counter() - t0)
        time.sleep(0.01)
    stop.set()
    check(f"TC-INT-005 쏟아붓는 손님 옆에서 PING 최악 지연 < 100ms (최악 {worst * 1000:.1f}ms)", worst < 0.1)


# TC-INT-006 출력 버퍼 상한: 큰 응답을 요청하고 읽지 않는 손님은 끊긴다, 서버는 계속 동작 (Q17)
def t_output_limit():
    s = conn()
    s.sendall(cmd("SET", "big", "y" * (1 << 20)))
    assert Reader(s).reply() == b"+OK"
    lazy = conn(10)
    lazy.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    lazy.sendall(cmd("GET", "big") * 200)  # 응답 약 200MB, 읽지 않음
    time.sleep(1.5)
    got, closed = 0, False
    try:
        while True:
            chunk = lazy.recv(1 << 20)
            if not chunk:
                closed = True
                break
            got += len(chunk)
    except (ConnectionResetError, socket.timeout):
        closed = True
    check("TC-INT-006 읽지 않는 손님은 출력 64MB 넘으면 끊김", closed and got < 150 * (1 << 20), f"받은 {got >> 20}MB")
    s.sendall(cmd("PING"))
    check("TC-INT-006b 서버는 계속 동작", Reader(s).reply() == b"+PONG")


server = start_server()
try:
    for t in (t_split, t_pipeline, t_protocol_error, t_concurrency, t_fairness, t_output_limit):
        t()
finally:
    server.terminate()
    server.wait(5)
print(f"  ok ({passed} checks)")
