#ifndef MINI_REDIS_RESP_H
#define MINI_REDIS_RESP_H

#include <stddef.h>

#include "str.h"

enum {
    RESP_MAX_ARGS = 1024,
    RESP_MAX_BULK_BYTES = 512 * 1024 * 1024,
    RESP_MAX_INLINE_BYTES = 64 * 1024,
    RESP_MAX_NUMBER_DIGITS = 20,
};

enum resp_parse_result {
    RESP_PARSE_DONE = 0,
    RESP_PARSE_NEED_MORE,
    RESP_PARSE_ERROR,
    RESP_PARSE_OUT_OF_MEMORY,
};

/* 파싱한 명령 하나. argv[0] 이 명령 이름이고 argv 와 각 인자는 이 구조체가 소유한다.
 * consumed_bytes 는 이 명령이 버퍼에서 차지한 바이트 수다 (파이프라인의 다음 명령 위치). */
struct resp_command {
    size_t argc;
    struct str **argv;
    size_t consumed_bytes;
};

/* buf 앞에서 명령 하나를 파싱한다. DONE 이면 out_cmd 를 채우고, 그 밖에는 빈 명령으로 둔다.
 * 결과와 상관없이 resp_command_deinit() 을 불러도 안전하다. buf 는 읽기만 한다. */
enum resp_parse_result resp_parse(const char *buf, size_t len, struct resp_command *out_cmd);

/* argv 와 각 인자를 해제하고 빈 명령으로 되돌린다. */
void resp_command_deinit(struct resp_command *cmd);

#endif
