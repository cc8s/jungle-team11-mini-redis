#include "test.h"

#include "buf.h"

/* TC-BUF-001 EP: 처음 상태 */
static void test_init_empty(void)
{
    buf b;
    buf_init(&b);
    CHECK(b.len == 0);
    buf_free(&b);
    CHECK(b.len == 0 && b.data == NULL);
    buf_free(&b); /* 두 번 해제해도 안전 */
}

/* TC-BUF-002 EG: 바이너리 안전, 끝에 항상 \0 */
static void test_append_binary(void)
{
    buf b;
    buf_init(&b);
    CHECK(buf_append(&b, "a\0b", 3) == 0);
    CHECK(buf_append(&b, "\r\n", 2) == 0);
    CHECK(b.len == 5);
    CHECK(memcmp(b.data, "a\0b\r\n", 5) == 0);
    CHECK(b.data[b.len] == '\0');
    CHECK(buf_append(&b, NULL, 0) == 0); /* 0 바이트 */
    CHECK(b.len == 5);
    buf_free(&b);
}

/* TC-BUF-003 EP: 1바이트씩 10만 번 → 여러 번 늘어나도 내용 그대로 */
static void test_growth(void)
{
    buf b;
    buf_init(&b);
    for (int i = 0; i < 100000; i++) {
        char c = (char)('a' + i % 26);
        CHECK(buf_append(&b, &c, 1) == 0);
    }
    CHECK(b.len == 100000);
    CHECK(b.cap >= b.len);
    for (int i = 0; i < 100000; i += 997)
        CHECK(b.data[i] == (char)('a' + i % 26));
    CHECK(b.data[b.len] == '\0');
    buf_free(&b);
}

/* TC-BUF-004 EP: 형식 문자열 */
static void test_appendf(void)
{
    buf b;
    buf_init(&b);
    CHECK(buf_appendf(&b, ":%lld\r\n", -9223372036854775807LL - 1) == 0);
    CHECK_STR_EQ(b.data, ":-9223372036854775808\r\n");
    CHECK(buf_appendf(&b, "$%zu\r\n", (size_t)0) == 0);
    CHECK_STR_EQ(b.data, ":-9223372036854775808\r\n$0\r\n");
    buf_free(&b);
}

/* TC-BUF-005 BVA: 앞에서 버리기 — 일부, 정확히 전부, 전부보다 많이 */
static void test_consume(void)
{
    buf b;
    buf_init(&b);
    buf_append(&b, "hello world", 11);
    buf_consume(&b, 6);
    CHECK(b.len == 5);
    CHECK_STR_EQ(b.data, "world");
    buf_consume(&b, 0);
    CHECK(b.len == 5);
    buf_consume(&b, 5);
    CHECK(b.len == 0);
    CHECK(b.data == NULL || b.data[0] == '\0');
    buf_append(&b, "abc", 3);
    buf_consume(&b, 100);
    CHECK(b.len == 0);
    buf_append(&b, "x", 1);
    CHECK_STR_EQ(b.data, "x");
    buf_free(&b);
}

/* TC-BUF-006 ST: 비워도 메모리는 남아 다시 쓴다 */
static void test_clear_keeps_capacity(void)
{
    buf b;
    buf_init(&b);
    buf_append(&b, "0123456789", 10);
    size_t cap = b.cap;
    buf_clear(&b);
    CHECK(b.len == 0 && b.cap == cap);
    buf_append(&b, "ab", 2);
    CHECK_STR_EQ(b.data, "ab");
    buf_free(&b);
}

int main(void)
{
    RUN(test_init_empty);
    RUN(test_append_binary);
    RUN(test_growth);
    RUN(test_appendf);
    RUN(test_consume);
    RUN(test_clear_keeps_capacity);
    TEST_DONE();
    return 0;
}
