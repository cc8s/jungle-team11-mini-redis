CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -g -O2 -pthread -D_GNU_SOURCE -Isrc
LDFLAGS := -pthread

BIN      := mini-redis
SRCS     := $(wildcard src/*.c)
LIB_SRCS := $(filter-out src/main.c, $(SRCS))
LIB_OBJS := $(patsubst src/%.c, build/%.o, $(LIB_SRCS))
TESTS    := $(patsubst tests/%.c, tests/bin/%, $(wildcard tests/test_*.c))

.PHONY: all test e2e valgrind stability bench run clean

all: $(BIN)

$(BIN): build/main.o $(LIB_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -MMD -c $< -o $@

build tests/bin:
	mkdir -p $@

tests/bin/%: tests/%.c $(LIB_OBJS) | tests/bin
	$(CC) $(CFLAGS) -Itests $^ -o $@ $(LDFLAGS)

# 테스트 실행 파일을 하나씩 돌리고, 하나라도 실패하면 make 가 실패한다
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

# 실제 서버를 띄워 redis-cli · redis-py 로 확인한다 (Linux 개발 컨테이너)
e2e: $(BIN)
	bash tests/e2e.sh

# 단위 테스트를 모두 valgrind 로 돌리고, 서버를 valgrind 로 띄워 e2e 를 돌린다
VALGRIND := valgrind -q --leak-check=full --errors-for-leak-kinds=definite,indirect --error-exitcode=99
valgrind: $(TESTS) $(BIN)
	@for t in $(TESTS); do echo "== valgrind $$t"; $(VALGRIND) ./$$t >/dev/null || exit 1; done
	E2E_VALGRIND=1 bash tests/e2e.sh

# 비정상 클라이언트 · 동시 연결 · 작은 maxmemory (Linux 개발 컨테이너)
stability: $(BIN)
	bash tests/stability.sh

# 벤치마크: 지연 측정 클라이언트와 rehash 를 한 번에 끝내는 비교용 서버를 만들고 시나리오를 돌린다
BENCH_DIR := build/bench
BENCH_BLOCKING_BIN := $(BENCH_DIR)/mini-redis-blocking-rehash
BENCH_LATENCY_BIN := $(BENCH_DIR)/latency

$(BENCH_DIR):
	mkdir -p $@

$(BENCH_BLOCKING_BIN): $(SRCS) $(wildcard src/*.h) | $(BENCH_DIR)
	$(CC) $(CFLAGS) -DMINI_REDIS_BLOCKING_REHASH $(SRCS) -o $@ $(LDFLAGS)

$(BENCH_LATENCY_BIN): bench/latency.c | $(BENCH_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

bench: $(BIN) $(BENCH_BLOCKING_BIN) $(BENCH_LATENCY_BIN)
	bash bench/run.sh

run: $(BIN)
	./$(BIN)

clean:
	rm -rf build tests/bin $(BIN)

-include $(wildcard build/*.d)
