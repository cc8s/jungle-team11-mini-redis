#ifndef BUF_H
#define BUF_H

/* 늘어나는 바이트 버퍼. 클라이언트 입력·출력, 응답 작성, AOF 가 같이 쓴다.
 *  - 바이너리 안전: data 안에 \0 이 있어도 된다. len 이 진짜 길이다.
 *  - 공간이 모자라면 2배씩 늘린다. 선언된 길이를 믿고 미리 크게 잡지 않는다(도착한 만큼만).
 *  - data[len] 은 항상 \0 이다(디버깅·문자열 비교 편의). cap 은 그 1바이트를 뺀 크기다. */

#include <stdarg.h>
#include <stddef.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} buf;

void buf_init(buf *b);  /* 빈 버퍼. 메모리는 처음 append 할 때 잡는다 */
void buf_free(buf *b);  /* 해제하고 빈 버퍼로 */
int buf_append(buf *b, const char *p, size_t n);          /* 실패 -1 (버퍼는 그대로) */
int buf_appendf(buf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));                /* 실패 -1 */
void buf_consume(buf *b, size_t n);                       /* 앞에서 n 바이트 버림. n >= len 이면 비움 */
void buf_clear(buf *b);                                   /* len = 0, 메모리는 유지 */

#endif
