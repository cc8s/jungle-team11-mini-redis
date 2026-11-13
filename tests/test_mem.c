#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mem.h"
#include "test.h"

enum {
    BLOCK_BYTES = 100,
    ELEMENT_COUNT = 10,
    BLOCK_COUNT = 5,
    FILL_BYTE = 0xAB,
};

static void test_alloc_adds_size_to_used_bytes(void)
{
    size_t before_bytes = mem_used_bytes();
    void *block = mem_alloc(BLOCK_BYTES);
    CHECK(block != NULL);
    CHECK(mem_used_bytes() == before_bytes + BLOCK_BYTES);
    mem_free(block);
}

static void test_free_returns_used_bytes_to_baseline(void)
{
    size_t before_bytes = mem_used_bytes();
    void *block = mem_alloc(BLOCK_BYTES);
    mem_free(block);
    CHECK(mem_used_bytes() == before_bytes);
}

static void test_alloc_block_is_writable(void)
{
    unsigned char *block = mem_alloc(BLOCK_BYTES);
    CHECK(block != NULL);
    memset(block, FILL_BYTE, BLOCK_BYTES);
    CHECK(block[0] == FILL_BYTE && block[BLOCK_BYTES - 1] == FILL_BYTE);
    mem_free(block);
}

static void test_alloc_returns_max_aligned_pointer(void)
{
    void *block = mem_alloc(1);
    CHECK(block != NULL);
    CHECK((uintptr_t)block % _Alignof(max_align_t) == 0);
    mem_free(block);
}

static void test_alloc_zero_bytes_is_freeable(void)
{
    size_t before_bytes = mem_used_bytes();
    void *block = mem_alloc(0);
    CHECK(block != NULL);
    CHECK(mem_used_bytes() == before_bytes);
    mem_free(block);
    CHECK(mem_used_bytes() == before_bytes);
}

static void test_alloc_overflowing_size_returns_null(void)
{
    size_t before_bytes = mem_used_bytes();
    CHECK(mem_alloc(SIZE_MAX) == NULL);
    CHECK(mem_used_bytes() == before_bytes);
}

static void test_calloc_zero_fills_block(void)
{
    int *values = mem_calloc(ELEMENT_COUNT, sizeof(*values));
    CHECK(values != NULL);
    bool is_all_zero = true;
    for (int i = 0; i < ELEMENT_COUNT; i++) {
        if (values[i] != 0) {
            is_all_zero = false;
        }
    }
    CHECK(is_all_zero);
    mem_free(values);
}

static void test_calloc_adds_total_size_to_used_bytes(void)
{
    size_t before_bytes = mem_used_bytes();
    int *values = mem_calloc(ELEMENT_COUNT, sizeof(*values));
    CHECK(mem_used_bytes() == before_bytes + ELEMENT_COUNT * sizeof(*values));
    mem_free(values);
    CHECK(mem_used_bytes() == before_bytes);
}

static void test_calloc_overflowing_count_returns_null(void)
{
    size_t before_bytes = mem_used_bytes();
    CHECK(mem_calloc(SIZE_MAX / 2, sizeof(uint32_t)) == NULL);
    CHECK(mem_used_bytes() == before_bytes);
}

static void test_free_null_keeps_used_bytes(void)
{
    size_t before_bytes = mem_used_bytes();
    mem_free(NULL);
    CHECK(mem_used_bytes() == before_bytes);
}

static void test_free_in_any_order_returns_to_baseline(void)
{
    size_t before_bytes = mem_used_bytes();
    void *blocks[BLOCK_COUNT];
    for (size_t i = 0; i < BLOCK_COUNT; i++) {
        blocks[i] = mem_alloc((i + 1) * BLOCK_BYTES);
    }
    for (size_t i = 0; i < BLOCK_COUNT; i += 2) {
        mem_free(blocks[i]);
    }
    for (size_t i = 1; i < BLOCK_COUNT; i += 2) {
        mem_free(blocks[i]);
    }
    CHECK(mem_used_bytes() == before_bytes);
}

int main(void)
{
    RUN(test_alloc_adds_size_to_used_bytes);
    RUN(test_free_returns_used_bytes_to_baseline);
    RUN(test_alloc_block_is_writable);
    RUN(test_alloc_returns_max_aligned_pointer);
    RUN(test_alloc_zero_bytes_is_freeable);
    RUN(test_alloc_overflowing_size_returns_null);
    RUN(test_calloc_zero_fills_block);
    RUN(test_calloc_adds_total_size_to_used_bytes);
    RUN(test_calloc_overflowing_count_returns_null);
    RUN(test_free_null_keeps_used_bytes);
    RUN(test_free_in_any_order_returns_to_baseline);
    TEST_DONE();
    return 0;
}
