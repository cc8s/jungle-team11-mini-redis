#ifndef MINI_REDIS_SERVER_H
#define MINI_REDIS_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "server_config.h"

enum {
    SERVER_LISTEN_BACKLOG = 511,
    SERVER_CRON_INTERVAL_MS = 100,
    SERVER_CRON_TASK_BUDGET_MS = 1,
    SERVER_MMAP_THRESHOLD_BYTES = 128 * 1024,
};

enum server_result {
    SERVER_OK = 0,
    SERVER_OUT_OF_MEMORY,
    SERVER_SYSTEM_ERROR, /* 소켓 · 이벤트 루프 호출 실패. errno 를 본다 */
};

struct server;

/* 소켓을 열고 이벤트 루프와 db 를 만든다. 실패하면 NULL, server_destroy() 로 해제한다. */
struct server *server_create(const struct server_config *cfg);

/* 열린 연결을 모두 닫고 서버를 해제한다. NULL 이면 아무것도 하지 않는다. */
void server_destroy(struct server *server);

/* SIGINT · SIGTERM 이나 이벤트 루프 오류가 날 때까지 요청을 처리한다. */
enum server_result server_run(struct server *server);

/* 한 바퀴만 처리한다. 테스트에서 서버와 클라이언트를 한 스레드로 번갈아 돌릴 때 쓴다. */
enum server_result server_run_once(struct server *server, int max_wait_ms);

/* 실제로 열린 포트를 반환한다 (설정 포트가 0 이었을 때 확인용). */
int server_port(const struct server *server);

/* 주기 작업을 한 번 한다: 만료 샘플링 → rehash → 지연 해제 → 유휴 연결 정리.
 * 앞의 셋은 SERVER_CRON_TASK_BUDGET_MS 씩만 쓴다. 테스트는 가짜 시각을 넘긴다. */
void server_cron(struct server *server, int64_t now_ms);

/* 서버의 저장소. 테스트에서 소켓을 거치지 않고 키를 넣고 상태를 확인할 때 쓴다. */
struct db *server_db(struct server *server);

/* 지금 열린 클라이언트 연결 수를 반환한다. */
size_t server_client_count(const struct server *server);

#endif
