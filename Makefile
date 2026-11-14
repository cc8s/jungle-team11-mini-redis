CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -g -O2 -pthread -D_GNU_SOURCE -Isrc $(EXTRA_CFLAGS)
LDFLAGS := -pthread $(EXTRA_LDFLAGS)

# 메모리 오류(AddressSanitizer)와 정의되지 않은 동작(UBSan)을 잡는다. 하나라도 걸리면 실패
SAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

# 목적 파일·테스트 실행 파일 위치. asan 빌드는 따로 둬서 일반 빌드와 섞이지 않게 한다
BUILD ?= build
TBIN  ?= tests/bin

BIN      := mini-redis
SRCS     := $(wildcard src/*.c)
LIB_SRCS := $(filter-out src/main.c, $(SRCS))
LIB_OBJS := $(patsubst src/%.c, $(BUILD)/%.o, $(LIB_SRCS))
TESTS    := $(patsubst tests/%.c, $(TBIN)/%, $(wildcard tests/test_*.c))

.PHONY: all test asan run clean

all: $(BIN)

$(BIN): $(BUILD)/main.o $(LIB_OBJS)
	$(CC) $^ -o $@ $(LDFLAGS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

$(BUILD) $(TBIN):
	mkdir -p $@

$(TBIN)/%: tests/%.c $(LIB_OBJS) | $(TBIN)
	$(CC) $(CFLAGS) -Itests $^ -o $@ $(LDFLAGS)

# 테스트 실행 파일을 하나씩 돌리고, 하나라도 실패하면 make 가 실패한다
test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

asan:
	$(MAKE) test BUILD=build/asan TBIN=tests/bin/asan EXTRA_CFLAGS="$(SAN)" EXTRA_LDFLAGS="$(SAN)"

run: $(BIN)
	./$(BIN)

clean:
	rm -rf build tests/bin $(BIN)

-include $(wildcard $(BUILD)/*.d)
