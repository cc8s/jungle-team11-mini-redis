# C 스타일 가이드

이 프로젝트의 모든 C 코드가 따르는 규칙이다. Linux 커널 스타일과 Redis 소스의 관례를 기반으로 하고, 클린코드 원칙을 C에 맞게 다듬었다.

- 포맷은 [`.clang-format`](../.clang-format), 이름 규칙은 [`.clang-tidy`](../.clang-tidy)가 자동으로 검사한다. **도구가 잡는 규칙은 사람이 리뷰에서 다투지 않는다.**
- 규칙끼리 충돌하거나 애매하면 **읽는 사람이 덜 헷갈리는 쪽**을 고른다.
- 이 문서를 바꾸려면 PR 로 제안하고 팀 합의 후 머지한다.

---

## 1. 네이밍

### 1.1 한눈에 보기

| 대상 | 규칙 | 예시 |
|---|---|---|
| 파일 | `snake_case`, 모듈 단위 | `hash_table.c`, `hash_table.h`, `event_loop.c` |
| 테스트 파일 | `test_<모듈>.c` | `tests/test_hash_table.c` |
| 구조체 (클래스 역할) | `snake_case` 명사 | `struct hash_table`, `struct client_conn` |
| enum 타입 | `snake_case` 명사 | `enum ht_result`, `enum resp_parse_result` |
| enum 값 | `UPPER_SNAKE_CASE`, 타입 접두사 | `HT_OK`, `HT_NOT_FOUND`, `RESP_PARSE_DONE` |
| 함수 (공개) | `<모듈>_<동사>[_<목적어>]` | `hash_table_find()`, `conn_write_reply()` |
| 함수 (내부) | `static`, 동사로 시작 | `static void rehash_step(...)` |
| 변수 · 필드 · 파라미터 | `snake_case` 명사 | `bucket_count`, `expire_at_ms` |
| bool | `is_` / `has_` / `can_` / `should_` | `is_rehashing`, `has_ttl` |
| 매크로 · 상수 | `UPPER_SNAKE_CASE`, 모듈 접두사 | `HT_INITIAL_SIZE`, `EXPIRE_SAMPLE_COUNT` |
| 전역 변수 | 가능하면 쓰지 않는다. 쓰면 `g_` 접두사 | `g_server` |
| 헤더 가드 | `MINI_REDIS_<파일명>_H` | `MINI_REDIS_HASH_TABLE_H` |
| 환경 변수 · 빌드 플래그 | `UPPER_SNAKE_CASE`, `MINI_REDIS_` 접두사 | `MINI_REDIS_PORT` |

C에는 클래스와 namespace 가 없으므로 **struct 가 클래스, 모듈 접두사가 namespace** 역할을 한다. 공개 함수에는 항상 모듈 접두사를 붙여 어느 파일에 있는지 이름만 보고 알 수 있게 한다.

### 1.2 모듈 접두사

새 모듈을 만들면 접두사를 이 표에 먼저 등록한다.

| 모듈 | 파일 | 접두사 |
|---|---|---|
| 해시 테이블 | `hash_table.c` | `hash_table_` / 상수 `HT_` |
| 이벤트 루프 | `event_loop.c` | `event_loop_` / 상수 `EL_` |
| 클라이언트 연결 | `client_conn.c` | `conn_` / 상수 `CONN_` |
| RESP 파서 · 응답 직렬화 | `resp.c` | `resp_` / 상수 `RESP_` |
| 명령 처리 (`SET`, `GET`, `DEL` ...) | `command.c` | `command_` / 상수 `COMMAND_` |
| 만료 | `expire.c` | `expire_` / 상수 `EXPIRE_` |
| 바이너리 안전 문자열 | `str.c` | `str_` |
| 시각 | `clock.c` | `clock_` |
| 메모리 래퍼 | `mem.c` | `mem_` |
| 가변 바이트 버퍼 | `buffer.c` | `buffer_` / 상수 `BUFFER_` |
| 저장소 (키 + 만료 테이블) | `db.c` | `db_` / 상수 `DB_` |
| 서버 · 설정 · 주기 작업 | `server.c` | `server_` / 상수 `SERVER_` |

### 1.3 RESP 명령 이름

명령과 응답은 실제 Redis 와 같게 맞춰서 `redis-cli`, `redis-py` 가 그대로 동작하게 한다.

| 대상 | 규칙 | 예시 |
|---|---|---|
| 명령 이름 | Redis 와 같은 이름, 대소문자 구분 없이 받는다 | `SET`, `GET`, `DEL` |
| 명령 옵션 | Redis 와 같은 이름, 대소문자 구분 없이 받는다 | `EX` |
| 에러 응답 | Redis 와 같은 접두어 + 메시지 | `-ERR unknown command 'foo'`, `-OOM ...` |
| 명령 처리 함수 | `static`, `handle_<명령>_command` | `handle_set_command()` |

### 1.4 이름 짓는 원칙

**변수는 명사, 함수는 동사로 짓는다.** 이름만 보고 무엇을 담는지, 무엇을 하는지 알 수 있어야 한다.

```c
/* Bad */
int n;
int flag;
void data(struct hash_table *ht);

/* Good */
size_t entry_count;
bool is_expired;
void hash_table_clear(struct hash_table *ht);
```

**단위를 이름에 넣는다.** 초와 ms 를 섞는 실수는 이름으로 막는다.

```c
int64_t ttl_ms;          /* 기간 */
int64_t expire_at_ms;    /* 절대 시각 */
int timeout_ms;
size_t max_bulk_bytes;
```

**길이와 용량을 구분한다.** `len` 은 사용 중인 크기, `cap` 은 할당된 크기다.

```c
struct buffer {
    char *data;
    size_t len;
    size_t cap;
};
```

**소유권을 이름으로 드러낸다.** 메모리를 할당하는 함수와 해제하는 함수는 짝을 맞춘다.

| 할당 | 해제 | 의미 |
|---|---|---|
| `*_create()` | `*_destroy()` | 객체 생성과 소멸 |
| `*_init()` | `*_deinit()` | 호출자가 준 메모리 위에서 초기화와 정리 |
| `*_dup()` | 짝이 되는 `*_destroy()` 또는 `free()` | 복사본을 만들어 소유권을 넘긴다 |

**축약은 아래 사전에 있는 것만 쓴다.** 새 축약이 필요하면 이 표에 추가하는 PR 을 먼저 올린다.

| 축약 | 원래 뜻 | 축약 | 원래 뜻 |
|---|---|---|---|
| `buf` | buffer | `idx` | index |
| `len` | length | `cap` | capacity |
| `conn` | connection | `ctx` | context |
| `cfg` | config | `ht` | hash table |
| `fd` | file descriptor | `cmd` | command |
| `ms` / `sec` | millisecond / second | `err` | error |

반복문 인덱스 `i`, `j` 는 범위가 몇 줄 안 될 때만 허용한다.

**예약된 이름은 쓰지 않는다.**

- `_` 로 시작하는 이름과 `__` 가 들어간 이름은 컴파일러와 표준 라이브러리가 예약했다.
- `_t` 접미사는 POSIX 가 예약했다. `hash_table_t` 대신 `struct hash_table` 을 쓴다.
- 구조체에는 `typedef` 를 쓰지 않는다. `struct` 키워드가 붙어 있으면 값 타입인지 구조체인지 바로 보인다. (함수 포인터 타입은 예외로 `typedef` 를 허용한다. 예: `typedef void event_handler_fn(struct event_loop *loop, int fd);`)

---

## 2. 함수 설계

### 2.1 인자는 3개 이하

인자가 3개를 넘으면 struct 로 묶는다. 첫 번째 인자는 **작업 대상 객체**(다른 언어의 `self`)로 고정한다.

```c
/* Bad */
int server_start(const char *host, int port, int backlog, int max_clients, size_t max_bulk);

/* Good */
struct server_config {
    const char *host;
    int port;
    int backlog;
    int max_clients;
    size_t max_bulk_bytes;
};

int server_start(struct server *server, const struct server_config *cfg);
```

- 함수가 읽기만 하는 포인터 인자에는 반드시 `const` 를 붙인다. `const` 가 "이 함수는 이 값을 바꾸지 않는다"는 문서 역할을 한다.
- bool 인자는 호출하는 쪽에서 의미가 안 보이므로 쓰지 않는다. 함수를 둘로 나누거나 enum 을 쓴다.

```c
/* Bad: true 가 무엇인지 알 수 없다 */
hash_table_delete(ht, key, true);

/* Good */
hash_table_delete(ht, key);
hash_table_delete_async(ht, key);
```

### 2.2 명령과 조회를 분리한다

C에는 예외가 없어서 반환값이 에러를 전달해야 한다. 그래서 다음 세 가지 형태만 쓴다.

```c
/* 조회: 상태를 바꾸지 않는다. 결과를 반환하고, 없으면 NULL */
const struct entry *hash_table_find(const struct hash_table *ht, const struct str *key);

/* 명령: 상태를 바꾼다. 결과 코드만 반환한다 */
enum ht_result hash_table_set(struct hash_table *ht, struct str *key, struct str *value);

/* 명령이면서 값도 돌려줘야 할 때: out 파라미터를 맨 뒤에 두고 out_ 접두사를 붙인다 */
enum ht_result hash_table_pop(struct hash_table *ht, const struct str *key,
                              struct str **out_value);
```

- 결과 코드는 `0` 과 `-1` 을 섞은 `int` 대신 **모듈별 결과 enum** 을 쓴다. 성공 값은 항상 `0` 이다.

```c
enum ht_result {
    HT_OK = 0,
    HT_NOT_FOUND,
    HT_OUT_OF_MEMORY,
};
```

- 조회 함수 이름은 `find` / `get` / `count` / `is_` / `has_` 로, 명령 함수 이름은 `set` / `add` / `delete` / `clear` / `write` 처럼 상태를 바꾸는 동사로 시작한다.

### 2.3 함수는 한 가지 일만 한다

- 기준은 **40줄 이하, 중첩 3단계 이하**다. 넘으면 내부 `static` 함수로 쪼갠다.
- 한 함수 안에서는 추상화 수준을 맞춘다. 고수준 흐름(`parse` → `execute` → `reply`) 사이에 바이트 단위 처리가 끼어 있으면 따로 뺀다.
- 예외 상황은 **early return** 으로 먼저 처리해서 중첩을 줄인다.

```c
int conn_handle_command(struct client_conn *conn)
{
    struct resp_command *cmd = resp_command_parse(&conn->read_buf);
    if (cmd == NULL) {
        return conn_reply_error(conn, "ERR Protocol error");
    }

    int result = command_execute(conn, cmd);
    resp_command_destroy(cmd);
    return result;
}
```

### 2.4 자원 정리는 `goto cleanup` 으로 한 곳에서

해제할 자원이 2개 이상이면 단일 출구 패턴을 쓴다. C에서는 이것이 공식적으로 허용되는 `goto` 용도다. 다른 용도의 `goto` 는 쓰지 않는다.

```c
enum ht_result hash_table_set_copy(struct hash_table *ht, const struct str *key,
                                   const struct str *value)
{
    enum ht_result result = HT_OUT_OF_MEMORY;
    struct str *key_copy = NULL;
    struct str *value_copy = NULL;

    key_copy = str_dup(key);
    if (key_copy == NULL) {
        goto cleanup;
    }
    value_copy = str_dup(value);
    if (value_copy == NULL) {
        goto cleanup;
    }

    result = hash_table_set(ht, key_copy, value_copy);
    if (result == HT_OK) {
        return HT_OK; /* 소유권이 해시 테이블로 넘어갔다 */
    }

cleanup:
    str_destroy(value_copy);
    str_destroy(key_copy);
    return result;
}
```

---

## 3. 주석

**코드가 "무엇을" 하는지는 이름으로 설명하고, 주석은 "왜"와 "계약"에만 쓴다.** 주석은 한국어로 쓴다.

C에서는 아래 네 가지는 반드시 주석으로 남긴다. 이름만으로는 표현할 수 없기 때문이다.

| 종류 | 예시 |
|---|---|
| 소유권과 수명 | `/* 반환된 문자열은 호출자가 free 해야 한다 */` |
| 불변식 | `/* rehash 중에는 새 키를 ht[1] 에만 넣는다 */` |
| 설계 이유와 트레이드오프 | `/* 이벤트 루프가 막히지 않도록 한 사이클에 최대 1ms 만 쓴다 */` |
| 공개 헤더의 함수 설명 | 함수 선언 위 한두 줄 |

쓰지 않는 주석:

```c
i++;              /* i 를 1 증가 */           /* 코드를 그대로 읽은 주석 */
/* int old = 0; */                           /* 주석 처리한 코드: git 이 기억한다 */
/* 2026-11-13 홍길동 수정 */                    /* 변경 이력: git log 를 본다 */
```

- 주석 형식은 `/* */` 로 통일한다.
- 할 일은 `/* TODO(이름): 내용 */` 으로 남기고, 이슈가 있으면 번호를 붙인다.

---

## 4. 파일과 모듈 구조

### 4.1 헤더는 계약만 공개한다

- 구현 세부를 숨길 수 있는 구조체는 헤더에 **불완전 타입**으로만 선언하고, 필드는 `.c` 에 둔다.
- 다른 모듈이 쓰지 않는 함수와 전역 변수는 모두 `static` 으로 선언한다.

```c
/* hash_table.h */
#ifndef MINI_REDIS_HASH_TABLE_H
#define MINI_REDIS_HASH_TABLE_H

#include <stddef.h>

struct hash_table;

/* 빈 해시 테이블을 만든다. 실패하면 NULL. hash_table_destroy() 로 해제한다. */
struct hash_table *hash_table_create(void);
void hash_table_destroy(struct hash_table *ht);

size_t hash_table_count(const struct hash_table *ht);

#endif
```

### 4.2 include 순서

그룹 사이에 빈 줄을 하나 두고, 그룹 안에서는 알파벳 순으로 정렬한다.

1. 자기 자신의 헤더 (`hash_table.c` 라면 `"hash_table.h"`) — 헤더가 혼자서도 컴파일되는지 확인하는 효과가 있다
2. C 표준 라이브러리 (`<stdio.h>`, `<stdlib.h>`)
3. 시스템 헤더 (`<sys/epoll.h>`, `<unistd.h>`)
4. 프로젝트 헤더 (`"resp.h"`)

### 4.3 파일 안의 순서

include → 매크로와 상수 → 타입 정의 → `static` 함수 선언 → 공개 함수 → `static` 함수 구현

---

## 5. 값과 타입

- **매직 넘버는 쓰지 않는다.** 정수 상수는 `enum`, 그 외 상수는 `static const` 로 이름을 붙인다. `#define` 은 조건부 컴파일이나 매크로가 꼭 필요할 때만 쓴다.

```c
enum {
    HT_INITIAL_SIZE = 16,
    EXPIRE_SAMPLE_COUNT = 20,
};
static const double HT_MAX_LOAD_FACTOR = 1.0;
```

- 크기와 개수는 `size_t`, 시간은 `int64_t`(ms 단위), bool 은 `<stdbool.h>` 의 `bool` 을 쓴다.
- 변수는 처음 쓰는 곳에서 선언하고 선언과 동시에 초기화한다. (C11)
- `malloc` 의 반환값은 항상 `NULL` 검사를 한다. 캐스팅은 하지 않는다.
- `sizeof` 는 타입이 아니라 변수에 적용한다: `malloc(sizeof(*entry))`
- 문자열은 `strcpy` / `sprintf` 대신 길이를 받는 `memcpy` / `snprintf` 를 쓴다.
- 키와 값은 `\0` 을 포함할 수 있으므로 `strlen` 에 의존하지 않고 길이를 함께 들고 다닌다.

---

## 6. 매크로

- 여러 문장으로 된 매크로는 `do { ... } while (0)` 로 감싼다.
- 매크로 인자는 괄호로 감싼다: `#define MIN(a, b) ((a) < (b) ? (a) : (b))`
- 함수로 쓸 수 있으면 매크로 대신 `static inline` 함수를 쓴다. 타입 검사와 디버깅이 된다.
- 테스트 매크로(`CHECK`, `RUN`)처럼 `__FILE__`, `__LINE__`, `#expr` 이 필요한 경우에만 매크로를 쓴다.

---

## 7. 포맷

`.clang-format` 이 강제하므로 직접 맞출 필요는 없다. 요약하면 다음과 같다.

| 항목 | 규칙 |
|---|---|
| 들여쓰기 | 스페이스 4칸, 탭 금지 |
| 한 줄 길이 | 100자 |
| 중괄호 | 함수는 다음 줄, 제어문과 구조체는 같은 줄 |
| `if` / `for` / `while` | 본문이 한 줄이어도 항상 중괄호 |
| 포인터 | `*` 를 변수 쪽에 붙인다: `char *name` |
| `switch` | `case` 는 `switch` 와 같은 들여쓰기 |
| 줄 끝 | LF, 파일 끝에 개행 하나 |

```c
static size_t bucket_index(const struct hash_table *ht, uint64_t hash)
{
    return hash & (ht->bucket_count - 1);
}

switch (parse_result) {
case RESP_PARSE_DONE:
    return execute_command(conn, &cmd);
case RESP_PARSE_NEED_MORE:
    return 0;
default:
    return conn_reply_error(conn, "ERR Protocol error");
}
```

---

## 8. 테스트

- 테스트 파일은 `tests/test_<모듈>.c`, 테스트 함수는 `test_<대상>_<상황>_<기대 결과>` 로 짓는다.

```c
static void test_find_after_delete_returns_null(void);
static void test_set_existing_key_overwrites_value(void);
```

- 테스트 하나는 한 가지 동작만 검증한다.
- 테스트도 같은 스타일 규칙을 따른다.

---

## 9. 도구

```bash
# 포맷 자동 적용
clang-format -i src/*.c src/*.h tests/*.c tests/*.h

# 포맷 검사만 (CI 용)
clang-format --dry-run -Werror src/*.c src/*.h tests/*.c tests/*.h

# 이름 규칙과 정적 분석
clang-tidy src/*.c -- -std=c11 -D_GNU_SOURCE -Isrc
```

- 개발 컨테이너(Ubuntu 22.04)에서는 `apt-get install clang-format clang-tidy` 로 설치한다.
- 컴파일 경고는 모두 고친다. 현재 `Makefile` 은 `-Wall -Wextra` 를 쓰고, 코드가 안정되면 `-Werror -Wshadow` 를 추가한다.

---

## 10. 리뷰 체크리스트

PR 을 올리기 전에 스스로 확인한다.

- [ ] 이름만 보고 무엇을 담는 변수인지, 무엇을 하는 함수인지 알 수 있는가
- [ ] 시간과 크기를 담는 변수 이름에 단위가 들어갔는가
- [ ] 인자가 3개 이하이고, 읽기 전용 포인터에 `const` 를 붙였는가
- [ ] 조회 함수가 상태를 바꾸지 않는가
- [ ] 할당한 메모리를 해제하는 경로가 모든 분기에 있는가
- [ ] 소유권과 불변식을 주석으로 남겼는가
- [ ] `clang-format` 과 `clang-tidy` 를 돌렸는가
