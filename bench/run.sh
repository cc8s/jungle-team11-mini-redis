#!/usr/bin/env bash
# 벤치마크: make bench 로 실행한다 (Linux 개발 컨테이너).
#   16.1 redis-benchmark 시나리오: GET 위주, SET 위주, SET ... EX 와 GET 혼합
#   16.2 점진적 rehash 유무에 따른 지연: 같은 키 수를 하나씩 SET 하며 요청별 왕복 지연을 잰다
# redis-server 가 설치돼 있으면 16.1 을 실제 Redis 에도 돌려 참고값으로 보여 준다.
# 환경변수: BENCH_PORT(기본 16579), BENCH_REQUESTS(500000), BENCH_CLIENTS(50),
#           BENCH_KEYSPACE(100000), BENCH_LATENCY_KEYS(1100000), BENCH_REPEAT(3)
# 컨테이너 · 노트북은 잡음이 커서 처리량은 BENCH_REPEAT 번 돌린 중앙값(그 회차의 p99)을 쓴다

set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT_DIR/mini-redis"
BLOCKING_BIN="$ROOT_DIR/build/bench/mini-redis-blocking-rehash"
LATENCY_BIN="$ROOT_DIR/build/bench/latency"
WORK_DIR="$(mktemp -d)"
PORT="${BENCH_PORT:-16579}"
REQUESTS="${BENCH_REQUESTS:-200000}"
REPEAT="${BENCH_REPEAT:-3}"
CLIENTS="${BENCH_CLIENTS:-50}"
KEYSPACE="${BENCH_KEYSPACE:-100000}"
LATENCY_KEYS="${BENCH_LATENCY_KEYS:-1100000}"
PIPELINE=16
EXPIRE_SEC=1

# shellcheck source=../tests/lib.sh
source "$ROOT_DIR/tests/lib.sh"
trap cleanup EXIT

# redis-benchmark 출력에서 처리량과 p99 를 뽑는다. 6.x 는 "99.12% <= 1 milliseconds" 누적 분포,
# 7.x 는 "latency summary" 표를 찍으므로 둘 다 읽는다. 6.x 의 p99 는 ms 단위로 거칠다
summarize_benchmark() {
    tr '\r' '\n' <"$1" | awk '
        /requests per second/ { for (i = 1; i <= NF; i++) if ($i ~ /^[0-9.]+$/) rps = $i }
        /% <= / && p99 == "" { pct = $1; sub("%", "", pct); if (pct + 0 >= 99) p99 = "<= " $3 " ms" }
        /latency summary/ { getline; getline; p99 = $5 " ms" }
        END { printf "%s | %s", rps, p99 }'
}

# 같은 측정을 REPEAT 번 하고, 처리량이 중앙값인 회차의 "처리량 | p99" 를 출력한다.
# 인자: 로그 접두어, 그 뒤는 redis-benchmark 인자
bench_median() {
    local log_prefix="$1"
    shift
    local round
    for ((round = 1; round <= REPEAT; round++)); do
        redis-benchmark "$@" >"$log_prefix.$round" 2>&1
        summarize_benchmark "$log_prefix.$round"
        echo
    done | sort -t'|' -k1 -n | sed -n "$(((REPEAT + 1) / 2))p"
}

# 표 한 줄: | 대상 | 시나리오 | 조건 | 처리량 | p99 |
bench_row() {
    echo "| $1 | $2 | $3 | $4 |"
}

run_scenarios() {
    local target="$1"
    local port="$2"
    local common=(-p "$port" -n "$REQUESTS" -r "$KEYSPACE" -c "$CLIENTS")
    local log="$WORK_DIR/${target// /_}"

    redis-benchmark -p "$port" -n "$KEYSPACE" -r "$KEYSPACE" -t set -q >/dev/null 2>&1
    bench_row "$target" "GET 위주" "c=$CLIENTS" "$(bench_median "$log.get" "${common[@]}" -t get)"
    bench_row "$target" "GET 위주" "c=$CLIENTS P=$PIPELINE" \
        "$(bench_median "$log.get_p" "${common[@]}" -t get -P "$PIPELINE")"
    bench_row "$target" "SET 위주" "c=$CLIENTS" "$(bench_median "$log.set" "${common[@]}" -t set)"
    bench_row "$target" "SET 위주" "c=$CLIENTS P=$PIPELINE" \
        "$(bench_median "$log.set_p" "${common[@]}" -t set -P "$PIPELINE")"

    # SET EX 와 GET 을 동시에 돌려, 만료 정리가 읽기와 섞이는 상황을 만든다 (회차마다 동시 실행)
    local round
    for ((round = 1; round <= REPEAT; round++)); do
        redis-benchmark "${common[@]}" SET 'key:__rand_int__' xxx EX "$EXPIRE_SEC" \
            >"$log.mix_set.$round" 2>&1 &
        local set_pid=$!
        redis-benchmark "${common[@]}" GET 'key:__rand_int__' >"$log.mix_get.$round" 2>&1
        wait "$set_pid"
    done
    bench_row "$target" "혼합: SET EX $EXPIRE_SEC" "c=$CLIENTS, GET 과 동시" \
        "$(median_of_logs "$log.mix_set")"
    bench_row "$target" "혼합: GET" "c=$CLIENTS, SET EX 와 동시" "$(median_of_logs "$log.mix_get")"
}

median_of_logs() {
    local round
    for ((round = 1; round <= REPEAT; round++)); do
        summarize_benchmark "$1.$round"
        echo
    done | sort -t'|' -k1 -n | sed -n "$(((REPEAT + 1) / 2))p"
}

# 첫 줄은 전체 분포, 둘째 줄은 리사이즈가 시작되는 요청들의 지연이다 (bench/latency.c)
run_latency() {
    local name="$1"
    local port="$2"
    "$LATENCY_BIN" "$port" "$LATENCY_KEYS" >"$WORK_DIR/latency_$port" || return 1
    local summary resize_points
    resize_points="$(sed -n 1p "$WORK_DIR/latency_$port" | cut -d' ' -f2-)"
    summary="$(sed -n 2p "$WORK_DIR/latency_$port")"
    local field
    local row="| $name |"
    for field in p50_us p99_us p999_us max_us over_10ms; do
        row="$row $(echo "$summary" | tr ' ' '\n' | awk -F= -v k="$field" '$1 == k { print $2 }') |"
    done
    row="$row $(echo "$resize_points" | sed 's/=/: /g; s/ \([0-9]*\):/, \1:/g') |"
    echo "$row"
}

start_real_redis() {
    redis-server --port "$1" --save '' --appendonly no --daemonize no \
        >"$WORK_DIR/redis-server.log" 2>&1 &
    SERVER_PID_BY_PORT[$1]=$!
    for _ in $(seq 50); do
        redis-cli -p "$1" PING >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    return 1
}

main() {
    require_tools redis-cli redis-benchmark
    for bin in "$BLOCKING_BIN" "$LATENCY_BIN"; do
        if [ ! -x "$bin" ]; then
            echo "$bin 이 없습니다. make bench 로 실행하세요"
            exit 1
        fi
    done
    echo "환경: $(nproc) CPU, $(uname -sr), requests=$REQUESTS keyspace=$KEYSPACE repeat=$REPEAT"
    echo
    echo "## 16.1 처리량"
    echo
    echo "| 대상 | 시나리오 | 조건 | 처리량 (req/s) | p99 |"
    echo "|---|---|---|---|---|"
    start_server "$PORT"
    run_scenarios mini-redis "$PORT"
    stop_server "$PORT" >/dev/null
    if command -v redis-server >/dev/null && start_real_redis "$((PORT + 1))"; then
        run_scenarios "Redis $(redis-server --version | awk '{print $3}' | cut -d= -f2)" \
            "$((PORT + 1))"
    fi

    echo
    echo "## 16.2 rehash 방식별 SET 왕복 지연 (키 $LATENCY_KEYS 개를 하나씩)"
    echo
    echo "| 서버 | p50 µs | p99 µs | p99.9 µs | 최대 µs | 10ms 이상 | 리사이즈 시작 요청(번째: µs) |"
    echo "|---|---|---|---|---|---|---|"
    start_server "$((PORT + 2))"
    run_latency "점진적 rehash (기본)" "$((PORT + 2))"
    stop_server "$((PORT + 2))" >/dev/null
    local default_bin="$BIN"
    BIN="$BLOCKING_BIN"
    start_server "$((PORT + 3))"
    run_latency "한 번에 rehash (비교용)" "$((PORT + 3))"
    stop_server "$((PORT + 3))" >/dev/null
    BIN="$default_bin"
    # 실제 Redis 도 같은 클라이언트로 재서, 리사이즈와 무관한 최대값이 환경 잡음인지 가늠한다
    if command -v redis-server >/dev/null && start_real_redis "$((PORT + 4))"; then
        run_latency "Redis (참고)" "$((PORT + 4))"
    fi
}

main
