#include "clock.h"

#include <time.h>

enum {
    MS_PER_SEC = 1000,
    NS_PER_MS = 1000000,
};

int64_t clock_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * MS_PER_SEC + now.tv_nsec / NS_PER_MS;
}
