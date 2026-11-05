#include "test.h"

/* 테스트 환경 자체가 동작하는지 확인. 실제 모듈 테스트가 생기면 지운다. */
static void test_harness_works(void)
{
    CHECK(1 + 1 == 2);
    CHECK_STR_EQ("mini", "mini");
}

int main(void)
{
    RUN(test_harness_works);
    TEST_DONE();
    return 0;
}
