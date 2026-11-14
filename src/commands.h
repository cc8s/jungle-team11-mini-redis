#ifndef COMMANDS_H
#define COMMANDS_H

/* 명령 실행. 소켓을 모른다: argv 를 받아 응답 바이트를 out 에 쓴다. 서버 없이 명령 동작 전체를 테스트할 수 있다.
 *
 *  - 명령 이름은 대소문자를 가리지 않는다. 인자 수가 틀리면 ERR wrong number of arguments for '<이름>' command.
 *  - 모르는 명령: ERR unknown command '<이름>', with args beginning with: '<인자>' ... (진짜 Redis 7 과 같은 문구)
 *  - 정수 인자 오류: ERR value is not an integer or out of range
 *  - 만료 시간이 범위를 벗어나면: ERR invalid expire time in '<이름>' command
 *  - 메모리 상한(D7, noeviction): OOM command not allowed when used memory > 'maxmemory'.
 *  - 지원 명령: PING, ECHO, SET(EX|PX, NX|XX), GET, DEL, EXISTS, EXPIRE, PEXPIRE, TTL, PTTL, PERSIST, KEYS, FLUSHALL,
 *    접속용 최소 응답(D3): COMMAND(빈 배열), CONFIG GET(빈 배열), CLIENT(+OK), SELECT 0(+OK).
 *
 * 반환: 데이터를 바꾼 쓰기 명령이면 1, 아니면 0 (AOF 기록 여부). argc 0 인 빈 명령은 아무것도 쓰지 않고 0. */

#include "buf.h"
#include "db.h"
#include "resp.h"

int cmd_execute(db *d, resp_cmd *c, buf *out);

/* Redis 의 글롭 패턴: * ? [abc] [^a] [a-z] \x. 맞으면 1 (KEYS 용, 테스트용으로 공개) */
int glob_match(const char *pat, size_t plen, const char *s, size_t slen);

#endif
