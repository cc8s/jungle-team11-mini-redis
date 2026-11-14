#ifndef SERVER_H
#define SERVER_H

/* epoll 단일 스레드 이벤트 루프 (R002). 소켓을 다루는 유일한 모듈이다.
 *
 *  - 리슨 소켓과 클라이언트 소켓은 논블로킹. epoll_wait 의 대기 시간으로 100ms cron 을 만든다.
 *  - 읽기: 이벤트 한 번에 SERVER_READ_CHUNK(16KB) 만 한 번 read → resp_parse 가 OK 인 동안 cmd_execute 반복
 *    (파이프라이닝) → 다 본 만큼 한 번에 consume. 남은 소켓 데이터는 다음 바퀴에(공정성, Redis 와 같음).
 *    INCOMPLETE 면 남은 바이트를 두고 다음 read 를 기다린다(분할 수신).
 *  - 쓰기: 응답을 out 에 모았다가 바로 write. 다 못 보냈을 때만 EPOLLOUT 을 켜고, 다 보내면 끈다.
 *  - 파싱 ERR: 에러 응답을 보낸 뒤 연결을 닫는다(Redis 와 같음).
 *  - 보호 한도: 입력 버퍼 SERVER_MAX_QUERYBUF(1GB), 출력 버퍼 SERVER_MAX_OUTBUF(64MB, 읽어 가지 않는 손님)를
 *    넘으면 끊음. 연결 SERVER_MAX_CLIENTS 개까지.
 *  - cron(100ms): active 만료 + rehash 중이면 1ms 동안 rehash 진행.
 *  - SIGINT/SIGTERM 이면 연결을 정리하고 0 을 돌려준다. SIGPIPE 는 무시한다. */

#include <stddef.h>

#define SERVER_MAX_CLIENTS 10000
#define SERVER_MAX_QUERYBUF (1024L * 1024 * 1024)
#define SERVER_MAX_OUTBUF (64L * 1024 * 1024)
#define SERVER_READ_CHUNK (16 * 1024)
#define SERVER_CRON_MS 100

typedef struct {
    int port;
    size_t maxmemory;
    int policy;
} server_config;

int server_run(const server_config *cfg);

#endif
