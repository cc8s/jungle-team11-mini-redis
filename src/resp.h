#ifndef MINI_REDIS_RESP_H
#define MINI_REDIS_RESP_H

#include <stddef.h>
#include <stdint.h>

#include "buffer.h"
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

/* 응답 직렬화. 모두 out_buf 뒤에 응답 하나를 통째로 붙이고, 할당이 실패하면 아무것도 붙이지
 * 않은 채 BUFFER_OUT_OF_MEMORY 를 돌려준다 (반쯤 쓴 응답이 남지 않는다).
 * text 에는 \r 과 \n 이 없어야 한다. */

/* +<text>\r\n */
enum buffer_result resp_write_simple(struct buffer *out_buf, const char *text);

/* -<text>\r\n. text 는 "ERR ..." 처럼 오류 종류로 시작한다. */
enum buffer_result resp_write_error(struct buffer *out_buf, const char *text);

/* :<value>\r\n */
enum buffer_result resp_write_integer(struct buffer *out_buf, int64_t value);

/* $<len>\r\n<data>\r\n. 길이로 쓰므로 값에 \0 · \r\n 이 있어도 된다. */
enum buffer_result resp_write_bulk(struct buffer *out_buf, const struct str *value);

/* $-1\r\n (없는 값) */
enum buffer_result resp_write_null(struct buffer *out_buf);

#endif
