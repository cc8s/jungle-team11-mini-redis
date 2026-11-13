#ifndef MINI_REDIS_CLOCK_H
#define MINI_REDIS_CLOCK_H

#include <stdint.h>

/* 단조 증가 시계의 현재 시각을 ms 로 반환한다. 시스템 시각을 바꿔도 줄어들지 않는다. */
int64_t clock_now_ms(void);

#endif
