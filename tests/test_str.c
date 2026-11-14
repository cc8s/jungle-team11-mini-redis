#include <stdint.h>

#include "mem.h"
#include "str.h"
#include "test.h"

static struct str *create_from_text(const char *text)
{
    return str_create(text, strlen(text));
}

static void test_create_copies_bytes_and_length(void)
{
    static const char text[] = "hello";
    struct str *s = create_from_text(text);
    CHECK(s != NULL);
    CHECK(s->len == strlen(text));
    CHECK(memcmp(s->data, text, strlen(text)) == 0);
    str_destroy(s);
}

static void test_create_with_nul_byte_keeps_full_length(void)
{
    static const char raw[] = {'a', '\0', 'b', '\0'};
    struct str *s = str_create(raw, sizeof(raw));
    CHECK(s != NULL);
    CHECK(s->len == sizeof(raw));
    CHECK(memcmp(s->data, raw, sizeof(raw)) == 0);
    str_destroy(s);
}

static void test_create_does_not_share_source_buffer(void)
{
    char buf[] = "abc";
    struct str *s = create_from_text(buf);
    CHECK(s != NULL);
    buf[0] = 'z';
    CHECK(s->data[0] == 'a');
    str_destroy(s);
}

static void test_create_empty_returns_zero_length(void)
{
    struct str *s = create_from_text("");
    CHECK(s != NULL);
    CHECK(s->len == 0);
    CHECK(s->data[0] == '\0');
    str_destroy(s);
}

static void test_create_null_data_with_zero_length_succeeds(void)
{
    struct str *s = str_create(NULL, 0);
    CHECK(s != NULL);
    CHECK(s->len == 0);
    str_destroy(s);
}

static void test_create_null_data_with_length_returns_null(void)
{
    CHECK(str_create(NULL, 1) == NULL);
}

static void test_create_overflowing_length_returns_null(void)
{
    CHECK(str_create("x", SIZE_MAX) == NULL);
}

static void test_dup_returns_equal_independent_copy(void)
{
    static const char raw[] = {'k', '\0', 'v'};
    struct str *src = str_create(raw, sizeof(raw));
    struct str *copy = str_dup(src);
    CHECK(copy != NULL);
    CHECK(copy != src);
    CHECK(str_equal(src, copy));
    str_destroy(src);
    CHECK(copy->len == sizeof(raw));
    CHECK(memcmp(copy->data, raw, sizeof(raw)) == 0);
    str_destroy(copy);
}

static void test_destroy_null_does_nothing(void)
{
    str_destroy(NULL);
    CHECK(1);
}

static void test_equal_same_bytes_returns_true(void)
{
    struct str *a = create_from_text("key");
    struct str *b = create_from_text("key");
    CHECK(str_equal(a, b));
    str_destroy(a);
    str_destroy(b);
}

static void test_equal_different_length_returns_false(void)
{
    struct str *a = create_from_text("key");
    struct str *b = create_from_text("keys");
    CHECK(!str_equal(a, b));
    str_destroy(a);
    str_destroy(b);
}

static void test_equal_different_bytes_returns_false(void)
{
    struct str *a = create_from_text("key");
    struct str *b = create_from_text("kez");
    CHECK(!str_equal(a, b));
    str_destroy(a);
    str_destroy(b);
}

static void test_equal_differs_after_nul_returns_false(void)
{
    static const char raw_a[] = {'a', '\0', 'b'};
    static const char raw_b[] = {'a', '\0', 'c'};
    struct str *a = str_create(raw_a, sizeof(raw_a));
    struct str *b = str_create(raw_b, sizeof(raw_b));
    CHECK(!str_equal(a, b));
    str_destroy(a);
    str_destroy(b);
}

static void test_equal_empty_strings_returns_true(void)
{
    struct str *a = str_create(NULL, 0);
    struct str *b = create_from_text("");
    CHECK(str_equal(a, b));
    str_destroy(a);
    str_destroy(b);
}

static void test_create_adds_header_and_bytes_to_used_bytes(void)
{
    size_t before_bytes = mem_used_bytes();
    struct str *s = create_from_text("hello");
    CHECK(mem_used_bytes() == before_bytes + sizeof(*s) + s->len + 1);
    str_destroy(s);
}

static void test_destroy_returns_used_bytes_to_baseline(void)
{
    size_t before_bytes = mem_used_bytes();
    struct str *s = create_from_text("hello");
    struct str *copy = str_dup(s);
    str_destroy(s);
    str_destroy(copy);
    CHECK(mem_used_bytes() == before_bytes);
}

int main(void)
{
    RUN(test_create_copies_bytes_and_length);
    RUN(test_create_with_nul_byte_keeps_full_length);
    RUN(test_create_does_not_share_source_buffer);
    RUN(test_create_empty_returns_zero_length);
    RUN(test_create_null_data_with_zero_length_succeeds);
    RUN(test_create_null_data_with_length_returns_null);
    RUN(test_create_overflowing_length_returns_null);
    RUN(test_dup_returns_equal_independent_copy);
    RUN(test_destroy_null_does_nothing);
    RUN(test_equal_same_bytes_returns_true);
    RUN(test_equal_different_length_returns_false);
    RUN(test_equal_different_bytes_returns_false);
    RUN(test_equal_differs_after_nul_returns_false);
    RUN(test_equal_empty_strings_returns_true);
    RUN(test_create_adds_header_and_bytes_to_used_bytes);
    RUN(test_destroy_returns_used_bytes_to_baseline);
    TEST_DONE();
    return 0;
}
