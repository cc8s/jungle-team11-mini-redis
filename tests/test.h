#ifndef TEST_H
#define TEST_H

/* 외부 라이브러리 없이 쓰는 최소 테스트 매크로.
 * 실패하면 파일:줄과 식을 출력하고 그 테스트 실행 파일을 실패(1)로 끝낸다. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_passed = 0;

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            exit(1);                                                       \
        }                                                                  \
        test_passed++;                                                     \
    } while (0)

#define CHECK_STR_EQ(a, b) CHECK(strcmp((a), (b)) == 0)

#define RUN(test_fn)                         \
    do {                                     \
        printf("  %s\n", #test_fn);          \
        test_fn();                           \
    } while (0)

#define TEST_DONE() printf("  ok (%d checks)\n", test_passed)

#endif
