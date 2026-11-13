#ifndef MINI_REDIS_STR_H
#define MINI_REDIS_STR_H

#include <stdbool.h>
#include <stddef.h>

/* 길이를 함께 저장하는 바이너리 안전 문자열 (data 에 \0 이 들어갈 수 있다) */
struct str {
    size_t len;
    char data[];
};

/* data 의 len 바이트를 복사해 새 문자열을 만든다. 실패하면 NULL, str_destroy() 로 해제한다. */
struct str *str_create(const char *data, size_t len);

/* src 의 복사본을 만든다. 실패하면 NULL, str_destroy() 로 해제한다. */
struct str *str_dup(const struct str *src);

/* 문자열을 해제한다. NULL 이면 아무것도 하지 않는다. */
void str_destroy(struct str *s);

/* 길이와 바이트가 모두 같으면 true 를 반환한다. */
bool str_equal(const struct str *a, const struct str *b);

#endif
