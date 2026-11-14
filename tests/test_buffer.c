#include <stddef.h>
#include <stdint.h>

#include "buffer.h"
#include "mem.h"
#include "test.h"

enum {
    MANY_APPEND_COUNT = 1000,
};

static void test_init_is_empty(void)
{
    struct buffer buf;
    buffer_init(&buf);
    CHECK(buf.data == NULL && buf.len == 0 && buf.cap == 0);
    buffer_deinit(&buf);
}

static void test_append_keeps_bytes_in_order(void)
{
    struct buffer buf;
    buffer_init(&buf);
    CHECK(buffer_append_text(&buf, "+OK") == BUFFER_OK);
    CHECK(buffer_append(&buf, "\r\n", 2) == BUFFER_OK);
    CHECK(buf.len == strlen("+OK\r\n"));
    CHECK(memcmp(buf.data, "+OK\r\n", buf.len) == 0);
    buffer_deinit(&buf);
}

static void test_append_with_nul_keeps_length(void)
{
    static const char data[] = {'a', '\0', 'b'};
    struct buffer buf;
    buffer_init(&buf);
    CHECK(buffer_append(&buf, data, sizeof(data)) == BUFFER_OK);
    CHECK(buf.len == sizeof(data));
    CHECK(memcmp(buf.data, data, sizeof(data)) == 0);
    buffer_deinit(&buf);
}

static void test_append_empty_does_not_allocate(void)
{
    struct buffer buf;
    buffer_init(&buf);
    CHECK(buffer_append(&buf, NULL, 0) == BUFFER_OK);
    CHECK(buf.data == NULL && buf.cap == 0);
    buffer_deinit(&buf);
}

static void test_many_appends_grow_and_keep_content(void)
{
    struct buffer buf;
    buffer_init(&buf);
    for (int i = 0; i < MANY_APPEND_COUNT; i++) {
        char c = (char)('a' + i % 26);
        CHECK(buffer_append(&buf, &c, 1) == BUFFER_OK);
    }
    CHECK(buf.len == MANY_APPEND_COUNT);
    CHECK(buf.cap >= buf.len);
    for (int i = 0; i < MANY_APPEND_COUNT; i++) {
        CHECK(buf.data[i] == (char)('a' + i % 26));
    }
    buffer_deinit(&buf);
}

static void test_append_overflowing_length_returns_out_of_memory(void)
{
    struct buffer buf;
    buffer_init(&buf);
    CHECK(buffer_append_text(&buf, "x") == BUFFER_OK);
    CHECK(buffer_append(&buf, "y", SIZE_MAX) == BUFFER_OUT_OF_MEMORY);
    CHECK(buf.len == 1 && buf.data[0] == 'x');
    buffer_deinit(&buf);
}

static void test_clear_keeps_capacity(void)
{
    struct buffer buf;
    buffer_init(&buf);
    CHECK(buffer_append_text(&buf, "hello") == BUFFER_OK);
    size_t cap = buf.cap;
    buffer_clear(&buf);
    CHECK(buf.len == 0 && buf.cap == cap);
    buffer_deinit(&buf);
}

static void test_deinit_returns_used_bytes_to_baseline(void)
{
    size_t baseline_bytes = mem_used_bytes();
    struct buffer buf;
    buffer_init(&buf);
    for (int i = 0; i < MANY_APPEND_COUNT; i++) {
        CHECK(buffer_append_text(&buf, "abc") == BUFFER_OK);
    }
    buffer_deinit(&buf);
    CHECK(mem_used_bytes() == baseline_bytes);
}

int main(void)
{
    RUN(test_init_is_empty);
    RUN(test_append_keeps_bytes_in_order);
    RUN(test_append_with_nul_keeps_length);
    RUN(test_append_empty_does_not_allocate);
    RUN(test_many_appends_grow_and_keep_content);
    RUN(test_append_overflowing_length_returns_out_of_memory);
    RUN(test_clear_keeps_capacity);
    RUN(test_deinit_returns_used_bytes_to_baseline);
    TEST_DONE();
    return 0;
}
