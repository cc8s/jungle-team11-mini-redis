#include "expire.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t real_clock(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static clock_fn clock_impl = real_clock;

void expire_set_clock(clock_fn f)
{
    clock_impl = f != NULL ? f : real_clock;
}

int64_t now_ms(void)
{
    return clock_impl();
}

int expire_active_cycle(db *d)
{
    int deleted = 0;
    int64_t start = mono_ms();
    for (;;) {
        size_t n = dict_len(d->expires);
        if (n == 0)
            break;
        int sample = n < ACTIVE_EXPIRE_SAMPLES ? (int)n : ACTIVE_EXPIRE_SAMPLES;
        int expired = 0;
        int64_t now = now_ms();
        for (int i = 0; i < sample; i++) {
            entry *x = dict_random_entry(d->expires);
            if (x == NULL)
                break;
            int64_t when;
            memcpy(&when, x->val, sizeof when);
            if (now <= when)
                continue;
            /* db_del 이 이 엔트리를 해제하므로 키를 먼저 복사한다 */
            size_t kl = x->keylen;
            char *k = malloc(kl + 1);
            if (k == NULL)
                return deleted;
            memcpy(k, x->key, kl);
            db_del(d, k, kl);
            free(k);
            expired++;
        }
        deleted += expired;
        if (expired * 100 <= sample * ACTIVE_EXPIRE_REPEAT_PCT)
            break;
        if (mono_ms() - start >= ACTIVE_EXPIRE_BUDGET_MS)
            break;
    }
    return deleted;
}
