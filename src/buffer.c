#include "buffer.h"

#include <stdint.h>
#include <string.h>

#include "mem.h"

static enum buffer_result reserve(struct buffer *buf, size_t extra_len);

void buffer_init(struct buffer *buf)
{
    *buf = (struct buffer){0};
}

void buffer_deinit(struct buffer *buf)
{
    mem_free(buf->data);
    buffer_init(buf);
}

enum buffer_result buffer_append(struct buffer *buf, const char *data, size_t len)
{
    if (len == 0) {
        return BUFFER_OK;
    }
    if (reserve(buf, len) != BUFFER_OK) {
        return BUFFER_OUT_OF_MEMORY;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return BUFFER_OK;
}

enum buffer_result buffer_append_text(struct buffer *buf, const char *text)
{
    return buffer_append(buf, text, strlen(text));
}

void buffer_clear(struct buffer *buf)
{
    buf->len = 0;
}

/* 2배씩 늘려 append 를 평균 O(1) 로 만든다. mem 에 realloc 이 없어 새로 할당하고 복사한다.
 * 실패하면 기존 data 를 그대로 두므로 버퍼 내용이 사라지지 않는다 */
static enum buffer_result reserve(struct buffer *buf, size_t extra_len)
{
    if (extra_len > SIZE_MAX - buf->len) {
        return BUFFER_OUT_OF_MEMORY;
    }
    size_t need_cap = buf->len + extra_len;
    if (need_cap <= buf->cap) {
        return BUFFER_OK;
    }
    size_t new_cap = buf->cap == 0 ? BUFFER_INITIAL_CAP : buf->cap;
    while (new_cap < need_cap) {
        new_cap = new_cap > SIZE_MAX / 2 ? need_cap : new_cap * 2;
    }

    char *new_data = mem_alloc(new_cap);
    if (new_data == NULL) {
        return BUFFER_OUT_OF_MEMORY;
    }
    if (buf->len > 0) {
        memcpy(new_data, buf->data, buf->len);
    }
    mem_free(buf->data);
    buf->data = new_data;
    buf->cap = new_cap;
    return BUFFER_OK;
}
