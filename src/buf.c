#include "buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void buf_init(buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buf_free(buf *b)
{
    free(b->data);
    buf_init(b);
}

/* len + extra 바이트(+ 끝 \0)가 들어갈 공간을 확보한다 */
static int reserve(buf *b, size_t extra)
{
    if (extra > (size_t)-1 / 2 - b->len)
        return -1;
    size_t need = b->len + extra;
    if (b->data != NULL && need <= b->cap)
        return 0;
    size_t cap = b->cap > 0 ? b->cap : 16;
    while (cap < need)
        cap *= 2;
    char *p = realloc(b->data, cap + 1);
    if (p == NULL)
        return -1;
    b->data = p;
    b->cap = cap;
    return 0;
}

int buf_append(buf *b, const char *p, size_t n)
{
    if (reserve(b, n) != 0)
        return -1;
    if (n > 0)
        memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int buf_appendf(buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || reserve(b, (size_t)n) != 0)
        return -1;
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    return 0;
}

void buf_consume(buf *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
    } else if (n > 0) {
        memmove(b->data, b->data + n, b->len - n);
        b->len -= n;
    }
    if (b->data != NULL)
        b->data[b->len] = '\0';
}

void buf_clear(buf *b)
{
    b->len = 0;
    if (b->data != NULL)
        b->data[0] = '\0';
}
