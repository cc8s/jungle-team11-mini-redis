#include "commands.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "expire.h"

#define ERR_INT "ERR value is not an integer or out of range"
#define ERR_SYNTAX "ERR syntax error"
#define ERR_OOM "OOM command not allowed when used memory > 'maxmemory'."

typedef int (*cmd_fn)(db *d, resp_cmd *c, buf *out);

typedef struct {
    const char *name;
    int arity; /* 양수: 정확히, 음수: 최소 |arity| */
    cmd_fn fn; /* 반환: 데이터를 바꿨으면 1 */
} command;

/* ---- 도우미 ---- */

/* 10진 정수 (resp 의 parse_ll 과 같은 규칙: '-' 하나 + 숫자, 넘치면 실패) */
static int to_ll(const char *p, size_t n, long long *out)
{
    size_t i = 0;
    int neg = 0;
    if (n > 0 && p[0] == '-') {
        neg = 1;
        i = 1;
    }
    if (i == n || n > 20)
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

static int arg_eq(resp_cmd *c, int i, const char *s)
{
    return c->argvlen[i] == strlen(s) && strncasecmp(c->argv[i], s, c->argvlen[i]) == 0;
}

static void err_expire(buf *out, const char *cmd)
{
    char msg[96];
    snprintf(msg, sizeof msg, "ERR invalid expire time in '%s' command", cmd);
    resp_error(out, msg);
}

/* amount 를 unit(1 또는 1000)ms 단위로 보고 현재 시각에 더한 절대 시각. 넘치면 -1 */
static int to_when(long long amount, long long unit, int64_t *when)
{
    if (amount > LLONG_MAX / unit || amount < LLONG_MIN / unit)
        return -1;
    long long ms = amount * unit;
    int64_t now = now_ms();
    if ((ms > 0 && now > LLONG_MAX - ms) || (ms < 0 && now < LLONG_MIN - ms))
        return -1;
    *when = now + ms;
    return 0;
}

/* ---- 명령 ---- */

static int cmd_ping(db *d, resp_cmd *c, buf *out)
{
    (void)d;
    if (c->argc == 1)
        resp_simple(out, "PONG");
    else
        resp_bulk(out, c->argv[1], c->argvlen[1]);
    return 0;
}

/* SET key value [NX|XX] [EX seconds|PX milliseconds] */
static int cmd_set(db *d, resp_cmd *c, buf *out)
{
    int nx = 0, xx = 0;
    long long unit = 0, amount = 0;
    for (int i = 3; i < c->argc; i++) {
        if (arg_eq(c, i, "NX") && !xx && !nx) {
            nx = 1;
        } else if (arg_eq(c, i, "XX") && !nx && !xx) {
            xx = 1;
        } else if ((arg_eq(c, i, "EX") || arg_eq(c, i, "PX")) && unit == 0 && i + 1 < c->argc) {
            unit = arg_eq(c, i, "EX") ? 1000 : 1;
            if (to_ll(c->argv[i + 1], c->argvlen[i + 1], &amount) != 0) {
                resp_error(out, ERR_INT);
                return 0;
            }
            i++;
        } else {
            resp_error(out, ERR_SYNTAX);
            return 0;
        }
    }
    int64_t when = 0;
    if (unit != 0 && (amount <= 0 || to_when(amount, unit, &when) != 0)) {
        err_expire(out, "set");
        return 0;
    }

    const char *k = c->argv[1], *v = c->argv[2];
    size_t kl = c->argvlen[1], vl = c->argvlen[2];
    int exists = db_lookup(d, k, kl) != NULL;
    if ((nx && exists) || (xx && !exists)) {
        resp_null(out);
        return 0;
    }
    if (db_make_room(d, db_entry_cost(kl, vl) + (unit ? db_entry_cost(kl, 8) : 0)) != 0) {
        resp_error(out, ERR_OOM);
        return 0;
    }
    if (db_set(d, k, kl, v, vl) != 0 || (unit != 0 && db_set_expire(d, k, kl, when) < 0)) {
        resp_error(out, "ERR out of memory");
        return 1;
    }
    resp_simple(out, "OK");
    return 1;
}

static int cmd_get(db *d, resp_cmd *c, buf *out)
{
    entry *e = db_lookup(d, c->argv[1], c->argvlen[1]);
    if (e == NULL)
        resp_null(out);
    else
        resp_bulk(out, e->val, e->vallen);
    return 0;
}

static int cmd_del(db *d, resp_cmd *c, buf *out)
{
    long long n = 0;
    for (int i = 1; i < c->argc; i++)
        n += db_del(d, c->argv[i], c->argvlen[i]);
    resp_int(out, n);
    return n > 0;
}

static int cmd_exists(db *d, resp_cmd *c, buf *out)
{
    long long n = 0;
    for (int i = 1; i < c->argc; i++)
        n += db_lookup(d, c->argv[i], c->argvlen[i]) != NULL;
    resp_int(out, n);
    return 0;
}

static int expire_generic(db *d, resp_cmd *c, buf *out, long long unit, const char *name)
{
    long long amount;
    if (to_ll(c->argv[2], c->argvlen[2], &amount) != 0) {
        resp_error(out, ERR_INT);
        return 0;
    }
    int64_t when;
    if (to_when(amount, unit, &when) != 0) {
        err_expire(out, name);
        return 0;
    }
    int r = db_set_expire(d, c->argv[1], c->argvlen[1], when);
    resp_int(out, r > 0);
    return r > 0;
}

static int cmd_expire(db *d, resp_cmd *c, buf *out)
{
    return expire_generic(d, c, out, 1000, "expire");
}

static int cmd_pexpire(db *d, resp_cmd *c, buf *out)
{
    return expire_generic(d, c, out, 1, "pexpire");
}

static int cmd_ttl(db *d, resp_cmd *c, buf *out)
{
    int64_t ms = db_ttl_ms(d, c->argv[1], c->argvlen[1]);
    resp_int(out, ms < 0 ? ms : (ms + 500) / 1000);
    return 0;
}

static int cmd_pttl(db *d, resp_cmd *c, buf *out)
{
    resp_int(out, db_ttl_ms(d, c->argv[1], c->argvlen[1]));
    return 0;
}

static int cmd_persist(db *d, resp_cmd *c, buf *out)
{
    int r = db_persist(d, c->argv[1], c->argvlen[1]);
    resp_int(out, r);
    return r;
}

/* 순회 중에는 main 을 건드리지 않는다: 만료 판정은 expires 만 보는 db_key_expired, 응답은 모았다가 한 번에 */
static int cmd_keys(db *d, resp_cmd *c, buf *out)
{
    buf body;
    buf_init(&body);
    long n = 0;
    dict_iter it;
    dict_iter_init(&it, d->main);
    for (entry *e = dict_iter_next(&it); e != NULL; e = dict_iter_next(&it)) {
        if (!glob_match(c->argv[1], c->argvlen[1], e->key, e->keylen))
            continue;
        if (db_key_expired(d, e->key, e->keylen))
            continue;
        resp_bulk(&body, e->key, e->keylen);
        n++;
    }
    resp_array_len(out, n);
    buf_append(out, body.data, body.len);
    buf_free(&body);
    return 0;
}

static int cmd_flushall(db *d, resp_cmd *c, buf *out)
{
    (void)c;
    db_flush(d);
    resp_simple(out, "OK");
    return 1;
}

static int cmd_empty_array(db *d, resp_cmd *c, buf *out)
{
    (void)d;
    (void)c;
    resp_array_len(out, 0);
    return 0;
}

static int cmd_ok(db *d, resp_cmd *c, buf *out)
{
    (void)d;
    (void)c;
    resp_simple(out, "OK");
    return 0;
}

static int cmd_select(db *d, resp_cmd *c, buf *out)
{
    (void)d;
    if (arg_eq(c, 1, "0"))
        resp_simple(out, "OK");
    else
        resp_error(out, "ERR DB index is out of range");
    return 0;
}

/* ---- DEBUG: 웹 인스펙터용 (진짜 Redis 의 DEBUG 와 이름만 같고 내용은 다름) ----
 *
 * DEBUG HT main|expires [maxbuckets]   해시 테이블 내부를 그대로 보여 준다. 읽기 전용: rehash 도, lazy 만료도 일으키지 않는다
 *   응답: [ "now_ms", n, "used_memory", n, "maxmemory", n, "evicted", n, "active_rehash", 0|1,
 *           "rehashidx", n, "tables", [ ht0, ht1 ] ]
 *   ht  = [ 버킷 수, 키 수, [ 버킷0, 버킷1, ... ] ]   (앞에서 maxbuckets 개까지만, 기본 1024)
 *   버킷 = [ 엔트리, ... ]                            (체인 순서 그대로)
 *   엔트리 = [ 키, 값 ] (main) 또는 [ 키, 만료 시각 ms ] (expires). 키·값은 앞 DEBUG_SHOW_BYTES 바이트만
 * DEBUG REHASHSTEP main|expires n      rehash 를 n 칸 진행하고 rehashidx 를 돌려준다
 * DEBUG ACTIVEREHASH on|off            cron 의 rehash 를 켜고 끈다 (끄면 옮기는 도중을 관찰할 수 있다) */

#define DEBUG_SHOW_BYTES 256
#define DEBUG_MAX_BUCKETS 65536

static dict *debug_dict(db *d, resp_cmd *c, int i)
{
    if (arg_eq(c, i, "main"))
        return d->main;
    if (arg_eq(c, i, "expires"))
        return d->expires;
    return NULL;
}

static size_t show_len(size_t n)
{
    return n < DEBUG_SHOW_BYTES ? n : DEBUG_SHOW_BYTES;
}

static void debug_table(buf *out, const table *t, int is_expires, long maxb)
{
    long n = t->size < (size_t)maxb ? (long)t->size : maxb;
    resp_array_len(out, 3);
    resp_int(out, (long long)t->size);
    resp_int(out, (long long)t->used);
    resp_array_len(out, n);
    for (long i = 0; i < n; i++) {
        long len = 0;
        for (entry *e = t->buckets[i]; e != NULL; e = e->next)
            len++;
        resp_array_len(out, len);
        for (entry *e = t->buckets[i]; e != NULL; e = e->next) {
            resp_array_len(out, 2);
            resp_bulk(out, e->key, show_len(e->keylen));
            if (is_expires) {
                int64_t when;
                memcpy(&when, e->val, sizeof when);
                resp_int(out, when);
            } else {
                resp_bulk(out, e->val, show_len(e->vallen));
            }
        }
    }
}

static int cmd_debug(db *d, resp_cmd *c, buf *out)
{
    long long n;
    dict *t;
    if (arg_eq(c, 1, "ht") && (c->argc == 3 || c->argc == 4)) {
        if ((t = debug_dict(d, c, 2)) == NULL) {
            resp_error(out, "ERR DEBUG HT target must be 'main' or 'expires'");
            return 0;
        }
        n = 1024;
        if (c->argc == 4 && (to_ll(c->argv[3], c->argvlen[3], &n) < 0 || n < 1 || n > DEBUG_MAX_BUCKETS)) {
            resp_error(out, "ERR maxbuckets must be between 1 and 65536");
            return 0;
        }
        resp_array_len(out, 14);
        resp_bulk(out, "now_ms", 6);
        resp_int(out, now_ms());
        resp_bulk(out, "used_memory", 11);
        resp_int(out, (long long)d->used);
        resp_bulk(out, "maxmemory", 9);
        resp_int(out, (long long)d->maxmemory);
        resp_bulk(out, "evicted", 7);
        resp_int(out, (long long)d->evicted);
        resp_bulk(out, "active_rehash", 13);
        resp_int(out, d->active_rehash);
        resp_bulk(out, "rehashidx", 9);
        resp_int(out, t->rehashidx);
        resp_bulk(out, "tables", 6);
        resp_array_len(out, 2);
        debug_table(out, &t->ht[0], t == d->expires, (long)n);
        debug_table(out, &t->ht[1], t == d->expires, (long)n);
        return 0;
    }
    if (arg_eq(c, 1, "rehashstep") && c->argc == 4) {
        if ((t = debug_dict(d, c, 2)) == NULL) {
            resp_error(out, "ERR DEBUG REHASHSTEP target must be 'main' or 'expires'");
            return 0;
        }
        if (to_ll(c->argv[3], c->argvlen[3], &n) < 0 || n < 1 || n > 1000000) {
            resp_error(out, "ERR steps must be between 1 and 1000000");
            return 0;
        }
        dict_rehash_step(t, (int)n);
        resp_int(out, t->rehashidx);
        return 0;
    }
    if (arg_eq(c, 1, "activerehash") && c->argc == 3 && (arg_eq(c, 2, "on") || arg_eq(c, 2, "off"))) {
        d->active_rehash = arg_eq(c, 2, "on");
        resp_simple(out, "OK");
        return 0;
    }
    resp_error(out, "ERR unknown DEBUG subcommand. Try HT, REHASHSTEP, ACTIVEREHASH");
    return 0;
}

static const command TABLE[] = {
    {"ping", -1, cmd_ping},       {"echo", 2, cmd_ping},        {"set", -3, cmd_set},         {"get", 2, cmd_get},
    {"del", -2, cmd_del},         {"exists", -2, cmd_exists},   {"expire", 3, cmd_expire},
    {"pexpire", 3, cmd_pexpire},  {"ttl", 2, cmd_ttl},          {"pttl", 2, cmd_pttl},
    {"persist", 2, cmd_persist},  {"keys", 2, cmd_keys},        {"flushall", -1, cmd_flushall},
    {"command", -1, cmd_empty_array}, {"config", -2, cmd_empty_array}, {"client", -2, cmd_ok},
    {"select", 2, cmd_select},    {"debug", -3, cmd_debug},
};

int cmd_execute(db *d, resp_cmd *c, buf *out)
{
    if (c->argc == 0)
        return 0;
    const command *cmd = NULL;
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
        if (c->argvlen[0] == strlen(TABLE[i].name) && strncasecmp(c->argv[0], TABLE[i].name, c->argvlen[0]) == 0) {
            cmd = &TABLE[i];
            break;
        }
    }
    if (cmd == NULL) {
        buf msg;
        buf_init(&msg);
        buf_appendf(&msg, "ERR unknown command '%.128s', with args beginning with: ", c->argv[0]);
        for (int i = 1; i < c->argc && msg.len < 512; i++)
            buf_appendf(&msg, "'%.128s' ", c->argv[i]);
        resp_error(out, msg.data);
        buf_free(&msg);
        return 0;
    }
    if ((cmd->arity > 0 && c->argc != cmd->arity) || (cmd->arity < 0 && c->argc < -cmd->arity) ||
        (cmd->arity == -1 && cmd->fn == cmd_ping && c->argc > 2)) {
        char msg[96];
        snprintf(msg, sizeof msg, "ERR wrong number of arguments for '%s' command", cmd->name);
        resp_error(out, msg);
        return 0;
    }
    return cmd->fn(d, c, out);
}

/* ---- 글롭 (Redis stringmatchlen 과 같은 규칙) ---- */

int glob_match(const char *p, size_t pl, const char *s, size_t sl)
{
    while (pl > 0) {
        switch (p[0]) {
        case '*':
            while (pl > 1 && p[1] == '*') {
                p++;
                pl--;
            }
            if (pl == 1)
                return 1;
            for (size_t i = 0; i <= sl; i++)
                if (glob_match(p + 1, pl - 1, s + i, sl - i))
                    return 1;
            return 0;
        case '?':
            if (sl == 0)
                return 0;
            s++;
            sl--;
            break;
        case '[': {
            if (sl == 0)
                return 0;
            p++;
            pl--;
            int neg = pl > 0 && p[0] == '^';
            if (neg) {
                p++;
                pl--;
            }
            int match = 0;
            while (pl > 0 && p[0] != ']') {
                if (p[0] == '\\' && pl >= 2) {
                    p++;
                    pl--;
                    if (p[0] == s[0])
                        match = 1;
                } else if (pl >= 3 && p[1] == '-') {
                    unsigned char lo = (unsigned char)p[0], hi = (unsigned char)p[2], ch = (unsigned char)s[0];
                    if (lo > hi) {
                        unsigned char t = lo;
                        lo = hi;
                        hi = t;
                    }
                    if (ch >= lo && ch <= hi)
                        match = 1;
                    p += 2;
                    pl -= 2;
                } else if (p[0] == s[0]) {
                    match = 1;
                }
                p++;
                pl--;
            }
            if (pl == 0) /* 닫는 ] 없음: 패턴 끝까지 본 것으로 */
                return 0;
            if (neg)
                match = !match;
            if (!match)
                return 0;
            s++;
            sl--;
            break;
        }
        case '\\':
            if (pl >= 2) {
                p++;
                pl--;
            }
            /* fall through */
        default:
            if (sl == 0 || p[0] != s[0])
                return 0;
            s++;
            sl--;
            break;
        }
        p++;
        pl--;
    }
    return sl == 0;
}
