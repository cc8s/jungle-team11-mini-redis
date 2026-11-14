#!/usr/bin/env bash
# 인수 테스트: 실제 mini-redis 를 띄우고 redis-cli · 원시 소켓 · redis-py 로 확인한다.
# 사용법: make e2e  (또는 bash tests/e2e.sh). 하나라도 실패하면 종료 코드 1.
# 필요한 도구: bash, redis-cli, python3, timeout, cmp. redis-py 가 없으면 build/e2e-venv 에 설치한다.
# 포트는 E2E_PORT (기본 16379) 와 그 다음 번호를 쓴다. E2E_VALGRIND=1 이면 서버를 valgrind 로 띄운다.
# RESP 요청의 $ 는 셸 변수가 아니라 글자 그대로 보내야 하므로 작은따옴표를 쓴다.
# shellcheck disable=SC2016

set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT_DIR/mini-redis"
PORT="${E2E_PORT:-16379}"
NO_FLUSH_PORT=$((PORT + 1))
VENV_DIR="$ROOT_DIR/build/e2e-venv"
WORK_DIR="$(mktemp -d)"
TTL_WAIT_SEC=1.5

# shellcheck source=lib.sh
source "$ROOT_DIR/tests/lib.sh"
trap cleanup EXIT

cli() {
    redis-cli -p "$PORT" "$@" 2>&1
}

# redis-cli 결과가 기대값과 같은지 본다
expect_cli() {
    local name="$1"
    local expected="$2"
    shift 2
    local actual
    actual="$("$@" 2>&1)"
    if [ "$actual" == "$expected" ]; then
        pass "$name"
    else
        fail "$name" "기대: $expected" "실제: $actual"
    fi
}

# 원시 바이트를 보내고 정확히 기대한 바이트를 받는지 본다. printf %b 형식(\r\n)으로 적는다.
# nc 는 개발 컨테이너에 없어서 bash 의 /dev/tcp 를 쓴다
expect_raw() {
    local name="$1"
    local request="$2"
    local expected="$3"
    printf '%b' "$expected" >"$WORK_DIR/expected"
    local want_bytes
    want_bytes="$(wc -c <"$WORK_DIR/expected")"
    if ! exec 3<>"/dev/tcp/127.0.0.1/$PORT"; then
        fail "$name" "연결 실패"
        return
    fi
    printf '%b' "$request" >&3
    timeout 2 head -c "$want_bytes" <&3 >"$WORK_DIR/actual"
    exec 3<&-
    if cmp -s "$WORK_DIR/expected" "$WORK_DIR/actual"; then
        pass "$name"
    else
        fail "$name" "기대: $(od -c "$WORK_DIR/expected" | head -3)" \
            "실제: $(od -c "$WORK_DIR/actual" | head -3)"
    fi
}

# 깨진 요청 뒤 서버가 연결을 닫는지 본다. 에러를 읽은 뒤 EOF 가 와야 한다
expect_closed_after_error() {
    local name="$1"
    local request="$2"
    exec 3<>"/dev/tcp/127.0.0.1/$PORT"
    printf '%b' "$request" >&3
    timeout 2 cat <&3 >"$WORK_DIR/actual"
    local status=$?
    exec 3<&-
    if [ "$status" -eq 0 ] && grep -q "Protocol error" "$WORK_DIR/actual"; then
        pass "$name"
    else
        fail "$name" "cat 종료 코드 $status (124 면 서버가 닫지 않음)" "받은 것: $(cat "$WORK_DIR/actual")"
    fi
}

test_basic_commands() {
    echo "14.2 기본 명령"
    expect_cli "SET 은 OK" "OK" cli SET greeting hello
    expect_cli "GET 은 값" "hello" cli GET greeting
    expect_cli "DEL 은 지운 수 1" "1" cli DEL greeting
    expect_cli "지운 키 GET 은 nil" "" cli GET greeting
    expect_cli "없는 키 DEL 은 0" "0" cli DEL greeting
    cli SET a 1 >/dev/null
    cli SET b 2 >/dev/null
    expect_cli "DEL 여러 키는 지운 수 합계" "2" cli DEL a b missing
    expect_cli "소문자 명령도 받음" "OK" cli set lower case
}

test_expire() {
    echo "14.3 · 14.4 만료"
    expect_cli "SET EX 는 OK" "OK" cli SET temp v EX 1
    expect_cli "만료 전 GET 은 값" "v" cli GET temp
    cli SET kept old EX 1 >/dev/null
    expect_cli "EX 없이 덮어쓰기" "OK" cli SET kept new
    sleep "$TTL_WAIT_SEC"
    expect_cli "1.5초 뒤 만료된 키는 nil" "" cli GET temp
    expect_cli "EX 없이 덮어쓴 키는 TTL 이 지워져 남음" "new" cli GET kept
}

test_flushall() {
    echo "14.5 전체 삭제"
    cli SET f1 1 >/dev/null
    cli SET f2 2 >/dev/null
    expect_cli "활성 서버의 FLUSHALL 은 OK" "OK" cli FLUSHALL
    expect_cli "FLUSHALL 뒤 키는 nil" "" cli GET f1
    redis-cli -p "$NO_FLUSH_PORT" SET keep 1 >/dev/null
    expect_cli "비활성 서버의 FLUSHALL 은 에러" "ERR FLUSHALL is disabled" \
        redis-cli -p "$NO_FLUSH_PORT" FLUSHALL
    expect_cli "비활성 서버의 키는 그대로" "1" redis-cli -p "$NO_FLUSH_PORT" GET keep
}

test_errors() {
    echo "14.6 에러"
    expect_cli "알 수 없는 명령" "ERR unknown command 'NOPE'" cli NOPE
    expect_cli "인자 수 오류" "ERR wrong number of arguments for 'get' command" cli GET
    expect_cli "잘못된 EX (0)" "ERR invalid expire time in 'set' command" cli SET k v EX 0
    expect_cli "잘못된 EX (숫자 아님)" "ERR invalid expire time in 'set' command" \
        cli SET k v EX abc
    expect_cli "모르는 SET 옵션" "ERR syntax error" cli SET k v PX 10
    expect_raw "깨진 RESP 는 프로토콜 에러" '*x\r\n' '-ERR Protocol error: invalid request\r\n'
    expect_closed_after_error "프로토콜 에러 뒤 연결을 닫음" '*1\r\n$abc\r\n'
    expect_raw "인라인 명령" 'SET inline yes\r\nGET inline\r\n' '+OK\r\n$3\r\nyes\r\n'
}

test_binary_value() {
    echo "14.7 바이너리 값"
    head -c 200000 /dev/urandom >"$WORK_DIR/blob"
    printf 'a\r\nb\0c\r\n' >>"$WORK_DIR/blob"
    expect_cli "redis-cli -x SET" "OK" cli -x SET blob <"$WORK_DIR/blob"
    # redis-cli 는 값을 출력한 뒤 줄바꿈을 하나 붙이므로 마지막 1바이트를 떼고 비교한다
    cli GET blob | head -c -1 >"$WORK_DIR/blob_out"
    if cmp -s "$WORK_DIR/blob" "$WORK_DIR/blob_out"; then
        pass "GET 이 같은 바이트를 돌려줌 (CR LF NUL 포함 200KB)"
    else
        fail "GET 이 같은 바이트를 돌려줌" "$(cmp "$WORK_DIR/blob" "$WORK_DIR/blob_out")"
    fi
}

test_pipelining() {
    echo "14.8 파이프라이닝"
    expect_raw "명령 여러 개를 한 번에 보내도 순서대로 응답" \
        '*3\r\n$3\r\nSET\r\n$1\r\np\r\n$1\r\n1\r\n*2\r\n$3\r\nGET\r\n$1\r\np\r\n*2\r\n$3\r\nDEL\r\n$1\r\np\r\n*2\r\n$3\r\nGET\r\n$1\r\np\r\n' \
        '+OK\r\n$1\r\n1\r\n:1\r\n$-1\r\n'
    local count=1000
    local i
    for ((i = 0; i < count; i++)); do
        printf 'SET pipe:%d %d\n' "$i" "$i"
    done >"$WORK_DIR/pipe_cmds"
    redis-cli -p "$PORT" <"$WORK_DIR/pipe_cmds" >"$WORK_DIR/pipe_out" 2>&1
    local ok_count
    ok_count="$(grep -c '^OK$' "$WORK_DIR/pipe_out")"
    if [ "$ok_count" -eq "$count" ] && [ "$(cli GET pipe:999)" == "999" ]; then
        pass "redis-cli 로 SET $count 개 연속"
    else
        fail "redis-cli 로 SET $count 개 연속" "OK $ok_count 개"
    fi
}

# redis-py 가 없으면 build/ 아래 venv 에 설치한다 (공유 개발 컨테이너 이미지는 건드리지 않는다)
find_python_with_redis() {
    if python3 -c 'import redis' 2>/dev/null; then
        echo python3
        return 0
    fi
    if [ ! -x "$VENV_DIR/bin/python" ]; then
        python3 -m venv "$VENV_DIR" >/dev/null 2>&1 || return 1
    fi
    if ! "$VENV_DIR/bin/python" -c 'import redis' 2>/dev/null; then
        "$VENV_DIR/bin/pip" install -q redis >/dev/null 2>&1 || return 1
    fi
    echo "$VENV_DIR/bin/python"
}

test_redis_py() {
    echo "14.9 redis-py"
    local python
    if ! python="$(find_python_with_redis)"; then
        fail "redis-py 준비" "python3 -m venv 또는 pip install redis 실패 (네트워크 확인)"
        return
    fi
    if "$python" - "$PORT" <<'EOF' >"$WORK_DIR/py_out" 2>&1; then
import sys
import time

import redis

# redis-py 8 부터 기본이 RESP3 라 접속 때 HELLO 3 을 보낸다. 이 서버는 RESP2 만 지원하므로 명시한다
r = redis.Redis(host="127.0.0.1", port=int(sys.argv[1]), protocol=2)
assert r.set("py:k", "v") is True
assert r.get("py:k") == b"v"
assert r.delete("py:k") == 1
assert r.get("py:k") is None
assert r.delete("py:k") == 0

blob = bytes(range(256)) * 10
assert r.set("py:bin", blob) is True
assert r.get("py:bin") == blob

assert r.set("py:ttl", "x", ex=1) is True
time.sleep(1.5)
assert r.get("py:ttl") is None

pipe = r.pipeline(transaction=False)
for i in range(100):
    pipe.set(f"py:p{i}", i)
pipe.get("py:p99")
results = pipe.execute()
assert results[-1] == b"99" and all(results[:-1])

try:
    r.execute_command("NOPE")
    raise AssertionError("unknown command did not raise")
except redis.ResponseError as err:
    assert "unknown command" in str(err)
print("redis-py", redis.__version__)
EOF
        pass "set / get / delete / ex / 바이너리 / 파이프라인 / 에러 ($(tail -1 "$WORK_DIR/py_out"))"
    else
        fail "redis-py" "$(tail -5 "$WORK_DIR/py_out")"
    fi
}

test_graceful_shutdown() {
    echo "종료"
    stop_all_servers
}

main() {
    require_tools redis-cli python3 timeout cmp
    start_server "$PORT" MINI_REDIS_ENABLE_FLUSH=1
    start_server "$NO_FLUSH_PORT"

    test_basic_commands
    test_expire
    test_flushall
    test_errors
    test_binary_value
    test_pipelining
    test_redis_py
    test_graceful_shutdown

    finish e2e
}

main
