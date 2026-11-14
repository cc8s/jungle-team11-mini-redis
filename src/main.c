#include <stdio.h>
#include <stdlib.h>

#include "event_loop.h"
#include "hash_table.h"
#include "server.h"
#include "server_config.h"

static const char *read_env(const char *name);
static void print_startup(const struct server_config *cfg, const struct server *server);

/* 설정 → 서버 생성 → 루프 실행 → 정리. SIGINT · SIGTERM 이면 정리하고 0 으로 끝난다 */
int main(void)
{
    struct server_config cfg;
    struct server_config_error err;
    if (!server_config_load_env(&cfg, read_env, &err)) {
        fprintf(stderr, "mini-redis: %s\n", err.message);
        return 1;
    }
    if (hash_table_load_random_seed() != HT_OK) {
        fprintf(stderr, "mini-redis: /dev/urandom 에서 해시 seed 를 읽지 못했습니다\n");
        return 1;
    }
    struct server *server = server_create(&cfg);
    if (server == NULL) {
        perror("mini-redis: 서버를 시작하지 못했습니다");
        return 1;
    }
    if (event_loop_stop_on_signals() != EL_OK) {
        perror("mini-redis: 시그널 처리를 설정하지 못했습니다");
        server_destroy(server);
        return 1;
    }
    print_startup(&cfg, server);
    enum server_result result = server_run(server);
    server_destroy(server);
    return result == SERVER_OK ? 0 : 1;
}

/* getenv 는 char * 를 돌려줘 server_env_lookup_fn 과 타입이 달라서 감싼다 */
static const char *read_env(const char *name)
{
    return getenv(name);
}

static void print_startup(const struct server_config *cfg, const struct server *server)
{
    printf("mini-redis: %s:%d 에서 대기 중\n", cfg->bind_address, server_port(server));
    if (cfg->max_memory_bytes > 0) {
        printf("mini-redis: maxmemory %zu 바이트\n", cfg->max_memory_bytes);
    }
    if (cfg->is_flush_enabled) {
        printf("mini-redis: FLUSHALL 활성 (테스트용)\n");
    }
    fflush(stdout);
}
