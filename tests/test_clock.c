#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "clock.h"
#include "test.h"

enum {
    REPEAT_COUNT = 100000,
    SLEEP_MS = 5,
    NS_PER_MS = 1000000,
};

static void sleep_ms(int64_t duration_ms)
{
    struct timespec duration = {.tv_sec = 0, .tv_nsec = duration_ms * NS_PER_MS};
    CHECK(nanosleep(&duration, NULL) == 0);
}

static void test_now_ms_returns_non_negative(void)
{
    CHECK(clock_now_ms() >= 0);
}

static void test_now_ms_called_twice_does_not_decrease(void)
{
    int64_t first_ms = clock_now_ms();
    int64_t second_ms = clock_now_ms();
    CHECK(second_ms >= first_ms);
}

static void test_now_ms_called_repeatedly_never_decreases(void)
{
    int64_t prev_ms = clock_now_ms();
    bool has_decreased = false;
    for (int i = 0; i < REPEAT_COUNT; i++) {
        int64_t now_ms = clock_now_ms();
        if (now_ms < prev_ms) {
            has_decreased = true;
        }
        prev_ms = now_ms;
    }
    CHECK(!has_decreased);
}

static void test_now_ms_after_sleep_advances_by_sleep_time(void)
{
    int64_t before_ms = clock_now_ms();
    sleep_ms(SLEEP_MS);
    int64_t after_ms = clock_now_ms();
    CHECK(after_ms - before_ms >= SLEEP_MS);
}

int main(void)
{
    RUN(test_now_ms_returns_non_negative);
    RUN(test_now_ms_called_twice_does_not_decrease);
    RUN(test_now_ms_called_repeatedly_never_decreases);
    RUN(test_now_ms_after_sleep_advances_by_sleep_time);
    TEST_DONE();
    return 0;
}
