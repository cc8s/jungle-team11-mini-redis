#include "hashtable.h"

#include <stdlib.h>
#include <string.h>

uint64_t dict_default_hash(const char *key, size_t keylen)
{
    uint64_t h = 1469598103934665603ULL; /* FNV-1a 64비트 offset basis */
    for (size_t i = 0; i < keylen; i++) {
        h ^= (unsigned char)key[i];
        h *= 1099511628211ULL; /* FNV prime */
    }
    return h;
}

/* ---- 테이블 하나 ---- */

static int table_init(table *t, size_t size)
{
    t->buckets = calloc(size, sizeof *t->buckets);
    if (t->buckets == NULL)
        return -1;
    t->size = size;
    t->mask = size - 1;
    t->used = 0;
    return 0;
}

static void table_reset(table *t)
{
    t->buckets = NULL;
    t->size = 0;
    t->mask = 0;
    t->used = 0;
}

static void entry_free(entry *e)
{
    free(e->key);
    free(e->val);
    free(e);
}

static void table_free(table *t)
{
    for (size_t i = 0; i < t->size; i++) {
        entry *e = t->buckets[i];
        while (e != NULL) {
            entry *next = e->next;
            entry_free(e);
            e = next;
        }
    }
    free(t->buckets);
    table_reset(t);
}

/* 길이 0 도 malloc(0) 에 기대지 않도록 최소 1바이트를 잡는다 */
static char *dup_bytes(const char *src, size_t len)
{
    char *p = malloc(len + 1);
    if (p == NULL)
        return NULL;
    if (len > 0)
        memcpy(p, src, len);
    p[len] = '\0';
    return p;
}

/* ---- dict ---- */

dict *dict_create_with_hash(dict_hash_fn hash)
{
    dict *d = malloc(sizeof *d);
    if (d == NULL)
        return NULL;
    if (table_init(&d->ht[0], DICT_INITIAL_SIZE) != 0) {
        free(d);
        return NULL;
    }
    table_reset(&d->ht[1]);
    d->rehashidx = -1;
    d->hash = hash != NULL ? hash : dict_default_hash;
    return d;
}

dict *dict_create(void)
{
    return dict_create_with_hash(dict_default_hash);
}

void dict_free(dict *d)
{
    if (d == NULL)
        return;
    table_free(&d->ht[0]);
    table_free(&d->ht[1]);
    free(d);
}

size_t dict_len(const dict *d)
{
    return d->ht[0].used + d->ht[1].used;
}

static void finish_rehash(dict *d)
{
    free(d->ht[0].buckets);
    d->ht[0] = d->ht[1];
    table_reset(&d->ht[1]);
    d->rehashidx = -1;
}

void dict_rehash_step(dict *d, int n)
{
    if (!dict_is_rehashing(d))
        return;
    while (n-- > 0 && d->ht[0].used > 0 && (size_t)d->rehashidx < d->ht[0].size) {
        entry *e = d->ht[0].buckets[d->rehashidx];
        while (e != NULL) {
            entry *next = e->next;
            size_t idx = d->hash(e->key, e->keylen) & d->ht[1].mask;
            e->next = d->ht[1].buckets[idx];
            d->ht[1].buckets[idx] = e;
            d->ht[0].used--;
            d->ht[1].used++;
            e = next;
        }
        d->ht[0].buckets[d->rehashidx] = NULL;
        d->rehashidx++;
    }
    if (d->ht[0].used == 0)
        finish_rehash(d);
}

/* 키를 찾는다. prev_out 에는 체인에서 바로 앞 노드(첫 노드면 NULL), table_out 에는 찾은 테이블 번호 */
static entry *find(dict *d, const char *key, size_t keylen, uint64_t h,
                   entry **prev_out, int *table_out)
{
    int tables = dict_is_rehashing(d) ? 2 : 1;
    for (int t = 0; t < tables; t++) {
        table *tb = &d->ht[t];
        entry *prev = NULL;
        for (entry *e = tb->buckets[h & tb->mask]; e != NULL; prev = e, e = e->next) {
            if (e->keylen == keylen && (keylen == 0 || memcmp(e->key, key, keylen) == 0)) {
                if (prev_out != NULL)
                    *prev_out = prev;
                if (table_out != NULL)
                    *table_out = t;
                return e;
            }
        }
    }
    return NULL;
}

static int start_rehash_if_needed(dict *d)
{
    if (dict_is_rehashing(d) || d->ht[0].used < d->ht[0].size)
        return 0;
    if (table_init(&d->ht[1], d->ht[0].size * 2) != 0)
        return -1;
    d->rehashidx = 0;
    return 0;
}

int dict_set(dict *d, const char *key, size_t keylen, const char *val, size_t vallen)
{
    if (dict_is_rehashing(d))
        dict_rehash_step(d, 1);

    uint64_t h = d->hash(key, keylen);
    entry *e = find(d, key, keylen, h, NULL, NULL);
    if (e != NULL) {
        char *nv = dup_bytes(val, vallen);
        if (nv == NULL)
            return -1;
        free(e->val);
        e->val = nv;
        e->vallen = vallen;
        return 0;
    }

    if (start_rehash_if_needed(d) != 0)
        return -1;

    e = malloc(sizeof *e);
    if (e == NULL)
        return -1;
    e->key = dup_bytes(key, keylen);
    e->val = dup_bytes(val, vallen);
    if (e->key == NULL || e->val == NULL) {
        entry_free(e);
        return -1;
    }
    e->keylen = keylen;
    e->vallen = vallen;

    table *tb = dict_is_rehashing(d) ? &d->ht[1] : &d->ht[0];
    size_t idx = h & tb->mask;
    e->next = tb->buckets[idx];
    tb->buckets[idx] = e;
    tb->used++;
    return 1;
}

entry *dict_get(dict *d, const char *key, size_t keylen)
{
    if (dict_is_rehashing(d))
        dict_rehash_step(d, 1);
    return find(d, key, keylen, d->hash(key, keylen), NULL, NULL);
}

/* ---- 무작위 엔트리 ---- */

static int (*rand_impl)(void) = rand;

void dict_set_rand(int (*rand_fn)(void))
{
    rand_impl = rand_fn != NULL ? rand_fn : rand;
}

entry *dict_random_entry(dict *d)
{
    if (dict_len(d) == 0)
        return NULL;

    entry *head;
    do {
        if (dict_is_rehashing(d)) {
            /* ht[0] 의 rehashidx 앞은 이미 비었으므로 뺀다. 그 뒤로 ht[0] 나머지 + ht[1] 전체 */
            size_t skip = (size_t)d->rehashidx;
            size_t span = d->ht[0].size + d->ht[1].size - skip;
            size_t idx = skip + (size_t)rand_impl() % span;
            head = idx < d->ht[0].size ? d->ht[0].buckets[idx] : d->ht[1].buckets[idx - d->ht[0].size];
        } else {
            head = d->ht[0].buckets[(size_t)rand_impl() & d->ht[0].mask];
        }
    } while (head == NULL);

    size_t len = 0;
    for (entry *e = head; e != NULL; e = e->next)
        len++;
    size_t pick = (size_t)rand_impl() % len;
    while (pick-- > 0)
        head = head->next;
    return head;
}

/* ---- 순회 ---- */

void dict_iter_init(dict_iter *it, dict *d)
{
    it->d = d;
    it->table = 0;
    it->bucket = 0;
    it->next = NULL;
}

entry *dict_iter_next(dict_iter *it)
{
    for (;;) {
        if (it->next != NULL) {
            entry *e = it->next;
            it->next = e->next;
            return e;
        }
        table *tb = &it->d->ht[it->table];
        while (it->bucket < tb->size) {
            entry *e = tb->buckets[it->bucket++];
            if (e != NULL) {
                it->next = e->next;
                return e;
            }
        }
        if (it->table == 0 && dict_is_rehashing(it->d)) {
            it->table = 1;
            it->bucket = 0;
            continue;
        }
        return NULL;
    }
}

/* ---- 전체 삭제 ---- */

int dict_clear(dict *d)
{
    table_free(&d->ht[0]);
    table_free(&d->ht[1]);
    d->rehashidx = -1;
    return table_init(&d->ht[0], DICT_INITIAL_SIZE);
}

int dict_del(dict *d, const char *key, size_t keylen)
{
    if (dict_is_rehashing(d))
        dict_rehash_step(d, 1);

    uint64_t h = d->hash(key, keylen);
    entry *prev = NULL;
    int t = 0;
    entry *e = find(d, key, keylen, h, &prev, &t);
    if (e == NULL)
        return 0;

    table *tb = &d->ht[t];
    if (prev == NULL)
        tb->buckets[h & tb->mask] = e->next;
    else
        prev->next = e->next;
    tb->used--;
    entry_free(e);
    return 1;
}
