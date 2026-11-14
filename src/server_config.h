#ifndef MINI_REDIS_SERVER_CONFIG_H
#define MINI_REDIS_SERVER_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SERVER_DEFAULT_PORT = 6379,
    SERVER_MAX_CLIENTS = 10000,
    SERVER_CONFIG_MAX_CLIENTS_LIMIT = 1000000,
    SERVER_CONFIG_MAX_IDLE_TIMEOUT_SEC = 365 * 24 * 60 * 60,
    SERVER_CONFIG_ERROR_BYTES = 200,
};

/* 서버 설정. main 은 server_config_load_env() 로 환경변수에서 채운다. */
struct server_config {
    const char *bind_address; /* IPv4 주소 문자열. 환경변수 문자열을 가리킬 수 있다 */
    int port;                 /* 0 이면 OS 가 빈 포트를 고른다 (테스트용) */
    size_t max_clients;
    size_t max_memory_bytes; /* 0 이면 제한하지 않는다 */
    int64_t idle_timeout_ms; /* 0 이면 유휴 연결을 닫지 않는다 */
    bool is_flush_enabled;
};

/* 환경변수 이름을 받아 값을 돌려준다. 없으면 NULL. main 은 getenv, 테스트는 가짜 표를 쓴다. */
typedef const char *server_env_lookup_fn(const char *name);

/* server_config_load_env() 가 실패했을 때 사람이 읽을 메시지 */
struct server_config_error {
    char message[SERVER_CONFIG_ERROR_BYTES];
};

/* 기본값: 127.0.0.1:6379, 최대 연결 SERVER_MAX_CLIENTS, 메모리 제한 없음,
 * 유휴 타임아웃 없음, FLUSHALL 비활성. */
void server_config_init_default(struct server_config *cfg);

/* 기본값에서 시작해 환경변수가 있는 항목만 덮어쓴다. 값이 하나라도 잘못됐으면 false 를
 * 반환하고 out_err 에 이유를 쓴다. 읽는 변수:
 *   MINI_REDIS_BIND               IPv4 주소
 *   MINI_REDIS_PORT               1 ~ 65535
 *   MINI_REDIS_MAXMEMORY          바이트, 0 이면 제한 없음
 *   MINI_REDIS_MAX_CLIENTS        1 ~ SERVER_CONFIG_MAX_CLIENTS_LIMIT
 *   MINI_REDIS_IDLE_TIMEOUT_SEC   초, 0 이면 끔
 *   MINI_REDIS_ENABLE_FLUSH       1 이면 FLUSHALL 허용, 0 이면 비활성 */
bool server_config_load_env(struct server_config *cfg, server_env_lookup_fn *lookup,
                            struct server_config_error *out_err);

#endif
