#ifndef MINI_REDIS_MEM_H
#define MINI_REDIS_MEM_H

#include <stddef.h>

/* size_bytes 바이트를 할당하고 사용량에 더한다. 실패하면 NULL, mem_free() 로 해제한다. */
void *mem_alloc(size_t size_bytes);

/* count * size_bytes 바이트를 0 으로 채워 할당한다. 실패하면 NULL, mem_free() 로 해제한다. */
void *mem_calloc(size_t count, size_t size_bytes);

/* 블록을 해제하고 사용량에서 뺀다. NULL 이면 아무것도 하지 않는다. */
void mem_free(void *ptr);

/* mem_alloc / mem_calloc 으로 할당해 아직 해제하지 않은 바이트 수를 반환한다. */
size_t mem_used_bytes(void);

#endif
