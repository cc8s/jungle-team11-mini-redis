#include "test.h"

#include "resp.h"

/* ---- 도우미 ---- */

typedef struct {
    resp_status st;
    size_t consumed;
    resp_cmd cmd;
    const char *err;
} result;

static result parse(const char *p, size_t n)
{
    result r;
    memset(&r, 0, sizeof r);
    r.st = resp_parse(p, n, &r.cmd, &r.consumed, &r.err);
    return r;
}
#define PARSE_S(s) parse((s), sizeof(s) - 1)

static int arg_is(const resp_cmd *c, int i, const char *s, size_t n)
{
    return i < c->argc && c->argvlen[i] == n && memcmp(c->argv[i], s, n) == 0 && c->argv[i][n] == '\0';
}
#define ARG_IS(c, i, s) arg_is((c), (i), (s), sizeof(s) - 1)

static int err_has(const result *r, const char *needle)
{
    return r->st == RESP_ERR && r->err != NULL && strstr(r->err, needle) != NULL && strncmp(r->err, "ERR ", 4) == 0;
}

/* ---- 기본 ---- */

/* TC-RESP-001 EP: 배열 명령 하나 */
static void test_basic_set(void)
{
    static const char in[] = "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$4\r\ncc8s\r\n";
    result r = PARSE_S(in);
    CHECK(r.st == RESP_OK);
    CHECK(r.consumed == sizeof in - 1);
    CHECK(r.cmd.argc == 3);
    CHECK(ARG_IS(&r.cmd, 0, "SET"));
    CHECK(ARG_IS(&r.cmd, 1, "name"));
    CHECK(ARG_IS(&r.cmd, 2, "cc8s"));
    resp_cmd_free(&r.cmd);
    CHECK(r.cmd.argc == 0);
}

/* TC-RESP-002 BVA: 완성된 명령의 모든 앞부분(0 ~ 길이-1 바이트)은 INCOMPLETE, 아무것도 소비하지 않음 */
static void test_every_prefix_incomplete(void)
{
    static const char in[] = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\n";
    size_t n = sizeof in - 1;
    for (size_t i = 0; i < n; i++) {
        result r = parse(in, i);
        CHECK(r.st == RESP_INCOMPLETE);
        CHECK(r.consumed == 0);
        CHECK(r.cmd.argc == 0);
    }
    result r = parse(in, n);
    CHECK(r.st == RESP_OK && r.consumed == n);
    resp_cmd_free(&r.cmd);
}

/* TC-RESP-003 BVA: 학습 노트 Q14 — 머리줄 $5 는 왔지만 값이 3바이트뿐 */
static void test_q14_header_is_not_element(void)
{
    result r = PARSE_S("*2\r\n$3\r\nGET\r\n$5\r\nhel");
    CHECK(r.st == RESP_INCOMPLETE);
    r = PARSE_S("*2\r\n$3\r\nGET\r\n$5\r\nhello\r");
    CHECK(r.st == RESP_INCOMPLETE); /* \r\n 중 \n 이 아직 없음 */
    r = PARSE_S("*2\r\n$3\r\nGET\r\n$5\r\nhello\r\n");
    CHECK(r.st == RESP_OK && r.cmd.argc == 2 && ARG_IS(&r.cmd, 1, "hello"));
    resp_cmd_free(&r.cmd);
}

/* TC-RESP-004 ST: 파이프라이닝 — 명령 3개 + 반쪽 하나 */
static void test_pipeline(void)
{
    static const char in[] =
        "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n"
        "*2\r\n$3\r\nGET\r\n$1\r\na\r\n"
        "PING\r\n"
        "*2\r\n$3\r\nDEL";
    const char *p = in;
    size_t left = sizeof in - 1;
    const char *names[] = {"SET", "GET", "PING"};
    for (int i = 0; i < 3; i++) {
        result r = parse(p, left);
        CHECK(r.st == RESP_OK);
        CHECK(arg_is(&r.cmd, 0, names[i], strlen(names[i])));
        p += r.consumed;
        left -= r.consumed;
        resp_cmd_free(&r.cmd);
    }
    result r = parse(p, left);
    CHECK(r.st == RESP_INCOMPLETE);
    CHECK(left == strlen("*2\r\n$3\r\nDEL")); /* 반쪽은 그대로 남음 */
}

/* TC-RESP-005 EG: 값 안의 \r\n, \0, 공백은 데이터 (학습 노트 Q13) */
static void test_binary_values(void)
{
    static const char in[] = "*3\r\n$3\r\nSET\r\n$3\r\na\0b\r\n$7\r\nab\r\ncde\r\n";
    result r = PARSE_S(in);
    CHECK(r.st == RESP_OK);
    CHECK(r.consumed == sizeof in - 1);
    CHECK(arg_is(&r.cmd, 1, "a\0b", 3));
    CHECK(arg_is(&r.cmd, 2, "ab\r\ncde", 7));
    resp_cmd_free(&r.cmd);

    r = PARSE_S("*2\r\n$4\r\nECHO\r\n$11\r\nhello world\r\n");
    CHECK(r.st == RESP_OK && r.cmd.argc == 2 && ARG_IS(&r.cmd, 1, "hello world"));
    resp_cmd_free(&r.cmd);
}

/* TC-RESP-006 BVA: 길이 0 벌크 */
static void test_empty_bulk(void)
{
    result r = PARSE_S("*2\r\n$3\r\nGET\r\n$0\r\n\r\n");
    CHECK(r.st == RESP_OK && r.cmd.argc == 2 && r.cmd.argvlen[1] == 0 && r.cmd.argv[1][0] == '\0');
    resp_cmd_free(&r.cmd);
}

/* TC-RESP-007 BVA: 원소 0개, -1개는 빈 명령(argc 0)으로 소비 */
static void test_empty_multibulk(void)
{
    result r = PARSE_S("*0\r\n*1\r\n$4\r\nPING\r\n");
    CHECK(r.st == RESP_OK && r.cmd.argc == 0 && r.consumed == 4);
    resp_cmd_free(&r.cmd);
    r = PARSE_S("*-1\r\n");
    CHECK(r.st == RESP_OK && r.cmd.argc == 0 && r.consumed == 5);
    resp_cmd_free(&r.cmd);
}

/* ---- 한도와 형식 오류 (학습 노트 Q15) ---- */

/* TC-RESP-008 BVA: 벌크 길이 512MB 는 기다리고, 1바이트 넘으면 ERR. 미리 잡지 않으니 512MB 선언만으로는 아무 일 없음 */
static void test_bulk_limit(void)
{
    result r = PARSE_S("*2\r\n$3\r\nGET\r\n$536870912\r\nabc");
    CHECK(r.st == RESP_INCOMPLETE);
    r = PARSE_S("*2\r\n$3\r\nGET\r\n$536870913\r\n");
    CHECK(err_has(&r, "invalid bulk length"));
    r = PARSE_S("*2\r\n$3\r\nGET\r\n$999999999999\r\n");
    CHECK(err_has(&r, "invalid bulk length"));
}

/* TC-RESP-009 EG: 숫자 넘침 */
static void test_number_overflow(void)
{
    result r = PARSE_S("*1\r\n$99999999999999999999\r\n");
    CHECK(r.st == RESP_ERR);
    r = PARSE_S("*99999999999999999999\r\n");
    CHECK(r.st == RESP_ERR);
}

/* TC-RESP-010 EG: 잘못된 벌크 길이 */
static void test_bad_bulk_length(void)
{
    const char *bad[] = {"*1\r\n$-5\r\n", "*1\r\n$-1\r\n", "*1\r\n$abc\r\n", "*1\r\n$\r\n", "*1\r\n$+5\r\n", "*1\r\n$5x\r\n", "*1\r\n$ 5\r\n"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        result r = parse(bad[i], strlen(bad[i]));
        CHECK(err_has(&r, "invalid bulk length"));
        CHECK(r.consumed == 0 && r.cmd.argc == 0);
    }
}

/* TC-RESP-011 BVA·EG: 배열 원소 수 */
static void test_multibulk_limit(void)
{
    result r = PARSE_S("*1048576\r\n$4\r\nPING\r\n");
    CHECK(r.st == RESP_INCOMPLETE); /* 한도 안: 나머지를 기다림 */
    r = PARSE_S("*1048577\r\n");
    CHECK(err_has(&r, "invalid multibulk length"));
    r = PARSE_S("*abc\r\n");
    CHECK(err_has(&r, "invalid multibulk length"));
    r = PARSE_S("*\r\n");
    CHECK(err_has(&r, "invalid multibulk length"));
}

/* TC-RESP-012 EG: 배열 원소가 $ 로 시작하지 않음 */
static void test_expected_dollar(void)
{
    result r = PARSE_S("*1\r\n:5\r\n");
    CHECK(err_has(&r, "expected '$', got ':'"));
}

/* TC-RESP-013 EG: 데이터 뒤 \r\n 자리에 다른 바이트 */
static void test_missing_crlf_after_bulk(void)
{
    result r = PARSE_S("*1\r\n$3\r\nfooXY");
    CHECK(r.st == RESP_ERR);
    r = PARSE_S("*1\r\n$3\r\nfoo\rX");
    CHECK(r.st == RESP_ERR);
    r = PARSE_S("*1\r\n$3\r\nfoo\r");
    CHECK(r.st == RESP_INCOMPLETE); /* 아직 판단할 수 없음 */
}

/* TC-RESP-014 BVA: 머리줄·인라인 줄이 \r\n 없이 64KB 를 넘으면 ERR, 그 안이면 기다림 */
static void test_header_too_long(void)
{
    size_t n = RESP_MAX_INLINE + 10;
    char *p = malloc(n);
    CHECK(p != NULL);

    p[0] = '*';
    memset(p + 1, '1', n - 1);
    result r = parse(p, (size_t)RESP_MAX_INLINE);
    CHECK(r.st == RESP_INCOMPLETE);
    r = parse(p, n);
    CHECK(err_has(&r, "too big mbulk count string"));

    memcpy(p, "*1\r\n$", 5);
    memset(p + 5, '1', n - 5);
    r = parse(p, n);
    CHECK(err_has(&r, "too big bulk count string"));

    memset(p, 'a', n);
    r = parse(p, (size_t)RESP_MAX_INLINE);
    CHECK(r.st == RESP_INCOMPLETE);
    r = parse(p, n);
    CHECK(err_has(&r, "too big inline request"));
    free(p);
}

/* ---- 인라인 명령 (D2) ---- */

/* TC-RESP-015 EP: 공백·탭으로 나눔, \r 은 선택, 빈 줄은 argc 0 */
static void test_inline(void)
{
    result r = PARSE_S("PING\r\n");
    CHECK(r.st == RESP_OK && r.consumed == 6 && r.cmd.argc == 1 && ARG_IS(&r.cmd, 0, "PING"));
    resp_cmd_free(&r.cmd);

    r = PARSE_S("  SET a\t\t b  \n");
    CHECK(r.st == RESP_OK && r.cmd.argc == 3);
    CHECK(ARG_IS(&r.cmd, 0, "SET") && ARG_IS(&r.cmd, 1, "a") && ARG_IS(&r.cmd, 2, "b"));
    resp_cmd_free(&r.cmd);

    r = PARSE_S("\r\n");
    CHECK(r.st == RESP_OK && r.cmd.argc == 0 && r.consumed == 2);
    resp_cmd_free(&r.cmd);

    r = PARSE_S("PIN");
    CHECK(r.st == RESP_INCOMPLETE && r.consumed == 0);
}

/* ---- 속성 기반: 무작위로 쪼개 보내도 결과가 같다 (Q11·Q12) ---- */

/* TC-RESP-016 속성: 명령 5개(바이너리, 인라인, 빈 배열 포함)를 무작위 크기 조각으로 500가지 방식으로 나눠 넣어도
 * 매번 같은 순서로 같은 명령 5개가 나온다 */
static void test_random_splits(void)
{
    static const char stream[] =
        "*3\r\n$3\r\nSET\r\n$3\r\nk\0y\r\n$7\r\nab\r\ncde\r\n"
        "PING\r\n"
        "*0\r\n"
        "*2\r\n$3\r\nGET\r\n$3\r\nk\0y\r\n"
        "*1\r\n$0\r\n\r\n";
    const size_t total = sizeof stream - 1;
    const int want_argc[] = {3, 1, 0, 2, 1};

    srand(7);
    for (int round = 0; round < 500; round++) {
        buf in;
        buf_init(&in);
        size_t fed = 0;
        int got = 0;
        while (fed < total || in.len > 0) {
            if (fed < total) {
                size_t chunk = 1 + (size_t)rand() % 9;
                if (chunk > total - fed)
                    chunk = total - fed;
                CHECK(buf_append(&in, stream + fed, chunk) == 0);
                fed += chunk;
            }
            for (;;) {
                result r = parse(in.data, in.len);
                if (r.st == RESP_INCOMPLETE)
                    break;
                CHECK(r.st == RESP_OK);
                CHECK(got < 5);
                CHECK(r.cmd.argc == want_argc[got]);
                if (got == 0)
                    CHECK(arg_is(&r.cmd, 2, "ab\r\ncde", 7));
                if (got == 3)
                    CHECK(arg_is(&r.cmd, 1, "k\0y", 3));
                got++;
                buf_consume(&in, r.consumed);
                resp_cmd_free(&r.cmd);
            }
            if (fed == total && in.len > 0) {
                CHECK(0); /* 다 넣었는데 남았다 = 파싱 실패 */
            }
        }
        CHECK(got == 5);
        buf_free(&in);
    }
}

/* ---- 응답 쓰기 ---- */

/* TC-RESP-017 EP·BVA: 응답 바이트 */
static void test_writers(void)
{
    buf o;
    buf_init(&o);
    CHECK(resp_simple(&o, "OK") == 0);
    CHECK(resp_error(&o, "ERR unknown command 'foo'") == 0);
    CHECK(resp_int(&o, 0) == 0);
    CHECK(resp_int(&o, -2) == 0);
    CHECK(resp_bulk(&o, "a\0b", 3) == 0);
    CHECK(resp_bulk(&o, "", 0) == 0);
    CHECK(resp_null(&o) == 0);
    CHECK(resp_array_len(&o, 0) == 0);
    CHECK(resp_array_len(&o, 2) == 0);
    static const char want[] =
        "+OK\r\n-ERR unknown command 'foo'\r\n:0\r\n:-2\r\n$3\r\na\0b\r\n$0\r\n\r\n$-1\r\n*0\r\n*2\r\n";
    CHECK(o.len == sizeof want - 1);
    CHECK(memcmp(o.data, want, o.len) == 0);
    buf_free(&o);
}

/* TC-RESP-018 EG: 빈 명령 해제, 두 번 해제 */
static void test_cmd_free_safe(void)
{
    resp_cmd c = {0, NULL, NULL};
    resp_cmd_free(&c);
    result r = PARSE_S("PING\r\n");
    resp_cmd_free(&r.cmd);
    resp_cmd_free(&r.cmd);
    CHECK(r.cmd.argc == 0 && r.cmd.argv == NULL);
}

int main(void)
{
    RUN(test_basic_set);
    RUN(test_every_prefix_incomplete);
    RUN(test_q14_header_is_not_element);
    RUN(test_pipeline);
    RUN(test_binary_values);
    RUN(test_empty_bulk);
    RUN(test_empty_multibulk);
    RUN(test_bulk_limit);
    RUN(test_number_overflow);
    RUN(test_bad_bulk_length);
    RUN(test_multibulk_limit);
    RUN(test_expected_dollar);
    RUN(test_missing_crlf_after_bulk);
    RUN(test_header_too_long);
    RUN(test_inline);
    RUN(test_random_splits);
    RUN(test_writers);
    RUN(test_cmd_free_safe);
    TEST_DONE();
    return 0;
}
