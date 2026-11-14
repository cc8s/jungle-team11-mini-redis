#!/usr/bin/env bash
# 안정성 검증: 비정상 클라이언트, 동시 연결 수백 개, 작은 maxmemory 에 대량 쓰기를 견디는지 본다.
# 사용법: make stability  (또는 bash tests/stability.sh). 하나라도 실패하면 종료 코드 1.
# 필요한 도구: bash, redis-cli, redis-benchmark, python3, timeout. Linux 전용 (/proc 로 RSS · fd 를 본다).
# 포트는 STABILITY_PORT (기본 16479) 와 그 다음 번호를 쓴다. E2E_VALGRIND=1 이면 서버를 valgrind 로 띄운다.

set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT_DIR/mini-redis"
PORT="${STABILITY_PORT:-16479}"
LIMITED_PORT=$((PORT + 1))
WORK_DIR="$(mktemp -d)"

MAX_MEMORY_BYTES=$((8 * 1024 * 1024))
FILL_KEY_COUNT=200000
FILL_VALUE_BYTES=100
BENCH_REQUEST_COUNT=100000
# RSS 는 할당기 오버헤드 · 단편화 · 연결 버퍼를 포함하므로 한도의 몇 배까지를 "근처" 로 본다
RSS_LIMIT_FACTOR=3
RSS_BASE_KB=8192

# shellcheck source=lib.sh
source "$ROOT_DIR/tests/lib.sh"
trap cleanup EXIT

server_rss_kb() {
    awk '/^VmRSS:/ {print $2}' "/proc/$(server_pid "$1")/status"
}

# 서버가 살아 있고 SET · GET 이 되는지 본다
expect_server_works() {
    local name="$1"
    local port="$2"
    if ! server_is_alive "$port"; then
        fail "$name" "서버 프로세스가 죽었습니다" "$(tail -5 "$WORK_DIR/server_$port.log")"
        return
    fi
    local reply
    reply="$(redis-cli -p "$port" SET alive yes 2>&1) $(redis-cli -p "$port" GET alive 2>&1)"
    if [ "$reply" == "OK yes" ]; then
        pass "$name"
    else
        fail "$name" "응답: $reply"
    fi
}

# 서버가 끊긴 연결을 모두 정리해 fd 수가 처음 값으로 돌아오는지 본다. 정리는 다음 이벤트에서
# 일어나므로 잠깐 기다린다
expect_fds_back_to() {
    local name="$1"
    local port="$2"
    local baseline="$3"
    local count
    for _ in $(seq 50); do
        count="$(server_fd_count "$port")"
        if [ "$count" -le "$baseline" ]; then
            pass "$name (fd $count 개)"
            return
        fi
        sleep 0.1
    done
    fail "$name" "fd 처음 $baseline 개, 지금 $count 개"
}

# 비정상 클라이언트 여러 종류를 python 으로 흉내 낸다. 끝나면 서버 쪽은 연결을 모두 닫았어야 한다
run_bad_clients() {
    python3 - "$PORT" <<'EOF'
import socket
import struct
import sys

port = int(sys.argv[1])


def connect():
    return socket.create_connection(("127.0.0.1", port))


# 1) 명령을 반만 보내고 닫는다 (RESP 와 인라인 둘 다)
for i in range(100):
    s = connect()
    s.sendall(b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nab" if i % 2 else b"SET half")
    s.close()

# 2) 아주 큰 bulk 길이만 알리고 몇 바이트 보낸 뒤 닫는다
s = connect()
s.sendall(b"*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$500000000\r\nabc")
s.close()

# 3) 반쯤 보내고 RST 로 끊는다 (SO_LINGER 0)
for _ in range(50):
    s = connect()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    s.sendall(b"*2\r\n$3\r\nGET\r\n$1")
    s.close()

# 4) 큰 응답을 잔뜩 요청하고 읽지 않은 채 끊는다. 서버는 끊긴 소켓에 쓰다가 EPIPE 를 본다
s = connect()
value = b"x" * (1024 * 1024)
s.sendall(b"*3\r\n$3\r\nSET\r\n$4\r\nhuge\r\n$%d\r\n%s\r\n" % (len(value), value))
assert s.recv(5) == b"+OK\r\n"
s.sendall(b"*2\r\n$3\r\nGET\r\n$4\r\nhuge\r\n" * 50)
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
s.close()

# 5) 연결만 맺고 아무것도 보내지 않은 채 닫는다
for _ in range(100):
    connect().close()
print("bad clients done")
EOF
}

test_bad_clients() {
    echo "15.2 비정상 클라이언트"
    local baseline
    baseline="$(server_fd_count "$PORT")"
    if run_bad_clients >"$WORK_DIR/bad_clients.log" 2>&1; then
        pass "반만 보내기 · 큰 길이 · RST · 응답 안 읽기 · 빈 연결 351개"
    else
        fail "비정상 클라이언트 실행" "$(tail -5 "$WORK_DIR/bad_clients.log")"
    fi
    expect_server_works "그 뒤에도 서버가 SET · GET 에 응답" "$PORT"
    expect_fds_back_to "끊긴 연결을 모두 정리" "$PORT" "$baseline"
}

# redis-benchmark 는 시작할 때 CONFIG GET 을 보내는데 이 서버에는 없는 명령이라 ERROR 를 찍고
# 기본값으로 계속한다. 그 줄은 빼고 남은 에러가 있는지 본다
has_benchmark_error() {
    tr '\r' '\n' <"$1" | grep -i 'error' | grep -qv 'CONFIG'
}

# redis-benchmark 는 진행 상황을 \r 로 덮어쓰므로 마지막 결과 줄만 남긴다
run_benchmark() {
    local log="$1"
    shift
    timeout 300 redis-benchmark -p "$PORT" -n "$BENCH_REQUEST_COUNT" -t set,get -q "$@" \
        >"$log" 2>&1
    local status=$?
    tr '\r' '\n' <"$log" | grep 'requests per second' >"$log.summary"
    return "$status"
}

test_many_clients() {
    echo "15.3 동시 연결"
    local baseline
    baseline="$(server_fd_count "$PORT")"
    local clients
    for clients in 200 500; do
        local log="$WORK_DIR/bench_$clients.log"
        if run_benchmark "$log" -c "$clients" && [ "$(wc -l <"$log.summary")" -eq 2 ] &&
            ! has_benchmark_error "$log"; then
            pass "redis-benchmark -c $clients ($(paste -sd ' ' "$log.summary"))"
        else
            fail "redis-benchmark -c $clients" "$(tr '\r' '\n' <"$log" | tail -5)"
        fi
    done
    local log="$WORK_DIR/bench_pipeline.log"
    if run_benchmark "$log" -c 50 -P 16 && [ "$(wc -l <"$log.summary")" -eq 2 ] &&
        ! has_benchmark_error "$log"; then
        pass "redis-benchmark -c 50 -P 16 ($(paste -sd ' ' "$log.summary"))"
    else
        fail "redis-benchmark -c 50 -P 16" "$(tr '\r' '\n' <"$log" | tail -5)"
    fi
    expect_server_works "벤치마크 뒤에도 서버가 응답" "$PORT"
    expect_fds_back_to "벤치마크 연결을 모두 정리" "$PORT" "$baseline"
}

# 한도의 몇 배를 쓰고, 응답 · 앞 키 · 뒤 키 상태를 JSON 한 줄로 출력한다
fill_limited_server() {
    python3 - "$LIMITED_PORT" "$FILL_KEY_COUNT" "$FILL_VALUE_BYTES" <<'EOF'
import json
import socket
import sys

port, key_count, value_bytes = (int(arg) for arg in sys.argv[1:4])
s = socket.create_connection(("127.0.0.1", port))
reader = s.makefile("rb")
value = b"v" * value_bytes
batch = 1000
replies = {"ok": 0, "oom": 0, "other": 0}
for start in range(0, key_count, batch):
    end = min(key_count, start + batch)
    s.sendall(b"".join(
        b"*3\r\n$3\r\nSET\r\n$%d\r\nfill:%d\r\n$%d\r\n%s\r\n" % (len(b"fill:%d" % i), i, value_bytes, value)
        for i in range(start, end)))
    for _ in range(start, end):
        line = reader.readline()
        if line == b"+OK\r\n":
            replies["ok"] += 1
        elif line.startswith(b"-OOM"):
            replies["oom"] += 1
        else:
            replies["other"] += 1


def present_count(numbers):
    s.sendall(b"".join(b"*2\r\n$3\r\nGET\r\n$%d\r\nfill:%d\r\n" % (len(b"fill:%d" % i), i) for i in numbers))
    found = 0
    for _ in numbers:
        line = reader.readline()
        if line != b"$-1\r\n":
            reader.readline()
            found += 1
    return found


replies["first_present"] = present_count(range(1000))
replies["last_present"] = present_count(range(key_count - 100, key_count))
print(json.dumps(replies))
EOF
}

json_field() {
    python3 -c 'import json, sys; print(json.loads(sys.argv[1])[sys.argv[2]])' "$1" "$2"
}

test_small_maxmemory() {
    echo "15.4 작은 maxmemory 에 대량 쓰기"
    local result
    if ! result="$(fill_limited_server 2>&1)"; then
        fail "키 $FILL_KEY_COUNT 개 쓰기" "$result"
        return
    fi
    local ok_count oom_count first_present last_present
    ok_count="$(json_field "$result" ok)"
    oom_count="$(json_field "$result" oom)"
    first_present="$(json_field "$result" first_present)"
    last_present="$(json_field "$result" last_present)"
    if [ "$ok_count" -eq "$FILL_KEY_COUNT" ] && [ "$oom_count" -eq 0 ]; then
        pass "작은 값은 퇴출로 자리를 만들어 SET 이 모두 OK ($ok_count 개)"
    else
        fail "SET 이 모두 OK" "$result"
    fi
    if [ "$first_present" -lt 100 ] && [ "$last_present" -ge 90 ]; then
        pass "오래된 키는 퇴출되고 최근 키는 남음 (앞 1000개 중 $first_present, 뒤 100개 중 $last_present)"
    else
        fail "오래된 키 퇴출" "$result"
    fi
    local rss_kb rss_bound_kb
    rss_kb="$(server_rss_kb "$LIMITED_PORT")"
    rss_bound_kb=$((MAX_MEMORY_BYTES * RSS_LIMIT_FACTOR / 1024 + RSS_BASE_KB))
    if [ "${E2E_VALGRIND:-0}" == "1" ]; then
        pass "RSS ${rss_kb}KB (valgrind 모드라 한도 비교는 건너뜀)"
    elif [ "$rss_kb" -le "$rss_bound_kb" ]; then
        pass "RSS ${rss_kb}KB 가 한도 $((MAX_MEMORY_BYTES / 1024))KB 근처 (상한 ${rss_bound_kb}KB)"
    else
        fail "RSS 가 한도 근처" "RSS ${rss_kb}KB > 상한 ${rss_bound_kb}KB"
    fi
    local oom_reply
    oom_reply="$(head -c $((MAX_MEMORY_BYTES * 2)) /dev/zero | tr '\0' 'x' |
        redis-cli -p "$LIMITED_PORT" -x SET too_big 2>&1)"
    if [[ "$oom_reply" == OOM* ]]; then
        pass "한도보다 큰 값 하나는 OOM 으로 거절"
    else
        fail "한도보다 큰 값은 OOM" "응답: $oom_reply"
    fi
    expect_server_works "그 뒤에도 서버가 응답" "$LIMITED_PORT"
}

main() {
    require_tools redis-cli redis-benchmark python3 timeout
    start_server "$PORT"
    start_server "$LIMITED_PORT" MINI_REDIS_MAXMEMORY="$MAX_MEMORY_BYTES"

    test_bad_clients
    test_many_clients
    test_small_maxmemory
    echo "종료"
    stop_all_servers

    finish stability
}

main
