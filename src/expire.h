#ifndef EXPIRE_H
#define EXPIRE_H

/* 시계와 active 만료.
 *  - now_ms(): 현재 시각(밀리초, CLOCK_REALTIME). 만료 시각은 절대 시각이라 AOF 재시작 뒤에도 맞는다.
 *    테스트는 expire_set_clock 으로 가짜 시계를 끼워 실제로 기다리지 않는다. NULL 이면 실제 시계로 돌아간다.
 *  - expire_active_cycle(): TTL 이 있는 키(expires)에서 ACTIVE_EXPIRE_SAMPLES 개를 무작위로 뽑아 지난 것을 지운다.
 *    뽑은 것 중 ACTIVE_EXPIRE_REPEAT_PCT % 를 넘게 지났으면 반복한다. 한 번 호출에 최대 ACTIVE_EXPIRE_BUDGET_MS
 *    (단조 시계로 잰다, 가짜 시계와 무관). 단일 스레드를 오래 막지 않기 위한 상한이다. 지운 개수를 돌려준다. */

#include <stdint.h>
#include "db.h"

#define ACTIVE_EXPIRE_SAMPLES 20
#define ACTIVE_EXPIRE_REPEAT_PCT 25
#define ACTIVE_EXPIRE_BUDGET_MS 25

typedef int64_t (*clock_fn)(void);

void expire_set_clock(clock_fn f);
int64_t now_ms(void);

int expire_active_cycle(db *d);

#endif
