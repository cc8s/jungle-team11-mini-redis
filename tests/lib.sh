# e2e.sh · stability.sh 가 같이 쓰는 함수: 결과 집계, 서버 시작 · 종료, 도구 확인.
# 쓰는 쪽이 source 하기 전에 BIN 과 WORK_DIR 을 정한다.
# E2E_VALGRIND=1 이면 서버를 valgrind 로 띄우고, 종료할 때 오류 · 누수가 있으면 실패로 센다.
# shellcheck shell=bash

PASS_COUNT=0
FAIL_COUNT=0
SERVER_PORTS=()
declare -A SERVER_PID_BY_PORT

# valgrind 는 오류나 definite · indirect 누수가 있으면 이 코드로 끝난다
VALGRIND_ERROR_EXIT_CODE=99
if [ "${E2E_VALGRIND:-0}" == "1" ]; then
    START_WAIT_TRIES="${E2E_START_WAIT_TRIES:-300}"
else
    START_WAIT_TRIES="${E2E_START_WAIT_TRIES:-50}"
fi

pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "  ok   $1"
}

fail() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "  FAIL $1"
    shift
    for line in "$@"; do
        echo "       $line"
    done
}

require_tools() {
    for tool in "$@"; do
        if ! command -v "$tool" >/dev/null; then
            echo "$tool 이 필요합니다"
            exit 1
        fi
    done
    if [ ! -x "$BIN" ]; then
        echo "$BIN 이 없습니다. make 를 먼저 실행하세요"
        exit 1
    fi
}

server_command() {
    local port="$1"
    if [ "${E2E_VALGRIND:-0}" == "1" ]; then
        echo valgrind --leak-check=full --errors-for-leak-kinds=definite,indirect \
            --error-exitcode="$VALGRIND_ERROR_EXIT_CODE" --log-file="$WORK_DIR/valgrind_$port.log"
    fi
}

# 서버를 띄우고 응답할 때까지 기다린다. 환경변수는 "이름=값" 으로 넘긴다
start_server() {
    local port="$1"
    shift
    local wrapper
    read -r -a wrapper <<<"$(server_command "$port")"
    env "$@" MINI_REDIS_PORT="$port" "${wrapper[@]}" "$BIN" >"$WORK_DIR/server_$port.log" 2>&1 &
    SERVER_PID_BY_PORT[$port]=$!
    SERVER_PORTS+=("$port")
    for _ in $(seq "$START_WAIT_TRIES"); do
        if redis-cli -p "$port" GET __ready__ >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    echo "서버가 포트 $port 에서 시작하지 않았습니다:"
    cat "$WORK_DIR/server_$port.log"
    exit 1
}

server_pid() {
    echo "${SERVER_PID_BY_PORT[$1]}"
}

server_is_alive() {
    kill -0 "$(server_pid "$1")" 2>/dev/null
}

# 열린 fd 수. 연결을 닫은 뒤 처음 값으로 돌아오는지 보는 데 쓴다 (Linux /proc)
server_fd_count() {
    find "/proc/$(server_pid "$1")/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l
}

# SIGTERM 으로 끝내고 종료 코드 0 인지 본다. valgrind 모드면 오류 · 누수도 여기서 드러난다
stop_server() {
    local port="$1"
    local pid
    pid="$(server_pid "$port")"
    kill -TERM "$pid"
    wait "$pid"
    local status=$?
    unset "SERVER_PID_BY_PORT[$port]"
    local name="포트 $port 서버가 SIGTERM 에 종료 코드 0"
    if [ "${E2E_VALGRIND:-0}" == "1" ]; then
        name="$name (valgrind 오류 · 누수 0)"
    fi
    if [ "$status" -eq 0 ]; then
        pass "$name"
    elif [ -f "$WORK_DIR/valgrind_$port.log" ]; then
        fail "$name" "종료 코드 $status" "$(tail -30 "$WORK_DIR/valgrind_$port.log")"
    else
        fail "$name" "종료 코드 $status" "$(tail -5 "$WORK_DIR/server_$port.log")"
    fi
}

stop_all_servers() {
    for port in "${SERVER_PORTS[@]}"; do
        if [ -n "${SERVER_PID_BY_PORT[$port]:-}" ]; then
            stop_server "$port"
        fi
    done
}

# 중간에 끝나도 서버가 남지 않게 trap 에서 부른다
cleanup() {
    for pid in "${SERVER_PID_BY_PORT[@]}"; do
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    done
    rm -rf "$WORK_DIR"
}

finish() {
    echo "$1: 통과 $PASS_COUNT, 실패 $FAIL_COUNT"
    [ "$FAIL_COUNT" -eq 0 ]
}
