#include "mem.h"

#include <stdint.h>
#include <stdlib.h>

/* 헤더를 max_align_t 크기로 맞춰서 헤더 뒤 블록이 어떤 타입이든 담을 수 있게 한다 */
union mem_header {
    size_t size_bytes;
    max_align_t align;
};

static size_t g_used_bytes = 0;

static void *track_block(union mem_header *header, size_t size_bytes);

void *mem_alloc(size_t size_bytes)
{
    if (size_bytes > SIZE_MAX - sizeof(union mem_header)) {
        return NULL;
    }
    union mem_header *header = malloc(sizeof(*header) + size_bytes);
    return track_block(header, size_bytes);
}

void *mem_calloc(size_t count, size_t size_bytes)
{
    if (size_bytes != 0 && count > (SIZE_MAX - sizeof(union mem_header)) / size_bytes) {
        return NULL;
    }
    size_t total_bytes = count * size_bytes;
    union mem_header *header = calloc(1, sizeof(*header) + total_bytes);
    return track_block(header, total_bytes);
}

void mem_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    union mem_header *header = (union mem_header *)ptr - 1;
    g_used_bytes -= header->size_bytes;
    free(header);
}

size_t mem_used_bytes(void)
{
    return g_used_bytes;
}

static void *track_block(union mem_header *header, size_t size_bytes)
{
    if (header == NULL) {
        return NULL;
    }
    header->size_bytes = size_bytes;
    g_used_bytes += size_bytes;
    return header + 1;
}
