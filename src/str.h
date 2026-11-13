#ifndef MINI_REDIS_STR_H
#define MINI_REDIS_STR_H

#include <stdbool.h>
#include <stddef.h>

/* 길이를 함께 들고 다니는 바이너리 안전 문자열.
 * data 에는 \0 이 들어갈 수 있으므로 길이는 항상 len 으로 판단한다.
 * 디버깅 출력을 위해 data[len] 에 \0 을 하나 더 두지만, 내용의 일부는 아니다.
 * 한 번 만들면 내용을 바꾸지 않는다. 바꿔야 하면 새로 만든다. */
struct str {
    size_t len;
    char data[];
};

/* data 의 앞 len 바이트를 복사해 새 문자열을 만든다. len 이 0 이면 data 는 NULL 이어도 된다.
 * 실패하면 NULL. 반환된 문자열은 호출자가 str_destroy() 로 해제한다. */
struct str *str_create(const char *data, size_t len);

/* src 의 복사본을 만든다. 실패하면 NULL. 반환된 문자열은 호출자가 str_destroy() 로 해제한다. */
struct str *str_dup(const struct str *src);

/* s 를 해제한다. NULL 이면 아무것도 하지 않는다. */
void str_destroy(struct str *s);

/* 길이와 모든 바이트가 같으면 true. a, b 는 NULL 이 아니어야 한다. */
bool str_equal(const struct str *a, const struct str *b);

#endif
