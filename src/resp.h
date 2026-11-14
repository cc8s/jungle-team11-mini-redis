#ifndef RESP_H
#define RESP_H

/* RESP2 요청 파싱과 응답 쓰기. 소켓을 모르는 순수 함수다.
 *
 * 요청 형식
 *  - 배열(멀티벌크): *<N>\r\n 다음에 $<L>\r\n<L바이트>\r\n 이 N번.
 *    데이터는 길이로 읽는다. \r\n 은 머리줄(*N, $L)의 끝에만 쓴다.
 *  - 인라인: 첫 바이트가 '*' 가 아니면 한 줄(\n 까지, 앞의 \r 은 버림)을 공백·탭으로 나눈다. 따옴표는 처리하지 않는다.
 *
 * resp_parse 결과
 *  - RESP_OK: 명령 하나 완성. *consumed = 쓴 바이트 수. out 을 채운다(argv 는 복사본, 각 끝에 \0 을 붙임).
 *    빈 명령(빈 인라인 줄, *0, *-1)도 OK 이고 argc = 0 이다. 호출자는 건너뛴다.
 *  - RESP_INCOMPLETE: 데이터가 모자라다. 아무것도 소비하지 않는다(*consumed = 0, out 은 비어 있음).
 *  - RESP_ERR: 형식 오류. *errmsg 에 응답할 에러 문구("ERR Protocol error: ..."). 서버는 응답 후 연결을 끊는다.
 *
 * 한도 (메모리 고갈 DoS 방어, 진짜 Redis 와 같은 값)
 *  - 배열 원소 수 최대 RESP_MAX_MULTIBULK, 벌크 길이 최대 RESP_MAX_BULK.
 *  - 머리줄이나 인라인 줄이 RESP_MAX_INLINE 바이트를 넘도록 \r\n(\n) 이 없으면 ERR.
 *  - 길이 숫자는 10진 숫자만 허용(부호는 '-' 하나만), 넘치면 ERR.
 *  - 길이를 믿고 미리 메모리를 잡지 않는다. 명령이 다 도착한 뒤에만 복사한다. */

#include <stddef.h>
#include "buf.h"

#define RESP_MAX_MULTIBULK 2147483647L           /* INT_MAX. 메모리는 입력 버퍼 한도가 막는다 */
#define RESP_MAX_BULK (512L * 1024 * 1024)       /* 512MB */
#define RESP_MAX_INLINE (64L * 1024)             /* 64KB */

typedef enum { RESP_OK, RESP_INCOMPLETE, RESP_ERR } resp_status;

typedef struct {
    int argc;
    char **argv;
    size_t *argvlen;
} resp_cmd;

resp_status resp_parse(const char *data, size_t len, resp_cmd *out, size_t *consumed, const char **errmsg);
void resp_cmd_free(resp_cmd *c); /* argv 해제 후 argc = 0. 빈 명령이어도 안전 */

/* 응답 쓰기. 실패하면 -1 */
int resp_simple(buf *o, const char *s);          /* +OK\r\n */
int resp_error(buf *o, const char *msg);         /* -<msg>\r\n  (msg 에 "ERR " 까지 포함) */
int resp_int(buf *o, long long n);               /* :1\r\n */
int resp_bulk(buf *o, const char *p, size_t n);  /* $3\r\nabc\r\n */
int resp_null(buf *o);                           /* $-1\r\n */
int resp_array_len(buf *o, long n);              /* *2\r\n */

#endif
