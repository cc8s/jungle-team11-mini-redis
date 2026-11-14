#ifndef MINI_REDIS_BUFFER_H
#define MINI_REDIS_BUFFER_H

#include <stddef.h>

enum {
    BUFFER_INITIAL_CAP = 64,
};

enum buffer_result {
    BUFFER_OK = 0,
    BUFFER_OUT_OF_MEMORY,
};

/* 뒤에 이어 붙이는 바이트 버퍼. len 은 쓴 바이트 수, cap 은 할당한 크기다.
 * 불변식: cap 이 0 이면 data 는 NULL 이고, 그 밖에는 len <= cap 이다. */
struct buffer {
    char *data;
    size_t len;
    size_t cap;
};

/* 빈 버퍼로 초기화한다. 할당은 처음 append 할 때 한다. */
void buffer_init(struct buffer *buf);

/* 데이터를 해제하고 빈 버퍼로 되돌린다. */
void buffer_deinit(struct buffer *buf);

/* data 의 len 바이트를 뒤에 붙인다. 할당이 실패하면 버퍼는 그대로이고 BUFFER_OUT_OF_MEMORY. */
enum buffer_result buffer_append(struct buffer *buf, const char *data, size_t len);

/* C 문자열을 뒤에 붙인다. 끝의 \0 은 붙이지 않는다. */
enum buffer_result buffer_append_text(struct buffer *buf, const char *text);

/* 내용만 비운다. 할당한 공간은 다시 쓰려고 남겨 둔다. */
void buffer_clear(struct buffer *buf);

#endif
