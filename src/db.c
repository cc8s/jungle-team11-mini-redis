#include "db.h"

#include <stdlib.h>
#include <string.h>
#include "expire.h"

size_t db_entry_cost(size_t kl, size_t vl)
{
    return kl + vl + DB_ENTRY_OVERHEAD;
}

db *db_create(size_t maxmemory, int policy)
{
    db *d = calloc(1, sizeof *d);
    if (d == NULL)
        return NULL;
    d->main = dict_create();
    d->expires = dict_create();
    if (d->main == NULL || d->expires == NULL) {
        db_free(d);
        return NULL;
    }
    d->maxmemory = maxmemory;
    d->policy = policy;
    d->active_rehash = 1;
    return d;
}

void db_free(db *d)
{
    if (d == NULL)
        return;
    dict_free(d->main);
    dict_free(d->expires);
    free(d);
}

/* expires 에 기록된 만료 시각. 없으면 0 을 돌려주고 *has = 0 */
static int64_t when_of(db *d, const char *k, size_t kl, int *has)
{
    entry *x = dict_get(d->expires, k, kl);
    *has = x != NULL;
    if (x == NULL)
        return 0;
    int64_t when;
    memcpy(&when, x->val, sizeof when);
    return when;
}

int db_key_expired(db *d, const char *k, size_t kl)
{
    int has;
    int64_t when = when_of(d, k, kl, &has);
    return has && now_ms() > when;
}

/* main 과 expires 에서 지우고 메모리를 돌려받는다. main 에 있었으면 1 */
static int remove_key(db *d, const char *k, size_t kl)
{
    entry *e = dict_get(d->main, k, kl);
    if (e == NULL)
        return 0;
    d->used -= db_entry_cost(kl, e->vallen);
    dict_del(d->main, k, kl);
    if (dict_get(d->expires, k, kl) != NULL) {
        d->used -= db_entry_cost(kl, sizeof(int64_t));
        dict_del(d->expires, k, kl);
    }
    return 1;
}

entry *db_lookup(db *d, const char *k, size_t kl)
{
    entry *e = dict_get(d->main, k, kl);
    if (e == NULL)
        return NULL;
    if (db_key_expired(d, k, kl)) {
        remove_key(d, k, kl);
        return NULL;
    }
    return e; /* 그 사이 expires 만 봤으므로 main 의 e 는 그대로 유효하다 */
}

int db_set(db *d, const char *k, size_t kl, const char *v, size_t vl)
{
    entry *e = dict_get(d->main, k, kl);
    size_t old = e != NULL ? db_entry_cost(kl, e->vallen) : 0;
    if (dict_set(d->main, k, kl, v, vl) < 0)
        return -1;
    d->used = d->used - old + db_entry_cost(kl, vl);
    if (dict_get(d->expires, k, kl) != NULL) {
        d->used -= db_entry_cost(kl, sizeof(int64_t));
        dict_del(d->expires, k, kl);
    }
    return 0;
}

int db_del(db *d, const char *k, size_t kl)
{
    if (db_key_expired(d, k, kl)) {
        remove_key(d, k, kl);
        return 0;
    }
    return remove_key(d, k, kl);
}

int db_set_expire(db *d, const char *k, size_t kl, int64_t when_ms)
{
    if (db_lookup(d, k, kl) == NULL)
        return 0;
    if (when_ms <= now_ms()) {
        remove_key(d, k, kl);
        return 1;
    }
    int had = dict_get(d->expires, k, kl) != NULL;
    if (dict_set(d->expires, k, kl, (const char *)&when_ms, sizeof when_ms) < 0)
        return -1;
    if (!had)
        d->used += db_entry_cost(kl, sizeof(int64_t));
    return 1;
}

int64_t db_ttl_ms(db *d, const char *k, size_t kl)
{
    if (db_lookup(d, k, kl) == NULL)
        return -2;
    int has;
    int64_t when = when_of(d, k, kl, &has);
    if (!has)
        return -1;
    int64_t left = when - now_ms();
    return left < 0 ? 0 : left;
}

int db_persist(db *d, const char *k, size_t kl)
{
    if (db_lookup(d, k, kl) == NULL)
        return 0;
    if (dict_get(d->expires, k, kl) == NULL)
        return 0;
    d->used -= db_entry_cost(kl, sizeof(int64_t));
    dict_del(d->expires, k, kl);
    return 1;
}

void db_flush(db *d)
{
    dict_clear(d->main);
    dict_clear(d->expires);
    d->used = 0;
}

size_t db_size(const db *d)
{
    return dict_len(d->main);
}

size_t db_used_memory(const db *d)
{
    return d->used;
}

int db_make_room(db *d, size_t need)
{
    if (d->maxmemory == 0)
        return 0;
    if (need > d->maxmemory)
        return -1;
    while (d->used + need > d->maxmemory) {
        if (d->policy != DB_ALLKEYS_RANDOM)
            return -1;
        entry *e = dict_random_entry(d->main);
        if (e == NULL)
            return -1;
        /* remove_key 가 이 엔트리를 해제하므로 키를 먼저 복사한다 */
        char *k = malloc(e->keylen + 1);
        if (k == NULL)
            return -1;
        size_t kl = e->keylen;
        memcpy(k, e->key, kl);
        remove_key(d, k, kl);
        free(k);
        d->evicted++;
    }
    return 0;
}
