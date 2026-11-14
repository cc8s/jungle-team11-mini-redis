"""차등 테스트 (R007 D6): 같은 명령을 진짜 Redis 와 mini-redis 에 보내 응답 바이트를 비교한다.

기대값을 사람이나 AI 가 적지 않는다. 진짜 Redis 의 응답이 정답이다.

사용법: python3 tests/integration/test_diff.py ./mini-redis [redis-server 경로]
  - 진짜 Redis 는 7 이상이어야 한다(에러 문구가 6 과 다르다). 버전이 낮으면 실패로 끝낸다.
  - 비교 규칙
      KEYS          순서가 해시 순서라 정해져 있지 않다 → 정렬해서 비교
      PTTL          두 서버에 보내는 시각이 조금 다르다 → 50ms 까지 차이 허용
  - 일부러 비교하지 않는 것(우리는 접속용 최소 응답만 함, R007 D3): COMMAND, CONFIG, CLIENT, SELECT 1 이상
테스트 ID: TC-DIFF-<번호>
"""
import random
import socket
import subprocess
import sys
import time

MINI = sys.argv[1] if len(sys.argv) > 1 else "./mini-redis"
REAL = sys.argv[2] if len(sys.argv) > 2 else "redis-server"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_up(port):
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.1).close()
            return
        except OSError:
            time.sleep(0.05)
    raise SystemExit(f"{port} 번 서버가 뜨지 않음")


def enc(args):
    out = b"*%d\r\n" % len(args)
    for a in args:
        a = a if isinstance(a, bytes) else str(a).encode()
        out += b"$%d\r\n%s\r\n" % (len(a), a)
    return out


class Conn:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""

    def _fill(self):
        c = self.s.recv(65536)
        if not c:
            raise EOFError
        self.buf += c

    def _line(self):
        while b"\r\n" not in self.buf:
            self._fill()
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def reply(self):
        """응답 하나를 (원래 바이트, 해석한 값)으로"""
        line = self._line()
        raw = line + b"\r\n"
        t, rest = line[:1], line[1:]
        if t in (b"+", b"-", b":"):
            return raw, line
        if t == b"$":
            n = int(rest)
            if n < 0:
                return raw, None
            while len(self.buf) < n + 2:
                self._fill()
            data, self.buf = self.buf[:n], self.buf[n + 2:]
            return raw + data + b"\r\n", data
        if t == b"*":
            items = [self.reply() for _ in range(int(rest))]
            return raw + b"".join(r for r, _ in items), [v for _, v in items]
        raise ValueError(line)

    def send_raw(self, data):
        self.s.sendall(data)


passed = 0
mismatches = []


def compare(label, args, a, b):
    """a = 진짜 Redis, b = mini-redis 의 (raw, 값)"""
    global passed
    name = (args[0] if isinstance(args[0], str) else args[0].decode()).upper() if args else ""
    ok = a[0] == b[0]
    if not ok and name == "KEYS" and isinstance(a[1], list) and isinstance(b[1], list):
        ok = sorted(a[1]) == sorted(b[1])
    if not ok and name == "PTTL" and a[0][:1] == b[0][:1] == b":":
        x, y = int(a[1][1:]), int(b[1][1:])
        ok = (x < 0 and x == y) or (x >= 0 and y >= 0 and abs(x - y) <= 50)
    if ok:
        passed += 1
    else:
        mismatches.append((label, args, a[0], b[0]))


def run_both(label, seq):
    """명령 목록을 두 서버에 같은 순서로 보내고 하나씩 비교"""
    for args in seq:
        real.send_raw(enc(args))
        mini.send_raw(enc(args))
        compare(label, args, real.reply(), mini.reply())


def raw_both(label, data, n_replies=1):
    """프로토콜 바이트를 그대로 보내고 응답 n 개 비교(형식 오류 응답 포함)"""
    r, m = Conn(REAL_PORT), Conn(MINI_PORT)
    r.s.settimeout(1.0)
    m.s.settimeout(1.0)
    r.send_raw(data)
    m.send_raw(data)

    def get(c):  # 응답 없이 더 기다리는 것도 결과다
        try:
            return c.reply()
        except (socket.timeout, TimeoutError):
            return (b"(waiting)", None)
        except EOFError:
            return (b"(closed)", None)

    for _ in range(n_replies):
        compare(label, [data[:30]], get(r), get(m))


def flush():
    run_both("flush", [["FLUSHALL"]])


# ---- 시나리오 (TC-DIFF-001~009): 기대값 없이 명령만 적는다 ----

SCENARIOS = {
    "TC-DIFF-001 기본·대소문자·nil": [
        ["PING"], ["ping", "hi"], ["ECHO", "x y"], ["SET", "a", "1"], ["get", "a"], ["GET", "none"],
        ["SET", "a", "2"], ["GET", "a"], ["SET", "", ""], ["GET", ""], ["SET", "bin", b"a\x00b\r\n"], ["GET", "bin"],
    ],
    "TC-DIFF-002 DEL·EXISTS 중복": [
        ["SET", "a", "1"], ["SET", "b", "1"], ["EXISTS", "a", "a", "b", "z"], ["DEL", "a", "a", "b", "z"],
        ["DEL", "a"], ["EXISTS", "a"],
    ],
    "TC-DIFF-003 SET 옵션": [
        ["SET", "k", "v", "EX", "1000"], ["TTL", "k"], ["SET", "k", "v", "PX", "500000"], ["TTL", "k"], ["PTTL", "k"],
        ["SET", "k", "w", "NX"], ["GET", "k"], ["SET", "n", "1", "XX"], ["GET", "n"], ["SET", "n", "1", "NX"],
        ["SET", "n", "2", "XX"], ["GET", "n"], ["SET", "k", "v2"], ["TTL", "k"],
        ["SET", "e", "1", "nx", "ex", "100"], ["TTL", "e"], ["SET", "e", "2", "XX", "PX", "100000"], ["GET", "e"],
    ],
    "TC-DIFF-004 SET 옵션 오류": [
        ["SET", "k", "v", "NX", "XX"], ["SET", "k", "v", "EX", "1", "PX", "1"], ["SET", "k", "v", "EX"],
        ["SET", "k", "v", "FOO"], ["SET", "k", "v", "EX", "abc"], ["SET", "k", "v", "EX", "0"],
        ["SET", "k", "v", "PX", "-5"], ["SET", "k", "v", "EX", "9223372036854775807"], ["SET", "k", "v", "EX", "1.5"],
        ["SET", "k"], ["GET"], ["GET", "a", "b"], ["GET", "k"],
    ],
    "TC-DIFF-005 만료 명령": [
        ["SET", "k", "v"], ["TTL", "k"], ["PTTL", "k"], ["TTL", "none"], ["PTTL", "none"],
        ["EXPIRE", "k", "1000"], ["TTL", "k"], ["EXPIRE", "none", "10"], ["PERSIST", "k"], ["PERSIST", "k"],
        ["TTL", "k"], ["PEXPIRE", "k", "900000"], ["TTL", "k"], ["PERSIST", "none"],
        ["SET", "z", "1"], ["EXPIRE", "z", "0"], ["EXISTS", "z"], ["SET", "z", "1"], ["PEXPIRE", "z", "-1"], ["GET", "z"],
    ],
    "TC-DIFF-006 만료 명령 오류": [
        ["SET", "k", "v"], ["EXPIRE", "k", "abc"], ["EXPIRE", "k", "1.5"], ["EXPIRE", "k", "99999999999999999999"],
        ["EXPIRE", "k", "9223372036854775807"], ["PEXPIRE", "k", "9223372036854775807"], ["EXPIRE", "k"],
        ["TTL"], ["PERSIST"], ["TTL", "k"],
    ],
    "TC-DIFF-007 KEYS 글롭": [
        ["SET", "user:1", "a"], ["SET", "user:2", "a"], ["SET", "user:10", "a"], ["SET", "item:1", "a"],
        ["SET", "a*b", "1"], ["SET", "hallo", "1"], ["SET", "hello", "1"], ["SET", "hillo", "1"],
        ["KEYS", "*"], ["KEYS", "user:*"], ["KEYS", "user:?"], ["KEYS", "h[ae]llo"], ["KEYS", "h[^e]llo"],
        ["KEYS", "h[a-f]llo"], ["KEYS", "a\\*b"], ["KEYS", "nomatch*"], ["KEYS"],
    ],
    "TC-DIFF-008 모르는 명령·인자 수": [
        # 진짜 Redis 에도 없는 이름만 쓴다(INCR·HSET 처럼 범위 밖이지만 Redis 엔 있는 명령은 비교 대상이 아니다)
        ["FOO"], ["NOSUCHCMD", "a", "b"], ["xyz", "h", "f", "v"], ["PING", "a", "b"], ["ECHO"], ["DEL"], ["EXISTS"],
        ["FLUSHALL", "ASYNC"], ["KEYS", "*"],
    ],
    "TC-DIFF-009 FLUSHALL": [["SET", "a", "1"], ["FLUSHALL"], ["KEYS", "*"], ["GET", "a"]],
}

RAW = {
    "TC-DIFF-010 프로토콜 오류: 음수 벌크": (b"*1\r\n$-5\r\n", 1),
    "TC-DIFF-011 프로토콜 오류: 숫자 아닌 벌크": (b"*1\r\n$abc\r\n", 1),
    "TC-DIFF-012 원소 수: 큰 선언은 기다림, INT_MAX 초과는 오류": (b"*1048577\r\n", 1),
    "TC-DIFF-012b 원소 수 INT_MAX 초과": (b"*2147483648\r\n", 1),
    "TC-DIFF-013 프로토콜 오류: $ 아닌 원소": (b"*1\r\n:5\r\n", 1),
    "TC-DIFF-014 프로토콜 오류: 거대한 벌크": (b"*1\r\n$999999999999\r\n", 1),
    "TC-DIFF-015 인라인 명령": (b"PING\r\nSET  a   b\r\nGET a\r\n", 3),
    "TC-DIFF-016 빈 배열·빈 인라인 줄은 건너뜀": (b"*0\r\n\r\nPING\r\n", 1),
}


def random_ops(rng, n):
    """작은 키 공간에서 무작위 명령. TTL 은 비교가 흔들리지 않게 길게"""
    keys = [f"k{i}" for i in range(8)]
    ops = []
    for _ in range(n):
        k = rng.choice(keys)
        r = rng.random()
        if r < 0.25:
            opt = rng.choice([[], ["NX"], ["XX"], ["EX", "1000"], ["PX", "800000"], ["NX", "EX", "900"]])
            ops.append(["SET", k, str(rng.randint(0, 99))] + opt)
        elif r < 0.45:
            ops.append(["GET", k])
        elif r < 0.55:
            ops.append(["DEL"] + rng.sample(keys, rng.randint(1, 3)))
        elif r < 0.65:
            ops.append(["EXISTS"] + [rng.choice(keys) for _ in range(rng.randint(1, 3))])
        elif r < 0.72:
            ops.append(["EXPIRE", k, str(rng.choice([1000, 5000, 0, -1]))])
        elif r < 0.78:
            ops.append(["PERSIST", k])
        elif r < 0.86:
            ops.append([rng.choice(["TTL", "PTTL"]), k])
        elif r < 0.94:
            ops.append(["KEYS", rng.choice(["*", "k?", "k[0-3]", "k[^5]", "x*"])])
        elif r < 0.97:
            ops.append(["PEXPIRE", k, str(rng.choice([900000, 0]))])
        else:
            ops.append(["FLUSHALL"])
    return ops


MINI_PORT, REAL_PORT = free_port(), free_port()
procs = [
    subprocess.Popen([MINI, "--port", str(MINI_PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
    subprocess.Popen([REAL, "--port", str(REAL_PORT), "--save", "", "--appendonly", "no"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
]
try:
    wait_up(MINI_PORT)
    wait_up(REAL_PORT)
    real, mini = Conn(REAL_PORT), Conn(MINI_PORT)
    real.send_raw(enc(["INFO", "server"]))
    info = real.reply()[1].decode()
    version = next(l.split(":")[1] for l in info.split("\r\n") if l.startswith("redis_version:"))
    print(f"  기준: 진짜 Redis {version}")
    if int(version.split(".")[0]) < 7:
        raise SystemExit("  FAIL 진짜 Redis 가 7 미만이다(에러 문구가 다르다). 7 이상으로 실행할 것")

    for label, seq in SCENARIOS.items():
        flush()
        run_both(label, seq)
    for label, (data, n) in RAW.items():
        raw_both(label, data, n)
    flush()
    rng = random.Random(20261114)
    run_both("TC-DIFF-017 무작위 2000개", random_ops(rng, 2000))
finally:
    for p in procs:
        p.terminate()
        p.wait(5)

for label, args, a, b in mismatches[:30]:
    print(f"  FAIL {label}: {args}\n       redis: {a!r}\n       mini : {b!r}")
print(f"  {'FAIL' if mismatches else 'ok'} ({passed} 일치 / {len(mismatches)} 불일치)")
sys.exit(1 if mismatches else 0)
