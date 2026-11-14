#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "db.h"
#include "server.h"

static void usage(void)
{
    fprintf(stderr,
            "사용법: mini-redis [--port 6379] [--maxmemory <바이트|숫자kb|mb|gb>]\n"
            "                   [--maxmemory-policy noeviction|allkeys-random]\n");
}

/* "100", "64kb", "100mb", "1gb" → 바이트. 실패 -1 */
static long long parse_size(const char *s)
{
    char *end;
    long long v = strtoll(s, &end, 10);
    if (end == s || v < 0)
        return -1;
    if (*end == '\0' || strcasecmp(end, "b") == 0)
        return v;
    if (strcasecmp(end, "kb") == 0)
        return v * 1024;
    if (strcasecmp(end, "mb") == 0)
        return v * 1024 * 1024;
    if (strcasecmp(end, "gb") == 0)
        return v * 1024 * 1024 * 1024;
    return -1;
}

int main(int argc, char **argv)
{
    server_config cfg = {.port = 6379, .maxmemory = 0, .policy = DB_NOEVICTION};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cfg.port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--maxmemory") == 0 && i + 1 < argc) {
            long long v = parse_size(argv[++i]);
            if (v < 0) {
                usage();
                return 2;
            }
            cfg.maxmemory = (size_t)v;
        } else if (strcmp(argv[i], "--maxmemory-policy") == 0 && i + 1 < argc) {
            const char *p = argv[++i];
            if (strcmp(p, "noeviction") == 0)
                cfg.policy = DB_NOEVICTION;
            else if (strcmp(p, "allkeys-random") == 0)
                cfg.policy = DB_ALLKEYS_RANDOM;
            else {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    if (cfg.port <= 0 || cfg.port > 65535) {
        usage();
        return 2;
    }
    return server_run(&cfg);
}
