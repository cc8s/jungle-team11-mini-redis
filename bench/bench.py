"""캐시 유무 성능 비교 (과제 품질 요구사항: 레디스를 쓸 때와 안 쓸 때).

mini-redis 와 데모 API(demo/app.py)를 띄우고, 같은 요청을 캐시 우회(?cache=0)와 캐시 경유로 보낸다.
사용법: python3 bench/bench.py ./mini-redis [--requests 3000] [--concurrency 20] [--users 100] [--delay-ms 100]
결과는 표준 출력에 마크다운 표로 쓴다.
"""
import argparse
import os
import platform
import random
import socket
import statistics
import subprocess
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor

ap = argparse.ArgumentParser()
ap.add_argument("mini")
ap.add_argument("--requests", type=int, default=3000)
ap.add_argument("--concurrency", type=int, default=20)
ap.add_argument("--users", type=int, default=100)
ap.add_argument("--delay-ms", type=int, nargs="+", default=[100, 10])
a = ap.parse_args()


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


def one(url):
    t0 = time.perf_counter()
    with urllib.request.urlopen(url, timeout=30) as r:
        r.read()
        hit = r.headers.get("X-Cache") == "HIT"
    return time.perf_counter() - t0, hit


def run(base, suffix, ids):
    with ThreadPoolExecutor(a.concurrency) as ex:
        t0 = time.perf_counter()
        res = list(ex.map(lambda i: one(f"{base}/users/{i}{suffix}"), ids))
        wall = time.perf_counter() - t0
    lat = sorted(x for x, _ in res)
    q = lambda p: lat[min(len(lat) - 1, int(len(lat) * p))] * 1000  # noqa: E731
    return {"rps": len(lat) / wall, "p50": q(0.50), "p95": q(0.95), "p99": q(0.99),
            "mean": statistics.mean(lat) * 1000, "hit": sum(h for _, h in res) / len(res)}


rows = []
procs = []
try:
    for delay in a.delay_ms:
        rport, port = free_port(), free_port()
        procs.append(subprocess.Popen([a.mini, "--port", str(rport)], stdout=subprocess.DEVNULL))
        wait_port(rport)
        procs.append(subprocess.Popen([sys.executable, "demo/app.py", "--port", str(port), "--redis-port", str(rport),
                                       "--delay-ms", str(delay), "--ttl", "600"], stdout=subprocess.DEVNULL))
        wait_port(port)
        base = f"http://127.0.0.1:{port}"
        rng = random.Random(1)
        ids = [rng.randint(1, a.users) for _ in range(a.requests)]
        rows.append((delay, "캐시 없음 (원본만)", run(base, "?cache=0", ids)))
        rows.append((delay, "mini-redis 캐시 (빈 상태에서 시작)", run(base, "", ids)))
        rows.append((delay, "mini-redis 캐시 (모두 적중)", run(base, "", ids)))
        for p in procs:
            p.terminate()
            p.wait(5)
        procs.clear()
finally:
    for p in procs:
        p.kill()

print(f"요청 {a.requests}개, 동시 {a.concurrency}, 사용자 {a.users}명 무작위, "
      f"{platform.system()} {platform.release()}, CPU {os.cpu_count()}개\n")
print("| 원본 지연 | 방식 | 처리량(req/s) | 평균 | p50 | p95 | p99 | 캐시 적중률 |")
print("|---|---|---|---|---|---|---|---|")
for delay, name, r in rows:
    print(f"| {delay}ms | {name} | {r['rps']:.0f} | {r['mean']:.1f}ms | {r['p50']:.1f}ms | {r['p95']:.1f}ms | "
          f"{r['p99']:.1f}ms | {r['hit'] * 100:.0f}% |")
