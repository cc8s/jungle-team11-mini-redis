#include "str.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct str *str_create(const char *data, size_t len)
{
    if (len > SIZE_MAX - sizeof(struct str) - 1) {
        return NULL;
    }
    if (data == NULL && len > 0) {
        return NULL;
    }

    struct str *s = malloc(sizeof(*s) + len + 1);
    if (s == NULL) {
        return NULL;
    }
    s->len = len;
    if (len > 0) {
        memcpy(s->data, data, len);
    }
    s->data[len] = '\0';
    return s;
}

struct str *str_dup(const struct str *src)
{
    return str_create(src->data, src->len);
}

void str_destroy(struct str *s)
{
    free(s);
}

bool str_equal(const struct str *a, const struct str *b)
{
    if (a->len != b->len) {
        return false;
    }
    return memcmp(a->data, b->data, a->len) == 0;
}
