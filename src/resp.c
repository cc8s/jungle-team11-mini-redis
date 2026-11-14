#include "resp.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 숫자와 줄 ---- */

/* 10진 정수만 허용: '-' 하나(선택) + 숫자 1개 이상. 넘치면 실패. 성공 0 */
static int parse_ll(const char *p, size_t n, long long *out)
{
    size_t i = 0;
    int neg = 0;
    if (n > 0 && p[0] == '-') {
        neg = 1;
        i = 1;
    }
    if (i == n)
        return -1;
    unsigned long long v = 0;
    const unsigned long long limit = neg ? (unsigned long long)LLONG_MAX + 1 : (unsigned long long)LLONG_MAX;
    for (; i < n; i++) {
        if (p[i] < '0' || p[i] > '9')
            return -1;
        unsigned d = (unsigned)(p[i] - '0');
        if (v > (limit - d) / 10)
            return -1;
        v = v * 10 + d;
    }
    *out = neg ? (long long)(0 - v) : (long long)v;
    return 0;
}

/* data[from..len) 에서 "\r\n" 의 위치. 없으면 -1 */
static long find_crlf(const char *data, size_t from, size_t len)
{
    for (size_t i = from; i + 1 < len; i++)
        if (data[i] == '\r' && data[i + 1] == '\n')
            return (long)i;
    return -1;
}

/* ---- 명령 채우기 ---- */

static int cmd_alloc(resp_cmd *c, int argc)
{
    c->argc = 0;
    c->argv = NULL;
    c->argvlen = NULL;
    if (argc == 0)
        return 0;
    c->argv = calloc((size_t)argc, sizeof *c->argv);
    c->argvlen = calloc((size_t)argc, sizeof *c->argvlen);
    if (c->argv == NULL || c->argvlen == NULL) {
        free(c->argv);
        free(c->argvlen);
        c->argv = NULL;
        c->argvlen = NULL;
        return -1;
    }
    return 0;
}

static int cmd_push(resp_cmd *c, const char *p, size_t n)
{
    char *s = malloc(n + 1);
    if (s == NULL)
        return -1;
    if (n > 0)
        memcpy(s, p, n);
    s[n] = '\0';
    c->argv[c->argc] = s;
    c->argvlen[c->argc] = n;
    c->argc++;
    return 0;
}

void resp_cmd_free(resp_cmd *c)
{
    for (int i = 0; i < c->argc; i++)
        free(c->argv[i]);
    free(c->argv);
    free(c->argvlen);
    c->argc = 0;
    c->argv = NULL;
    c->argvlen = NULL;
}

/* ---- 인라인 ---- */

static resp_status parse_inline(const char *data, size_t len, resp_cmd *out, size_t *consumed, const char **errmsg)
{
    const char *nl = memchr(data, '\n', len);
    if (nl == NULL) {
        if (len > (size_t)RESP_MAX_INLINE) {
            *errmsg = "ERR Protocol error: too big inline request";
            return RESP_ERR;
        }
        return RESP_INCOMPLETE;
    }
    size_t end = (size_t)(nl - data);
    size_t line_end = (end > 0 && data[end - 1] == '\r') ? end - 1 : end;

    /* 1차: 단어 수 세기 */
    int words = 0;
    for (size_t i = 0; i < line_end;) {
        while (i < line_end && (data[i] == ' ' || data[i] == '\t'))
            i++;
        if (i == line_end)
            break;
        words++;
        while (i < line_end && data[i] != ' ' && data[i] != '\t')
            i++;
    }
    /* 2차: 복사 */
    if (cmd_alloc(out, words) != 0)
        goto oom;
    for (size_t i = 0; i < line_end;) {
        while (i < line_end && (data[i] == ' ' || data[i] == '\t'))
            i++;
        if (i == line_end)
            break;
        size_t start = i;
        while (i < line_end && data[i] != ' ' && data[i] != '\t')
            i++;
        if (cmd_push(out, data + start, i - start) != 0)
            goto oom;
    }
    *consumed = end + 1;
    return RESP_OK;
oom:
    resp_cmd_free(out);
    *errmsg = "ERR out of memory";
    return RESP_ERR;
}

/* ---- 멀티벌크 ---- */

/* 머리줄 "<c><숫자>\r\n" 을 data[pos] 에서 읽는다. 성공하면 *num, *next. */
static resp_status read_header(const char *data, size_t len, size_t pos, long long *num, size_t *next,
                               int *bad_number)
{
    long crlf = find_crlf(data, pos, len);
    if (crlf < 0) {
        if (len - pos > (size_t)RESP_MAX_INLINE) {
            *bad_number = 0;
            return RESP_ERR;
        }
        return RESP_INCOMPLETE;
    }
    if (parse_ll(data + pos + 1, (size_t)crlf - pos - 1, num) != 0) {
        *bad_number = 1;
        return RESP_ERR;
    }
    *next = (size_t)crlf + 2;
    return RESP_OK;
}

/* copy = 0: 검사만(할당 없음), copy = 1: out 에 복사. 둘 다 같은 길을 걷는다 */
static resp_status walk_multibulk(const char *data, size_t len, resp_cmd *out, size_t *consumed,
                                  const char **errmsg, int copy)
{
    static char expected_msg[64];
    long long count;
    size_t pos;
    int bad_number = 0;

    resp_status st = read_header(data, len, 0, &count, &pos, &bad_number);
    if (st == RESP_INCOMPLETE)
        return st;
    if (st == RESP_ERR) {
        *errmsg = bad_number ? "ERR Protocol error: invalid multibulk length"
                             : "ERR Protocol error: too big mbulk count string";
        return RESP_ERR;
    }
    if (count > RESP_MAX_MULTIBULK) {
        *errmsg = "ERR Protocol error: invalid multibulk length";
        return RESP_ERR;
    }
    if (count <= 0) { /* *0, *-1: 빈 명령 */
        if (copy)
            cmd_alloc(out, 0);
        *consumed = pos;
        return RESP_OK;
    }
    if (copy && cmd_alloc(out, (int)count) != 0)
        goto oom;

    for (long long i = 0; i < count; i++) {
        if (pos >= len)
            return RESP_INCOMPLETE;
        if (data[pos] != '$') {
            snprintf(expected_msg, sizeof expected_msg, "ERR Protocol error: expected '$', got '%c'", data[pos]);
            *errmsg = expected_msg;
            return RESP_ERR;
        }
        long long blen;
        st = read_header(data, len, pos, &blen, &pos, &bad_number);
        if (st == RESP_INCOMPLETE)
            return st;
        if (st == RESP_ERR) {
            *errmsg = bad_number ? "ERR Protocol error: invalid bulk length"
                                 : "ERR Protocol error: too big bulk count string";
            return RESP_ERR;
        }
        if (blen < 0 || blen > RESP_MAX_BULK) {
            *errmsg = "ERR Protocol error: invalid bulk length";
            return RESP_ERR;
        }
        size_t n = (size_t)blen;
        if (len - pos < n + 2)
            return RESP_INCOMPLETE;
        if (data[pos + n] != '\r' || data[pos + n + 1] != '\n') {
            *errmsg = "ERR Protocol error: expected CRLF after bulk data";
            return RESP_ERR;
        }
        if (copy && cmd_push(out, data + pos, n) != 0)
            goto oom;
        pos += n + 2;
    }
    *consumed = pos;
    return RESP_OK;
oom:
    *errmsg = "ERR out of memory";
    return RESP_ERR;
}

resp_status resp_parse(const char *data, size_t len, resp_cmd *out, size_t *consumed, const char **errmsg)
{
    out->argc = 0;
    out->argv = NULL;
    out->argvlen = NULL;
    *consumed = 0;
    *errmsg = NULL;
    if (len == 0)
        return RESP_INCOMPLETE;
    if (data[0] != '*')
        return parse_inline(data, len, out, consumed, errmsg);

    /* 1차: 할당 없이 끝까지 검사 → 완성됐을 때만 2차에서 복사 */
    size_t used = 0;
    resp_status st = walk_multibulk(data, len, out, &used, errmsg, 0);
    if (st != RESP_OK)
        return st;
    st = walk_multibulk(data, len, out, &used, errmsg, 1);
    if (st != RESP_OK) {
        resp_cmd_free(out);
        return st;
    }
    *consumed = used;
    return RESP_OK;
}

/* ---- 응답 ---- */

int resp_simple(buf *o, const char *s)
{
    return buf_appendf(o, "+%s\r\n", s);
}

int resp_error(buf *o, const char *msg)
{
    return buf_appendf(o, "-%s\r\n", msg);
}

int resp_int(buf *o, long long n)
{
    return buf_appendf(o, ":%lld\r\n", n);
}

int resp_bulk(buf *o, const char *p, size_t n)
{
    if (buf_appendf(o, "$%zu\r\n", n) != 0 || buf_append(o, p, n) != 0)
        return -1;
    return buf_append(o, "\r\n", 2);
}

int resp_null(buf *o)
{
    return buf_append(o, "$-1\r\n", 5);
}

int resp_array_len(buf *o, long n)
{
    return buf_appendf(o, "*%ld\r\n", n);
}
