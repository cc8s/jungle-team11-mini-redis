#include "test.h"

#include <stdarg.h>
#include "commands.h"
#include "expire.h"

static int64_t fake_now = 1000000;
static int64_t fake_clock(void)
{
    return fake_now;
}

static db *D;
static buf OUT;
static int last_write;

/* 문자열 인자들로 명령을 실행하고 응답이 want 와 바이트 단위로 같은지 본다 */
static int run(const char *want, int argc, ...)
{
    resp_cmd c;
    c.argc = argc;
    c.argv = calloc((size_t)argc + 1, sizeof *c.argv);
    c.argvlen = calloc((size_t)argc + 1, sizeof *c.argvlen);
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc; i++) {
        const char *s = va_arg(ap, const char *);
        c.argv[i] = strdup(s);
        c.argvlen[i] = strlen(s);
    }
    va_end(ap);
    buf_clear(&OUT);
    last_write = cmd_execute(D, &c, &OUT);
    resp_cmd_free(&c);
    int ok = OUT.len == strlen(want) && memcmp(OUT.data, want, OUT.len) == 0;
    if (!ok && want[0] != '\0') /* want 가 "" 이면 실행만 하고 응답은 호출자가 본다 */
        fprintf(stderr, "  want: %s\n  got:  %.*s\n", want, (int)OUT.len, OUT.data ? OUT.data : "");
    return ok;
}

static void reset(size_t maxmemory, int policy)
{
    expire_set_clock(fake_clock);
    fake_now = 1000000;
    db_free(D);
    D = db_create(maxmemory, policy);
    buf_clear(&OUT);
}

/* TC-CMD-001 EP: PING */
static void test_ping(void)
{
    reset(0, DB_NOEVICTION);
    CHECK(run("+PONG\r\n", 1, "PING"));
    CHECK(run("$2\r\nhi\r\n", 2, "ping", "hi"));
    CHECK(run("-ERR wrong number of arguments for 'ping' command\r\n", 3, "PING", "a", "b"));
    /* ECHO: redis-cli --pipe 가 대량 입력의 끝을 확인할 때 보낸다 */
    CHECK(run("$5\r\nhello\r\n", 2, "ECHO", "hello"));
    CHECK(run("-ERR wrong number of arguments for 'echo' command\r\n", 1, "ECHO"));
}

/* TC-CMD-002 EP: SET / GET, 대소문자 무시, 쓰기 표시 */
static void test_set_get(void)
{
    reset(0, DB_NOEVICTION);
    CHECK(run("+OK\r\n", 3, "SET", "name", "cc8s"));
    CHECK(last_write == 1);
    CHECK(run("$4\r\ncc8s\r\n", 2, "get", "name"));
    CHECK(last_write == 0);
    CHECK(run("$-1\r\n", 2, "GET", "nope"));
    CHECK(run("-ERR wrong number of arguments for 'get' command\r\n", 1, "GET"));
    CHECK(run("-ERR wrong number of arguments for 'set' command\r\n", 2, "SET", "k"));
}

/* TC-CMD-003 DT: SET 옵션 EX/PX × NX/XX 와 오류 */
static void test_set_options(void)
{
    reset(0, DB_NOEVICTION);
    CHECK(run("+OK\r\n", 5, "SET", "a", "1", "EX", "10"));
    CHECK(run(":10\r\n", 2, "TTL", "a"));
    CHECK(run("+OK\r\n", 5, "SET", "b", "1", "px", "1500"));
    CHECK(run(":1500\r\n", 2, "PTTL", "b"));
    CHECK(run("$-1\r\n", 4, "SET", "a", "2", "NX"));  /* 있으니 안 씀 */
    CHECK(last_write == 0);
    CHECK(run("$1\r\n1\r\n", 2, "GET", "a"));
    CHECK(run("+OK\r\n", 4, "SET", "c", "1", "NX"));
    CHECK(run("$-1\r\n", 4, "SET", "d", "1", "XX"));  /* 없으니 안 씀 */
    CHECK(run("+OK\r\n", 4, "SET", "c", "2", "xx"));
    CHECK(run("+OK\r\n", 6, "SET", "e", "1", "NX", "EX", "5"));
    CHECK(run("-ERR syntax error\r\n", 5, "SET", "k", "v", "NX", "XX"));
    CHECK(run("-ERR syntax error\r\n", 7, "SET", "k", "v", "EX", "1", "PX", "1"));
    CHECK(run("-ERR syntax error\r\n", 4, "SET", "k", "v", "EX"));
    CHECK(run("-ERR syntax error\r\n", 4, "SET", "k", "v", "FOO"));
    CHECK(run("-ERR value is not an integer or out of range\r\n", 5, "SET", "k", "v", "EX", "abc"));
    CHECK(run("-ERR invalid expire time in 'set' command\r\n", 5, "SET", "k", "v", "EX", "0"));
    CHECK(run("-ERR invalid expire time in 'set' command\r\n", 5, "SET", "k", "v", "PX", "-5"));
    CHECK(run("-ERR invalid expire time in 'set' command\r\n", 5, "SET", "k", "v", "EX", "9223372036854775807"));
    CHECK(run("$-1\r\n", 2, "GET", "k")); /* 오류면 아무것도 안 씀 */
}

/* TC-CMD-004 ST: SET 덮어쓰기는 TTL 을 지운다 */
static void test_set_clears_ttl(void)
{
    reset(0, DB_NOEVICTION);
    run("+OK\r\n", 5, "SET", "k", "v", "EX", "10");
    CHECK(run("+OK\r\n", 3, "SET", "k", "v2"));
    CHECK(run(":-1\r\n", 2, "TTL", "k"));
}

/* TC-CMD-005 EP: DEL / EXISTS 여러 키 (중복 처리 차이) */
static void test_del_exists(void)
{
    reset(0, DB_NOEVICTION);
    run("+OK\r\n", 3, "SET", "a", "1");
    run("+OK\r\n", 3, "SET", "b", "1");
    CHECK(run(":3\r\n", 4, "EXISTS", "a", "a", "b")); /* EXISTS 는 중복도 센다 */
    CHECK(run(":0\r\n", 2, "EXISTS", "zz"));
    CHECK(run(":2\r\n", 5, "DEL", "a", "a", "b", "zz")); /* DEL 은 실제로 지운 수 */
    CHECK(last_write == 1);
    CHECK(run(":0\r\n", 2, "DEL", "a"));
    CHECK(last_write == 0);
    CHECK(run("-ERR wrong number of arguments for 'del' command\r\n", 1, "DEL"));
}

/* TC-CMD-006 BVA: EXPIRE / PEXPIRE / TTL 반올림 / PTTL / PERSIST */
static void test_expire_family(void)
{
    reset(0, DB_NOEVICTION);
    run("+OK\r\n", 3, "SET", "k", "v");
    CHECK(run(":-1\r\n", 2, "TTL", "k"));
    CHECK(run(":-2\r\n", 2, "TTL", "nope"));
    CHECK(run(":-2\r\n", 2, "PTTL", "nope"));
    CHECK(run(":1\r\n", 3, "EXPIRE", "k", "10"));
    CHECK(run(":0\r\n", 3, "EXPIRE", "nope", "10"));
    fake_now += 9400; /* 600ms 남음 → 반올림 1 */
    CHECK(run(":1\r\n", 2, "TTL", "k"));
    CHECK(run(":600\r\n", 2, "PTTL", "k"));
    fake_now += 200;  /* 400ms 남음 → 반올림 0 */
    CHECK(run(":0\r\n", 2, "TTL", "k"));
    CHECK(run(":1\r\n", 2, "PERSIST", "k"));
    CHECK(run(":0\r\n", 2, "PERSIST", "k"));
    CHECK(run(":1\r\n", 3, "PEXPIRE", "k", "50"));
    fake_now += 51;
    CHECK(run("$-1\r\n", 2, "GET", "k"));
    CHECK(run(":0\r\n", 2, "EXISTS", "k"));
}

/* TC-CMD-007 BVA·EG: EXPIRE 0·음수는 즉시 삭제, 정수·범위 오류 */
static void test_expire_edges(void)
{
    reset(0, DB_NOEVICTION);
    run("+OK\r\n", 3, "SET", "a", "1");
    run("+OK\r\n", 3, "SET", "b", "1");
    CHECK(run(":1\r\n", 3, "EXPIRE", "a", "0"));
    CHECK(run(":0\r\n", 2, "EXISTS", "a"));
    CHECK(run(":1\r\n", 3, "PEXPIRE", "b", "-100"));
    CHECK(run(":0\r\n", 2, "EXISTS", "b"));
    run("+OK\r\n", 3, "SET", "c", "1");
    CHECK(run("-ERR value is not an integer or out of range\r\n", 3, "EXPIRE", "c", "1.5"));
    CHECK(run("-ERR value is not an integer or out of range\r\n", 3, "EXPIRE", "c", "99999999999999999999"));
    CHECK(run("-ERR invalid expire time in 'expire' command\r\n", 3, "EXPIRE", "c", "9223372036854775807"));
    CHECK(run("-ERR invalid expire time in 'pexpire' command\r\n", 3, "PEXPIRE", "c", "9223372036854775807"));
    CHECK(run(":-1\r\n", 2, "TTL", "c")); /* 오류면 그대로 */
}

/* TC-CMD-008 EP·EG: KEYS 글롭, 만료된 키 제외 */
static void test_keys(void)
{
    reset(0, DB_NOEVICTION);
    run("+OK\r\n", 3, "SET", "user:1", "a");
    run("+OK\r\n", 3, "SET", "user:2", "a");
    run("+OK\r\n", 3, "SET", "item:1", "a");
    run("+OK\r\n", 5, "SET", "user:3", "a", "PX", "10");
    fake_now += 11;
    buf_clear(&OUT);
    CHECK(run("*0\r\n", 2, "KEYS", "zzz*"));
    /* 순서는 해시 순서라 정해져 있지 않다 → 개수와 포함 여부만 */
    run("", 2, "KEYS", "user:*");
    CHECK(OUT.len > 4 && memcmp(OUT.data, "*2\r\n", 4) == 0);
    CHECK(strstr(OUT.data, "user:1") && strstr(OUT.data, "user:2") && !strstr(OUT.data, "user:3"));
    run("", 2, "KEYS", "*");
    CHECK(memcmp(OUT.data, "*3\r\n", 4) == 0);
}

/* TC-CMD-009 DT: 글롭 패턴 */
static void test_glob(void)
{
#define G(p, s) glob_match((p), strlen(p), (s), strlen(s))
    CHECK(G("*", "") && G("*", "abc"));
    CHECK(G("a*c", "abbbc") && !G("a*c", "abcd"));
    CHECK(G("h?llo", "hello") && !G("h?llo", "hllo"));
    CHECK(G("h[ae]llo", "hallo") && !G("h[ae]llo", "hillo"));
    CHECK(G("h[^e]llo", "hallo") && !G("h[^e]llo", "hello"));
    CHECK(G("h[a-c]llo", "hbllo") && !G("h[a-c]llo", "hdllo"));
    CHECK(G("a\\*b", "a*b") && !G("a\\*b", "axb"));
    CHECK(G("*:*:*", "a:b:c") && !G("*:*:*", "a:b"));
    CHECK(glob_match("a?c", 3, "a\0c", 3)); /* 바이너리 */
#undef G
}

/* TC-CMD-010 ST: FLUSHALL */
static void test_flushall(void)
{
    reset(0, DB_NOEVICTION);
    run("+OK\r\n", 3, "SET", "a", "1");
    CHECK(run("+OK\r\n", 1, "FLUSHALL"));
    CHECK(last_write == 1);
    CHECK(run("*0\r\n", 2, "KEYS", "*"));
    CHECK(run("+OK\r\n", 2, "FLUSHALL", "ASYNC"));
}

/* TC-CMD-011 EG: 모르는 명령 (Redis 7 문구), 빈 명령 */
static void test_unknown(void)
{
    reset(0, DB_NOEVICTION);
    CHECK(run("-ERR unknown command 'foo', with args beginning with: \r\n", 1, "foo"));
    CHECK(run("-ERR unknown command 'INCR', with args beginning with: 'a' 'b' \r\n", 3, "INCR", "a", "b"));
    CHECK(run("", 0));
    CHECK(last_write == 0);
}

/* TC-CMD-012 EP: 클라이언트 접속용 최소 응답 (D3) */
static void test_handshake(void)
{
    reset(0, DB_NOEVICTION);
    CHECK(run("*0\r\n", 2, "COMMAND", "DOCS"));
    CHECK(run("*0\r\n", 1, "COMMAND"));
    CHECK(run("*0\r\n", 3, "CONFIG", "GET", "save"));
    CHECK(run("+OK\r\n", 4, "CLIENT", "SETINFO", "LIB-NAME", "redis-py"));
    CHECK(run("+OK\r\n", 2, "SELECT", "0"));
    CHECK(run("-ERR DB index is out of range\r\n", 2, "SELECT", "1"));
}

/* TC-CMD-013 BVA: 메모리 상한 — noeviction 은 OOM, allkeys-random 은 내보내고 OK (D7) */
static void test_maxmemory(void)
{
    size_t one = db_entry_cost(1, 1);
    reset(one * 2, DB_NOEVICTION);
    CHECK(run("+OK\r\n", 3, "SET", "a", "1"));
    CHECK(run("+OK\r\n", 3, "SET", "b", "1"));
    CHECK(run("-OOM command not allowed when used memory > 'maxmemory'.\r\n", 3, "SET", "c", "1"));
    CHECK(last_write == 0);
    CHECK(run(":1\r\n", 2, "DEL", "a")); /* 지우는 명령은 된다 */
    CHECK(run("+OK\r\n", 3, "SET", "c", "1"));

    reset(one * 2, DB_ALLKEYS_RANDOM);
    run("+OK\r\n", 3, "SET", "a", "1");
    run("+OK\r\n", 3, "SET", "b", "1");
    CHECK(run("+OK\r\n", 3, "SET", "c", "1"));
    CHECK(D->evicted == 1);
    CHECK(run("$1\r\n1\r\n", 2, "GET", "c"));
}

/* TC-CMD-014 EG: 바이너리 값 그대로 왕복 */
static void test_binary_roundtrip(void)
{
    reset(0, DB_NOEVICTION);
    resp_cmd c;
    static const char in[] = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\na\0b\r\n\r\n"; /* 값 = a \0 b \r \n */
    size_t used;
    const char *err;
    CHECK(resp_parse(in, sizeof in - 1, &c, &used, &err) == RESP_OK);
    buf_clear(&OUT);
    cmd_execute(D, &c, &OUT);
    resp_cmd_free(&c);
    run("", 2, "GET", "k"); /* 응답 비교는 아래에서 바이트로 */
    CHECK(OUT.len == 11 && memcmp(OUT.data, "$5\r\na\0b\r\n\r\n", 11) == 0);
}

/* 문자열 인자 명령을 실행만 한다 (응답은 OUT 에 남는다) */
#define EXEC(...) run("", __VA_ARGS__)

static int out_is(const buf *want)
{
    int ok = OUT.len == want->len && memcmp(OUT.data, want->data, OUT.len) == 0;
    if (!ok)
        fprintf(stderr, "  want: %.*s\n  got:  %.*s\n", (int)want->len, want->data, (int)OUT.len, OUT.data);
    return ok;
}

/* TC-CMD-015 EP: DEBUG HT 응답 모양 — 키 1개, 버킷 4개, ht[1] 비어 있음. maxbuckets 로 앞부분만 */
static void test_debug_ht_shape(void)
{
    reset(0, DB_NOEVICTION);
    CHECK(run("+OK\r\n", 3, "SET", "a", "1"));
    size_t slot = dict_default_hash("a", 1) & 3;
    for (int maxb = 4; maxb >= 2; maxb -= 2) {
        buf w;
        buf_init(&w);
        buf_appendf(&w, "*14\r\n$6\r\nnow_ms\r\n:1000000\r\n$11\r\nused_memory\r\n:%zu\r\n", db_entry_cost(1, 1));
        buf_appendf(&w, "$9\r\nmaxmemory\r\n:0\r\n$7\r\nevicted\r\n:0\r\n$13\r\nactive_rehash\r\n:1\r\n");
        buf_appendf(&w, "$9\r\nrehashidx\r\n:-1\r\n$6\r\ntables\r\n*2\r\n*3\r\n:4\r\n:1\r\n*%d\r\n", maxb);
        for (size_t i = 0; i < (size_t)maxb; i++)
            buf_appendf(&w, i == slot ? "*1\r\n*2\r\n$1\r\na\r\n$1\r\n1\r\n" : "*0\r\n");
        buf_appendf(&w, "*3\r\n:0\r\n:0\r\n*0\r\n");
        if (maxb == 4)
            EXEC(3, "DEBUG", "HT", "main");
        else
            EXEC(4, "debug", "ht", "main", "2");
        CHECK(out_is(&w));
        buf_free(&w);
    }
    /* expires 는 값 대신 만료 시각(정수) */
    CHECK(run(":1\r\n", 3, "PEXPIRE", "a", "500"));
    EXEC(3, "DEBUG", "HT", "expires");
    CHECK(strstr(OUT.data, "*2\r\n$1\r\na\r\n:1000500\r\n") != NULL);
}

/* TC-CMD-016 ST: DEBUG HT 는 읽기 전용 — rehash 를 진행시키지 않고 만료된 키도 지우지 않는다.
 * REHASHSTEP 은 진행시키고, ACTIVEREHASH 는 플래그만 바꾼다 */
static void test_debug_readonly_and_step(void)
{
    reset(0, DB_NOEVICTION);
    const char *k[] = {"k1", "k2", "k3", "k4", "k5"};
    for (int i = 0; i < 5; i++)
        run("+OK\r\n", 3, "SET", k[i], "v");
    CHECK(dict_is_rehashing(D->main)); /* 5번째 키에서 4 → 8 rehash 시작 */
    long idx = D->main->rehashidx;
    for (int i = 0; i < 3; i++)
        EXEC(3, "DEBUG", "HT", "main");
    CHECK(D->main->rehashidx == idx);

    EXEC(4, "DEBUG", "REHASHSTEP", "main", "1"); /* 한 칸: 그 버킷에 키가 몰려 있었으면 바로 끝날 수도 있다 */
    char want[32];
    snprintf(want, sizeof want, ":%ld\r\n", D->main->rehashidx);
    CHECK(D->main->rehashidx != idx && strcmp(OUT.data, want) == 0);
    CHECK(run(":-1\r\n", 4, "DEBUG", "REHASHSTEP", "main", "100"));
    CHECK(D->main->ht[0].size == 8 && D->main->ht[1].size == 0);
    CHECK(run(":-1\r\n", 4, "DEBUG", "REHASHSTEP", "expires", "1")); /* rehash 중이 아니면 그대로 */

    run("+OK\r\n", 5, "SET", "x", "1", "PX", "10");
    fake_now += 100;
    size_t before = dict_len(D->main);
    EXEC(3, "DEBUG", "HT", "main");
    EXEC(3, "DEBUG", "HT", "expires");
    CHECK(dict_len(D->main) == before && dict_len(D->expires) == 1);

    CHECK(run("+OK\r\n", 3, "DEBUG", "ACTIVEREHASH", "off"));
    CHECK(D->active_rehash == 0);
    EXEC(3, "DEBUG", "HT", "main");
    CHECK(strstr(OUT.data, "$13\r\nactive_rehash\r\n:0\r\n") != NULL);
    CHECK(run("+OK\r\n", 3, "DEBUG", "ACTIVEREHASH", "ON"));
    CHECK(D->active_rehash == 1);
    CHECK(last_write == 0);
}

/* TC-CMD-017 BVA/EG: DEBUG 인자 오류 */
static void test_debug_errors(void)
{
    reset(0, DB_NOEVICTION);
    CHECK(run("-ERR wrong number of arguments for 'debug' command\r\n", 2, "DEBUG", "HT"));
    CHECK(run("-ERR DEBUG HT target must be 'main' or 'expires'\r\n", 3, "DEBUG", "HT", "foo"));
    CHECK(run("-ERR maxbuckets must be between 1 and 65536\r\n", 4, "DEBUG", "HT", "main", "0"));
    CHECK(run("-ERR maxbuckets must be between 1 and 65536\r\n", 4, "DEBUG", "HT", "main", "65537"));
    CHECK(run("-ERR maxbuckets must be between 1 and 65536\r\n", 4, "DEBUG", "HT", "main", "x"));
    EXEC(4, "DEBUG", "HT", "main", "65536");
    CHECK(OUT.data[0] == '*');
    CHECK(run("-ERR steps must be between 1 and 1000000\r\n", 4, "DEBUG", "REHASHSTEP", "main", "0"));
    CHECK(run("-ERR steps must be between 1 and 1000000\r\n", 4, "DEBUG", "REHASHSTEP", "main", "1000001"));
    CHECK(run("-ERR DEBUG REHASHSTEP target must be 'main' or 'expires'\r\n", 4, "DEBUG", "REHASHSTEP", "x", "1"));
    CHECK(run("-ERR unknown DEBUG subcommand. Try HT, REHASHSTEP, ACTIVEREHASH\r\n", 3, "DEBUG", "ACTIVEREHASH", "maybe"));
    CHECK(run("-ERR unknown DEBUG subcommand. Try HT, REHASHSTEP, ACTIVEREHASH\r\n", 3, "DEBUG", "NOPE", "x"));
    CHECK(run("-ERR unknown DEBUG subcommand. Try HT, REHASHSTEP, ACTIVEREHASH\r\n", 5, "DEBUG", "HT", "main", "1", "2"));
}

int main(void)
{
    buf_init(&OUT);
    RUN(test_ping);
    RUN(test_set_get);
    RUN(test_set_options);
    RUN(test_set_clears_ttl);
    RUN(test_del_exists);
    RUN(test_expire_family);
    RUN(test_expire_edges);
    RUN(test_keys);
    RUN(test_glob);
    RUN(test_flushall);
    RUN(test_unknown);
    RUN(test_handshake);
    RUN(test_maxmemory);
    RUN(test_binary_roundtrip);
    RUN(test_debug_ht_shape);
    RUN(test_debug_readonly_and_step);
    RUN(test_debug_errors);
    db_free(D);
    buf_free(&OUT);
    TEST_DONE();
    return 0;
}
