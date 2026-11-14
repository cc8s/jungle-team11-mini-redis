#include "hash_table.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock.h"
#include "mem.h"

/* SipHash-2-4 초기화 상수 ("somepseudorandomlygeneratedbytes") */
static const uint64_t SIP_INIT_V0 = 0x736f6d6570736575ULL;
static const uint64_t SIP_INIT_V1 = 0x646f72616e646f6dULL;
static const uint64_t SIP_INIT_V2 = 0x6c7967656e657261ULL;
static const uint64_t SIP_INIT_V3 = 0x7465646279746573ULL;

enum {
    SIP_BLOCK_BYTES = 8,
    SIP_COMPRESSION_ROUNDS = 2,
    SIP_FINALIZATION_ROUNDS = 4,
    SIP_FINAL_XOR = 0xff,
    SIP_LEN_SHIFT = 56,
    SIP_ROT_A = 13,
    SIP_ROT_B = 16,
    SIP_ROT_C = 21,
    SIP_ROT_D = 17,
    SIP_ROT_HALF = 32,
};

enum {
    SHRINK_FILL_PERCENT = 10,
    PERCENT_SCALE = 100,
};

enum {
    NOT_REHASHING = -1,
};

struct sip_state {
    uint64_t v0;
    uint64_t v1;
    uint64_t v2;
    uint64_t v3;
};

/* 버킷 배열 하나. 불변식: bucket_count 는 0 이거나 2의 거듭제곱이고,
 * entry_count 는 모든 체인의 노드 수와 같다 */
struct ht_table {
    struct ht_entry **buckets;
    size_t bucket_count;
    size_t entry_count;
};

/* rehash 중이 아니면 tables[1] 은 비어 있고(buckets == NULL) rehash_idx 는 NOT_REHASHING 이다.
 * rehash 중이면 tables[0] 의 rehash_idx 미만 버킷은 모두 비어 있고, 그 키들은 tables[1] 에 있다.
 * 늘리기가 실패하지 않는 한 tables[0] 의 load factor 는 1.0 이하다.
 * destroy_step 중이면 지우는 중인 테이블의 clear_idx 미만 버킷은 비어 있다.
 * 지우는 순서는 tables[0] 다음 tables[1] 이다 */
struct hash_table {
    struct ht_table tables[2];
    long rehash_idx;
    size_t clear_idx;
    ht_value_destroy_fn *value_destroy;
};

/* 모든 테이블이 같은 seed 를 쓴다. 설정 전에는 0 으로 채워져 있다 */
static uint8_t g_hash_seed[HT_SEED_BYTES];

static bool table_init(struct ht_table *table, size_t bucket_count);
static void table_clear(const struct hash_table *ht, struct ht_table *table);
static inline size_t table_index(const struct ht_table *table, uint64_t hash);
static struct ht_entry *table_find(const struct ht_table *table, const struct str *key,
                                   uint64_t hash);
static void table_push(struct ht_table *table, struct ht_entry *entry, uint64_t hash);
static struct ht_entry *table_unlink(struct ht_table *table, const struct str *key, uint64_t hash);
static const struct ht_entry *table_random_entry(const struct ht_table *table, size_t first_idx);
static struct ht_entry **alloc_buckets(size_t bucket_count);
static void grow_if_needed(struct hash_table *ht);
static void shrink_if_needed(struct hash_table *ht);
static void start_rehash(struct hash_table *ht, size_t new_bucket_count);
static struct ht_entry *find_entry(const struct hash_table *ht, const struct str *key,
                                   uint64_t hash);
static const struct ht_entry *random_entry_from(const struct hash_table *ht, int table_idx);
static bool skip_empty_buckets(struct hash_table *ht, size_t *empty_visits_left);
static void move_bucket(struct hash_table *ht);
static void finish_rehash(struct hash_table *ht);
static void replace_value(const struct hash_table *ht, struct ht_entry *entry, void *value);
static void destroy_value(const struct hash_table *ht, void *value);
static void destroy_entry(const struct hash_table *ht, struct ht_entry *entry);
static struct ht_table *next_table_to_clear(struct hash_table *ht);
static void skip_moved_buckets(struct hash_table *ht, int table_idx);
static void destroy_chain(const struct hash_table *ht, struct ht_table *table,
                          struct ht_entry *head);
static size_t random_below(size_t bound);
static const struct ht_entry *random_in_chain(const struct ht_entry *head);
static uint64_t load_le64(const unsigned char *bytes, size_t len);
static inline uint64_t rotl64(uint64_t x, int bits);
static void sip_round(struct sip_state *st);
static void sip_absorb(struct sip_state *st, uint64_t block, int rounds);

struct hash_table *hash_table_create(ht_value_destroy_fn *value_destroy)
{
    struct hash_table *ht = mem_alloc(sizeof(*ht));
    if (ht == NULL) {
        return NULL;
    }
    if (!table_init(&ht->tables[0], HT_INITIAL_SIZE)) {
        mem_free(ht);
        return NULL;
    }
    ht->tables[1] = (struct ht_table){0};
    ht->rehash_idx = NOT_REHASHING;
    ht->clear_idx = 0;
    ht->value_destroy = value_destroy;
    return ht;
}

void hash_table_destroy(struct hash_table *ht)
{
    if (ht == NULL) {
        return;
    }
    table_clear(ht, &ht->tables[0]);
    table_clear(ht, &ht->tables[1]);
    mem_free(ht);
}

/* 연산마다 rehash 를 한 칸 진행한다. rehash 중 새 키는 tables[1] 에만 넣는다:
 * tables[0] 의 이미 옮긴 버킷에 넣으면 교체 때 함께 사라진다 */
enum ht_result hash_table_set(struct hash_table *ht, struct str *key, void *value)
{
    hash_table_rehash_step(ht, 1);
    uint64_t hash = hash_table_hash(key);
    struct ht_entry *existing = find_entry(ht, key, hash);
    if (existing != NULL) {
        replace_value(ht, existing, value);
        str_destroy(key);
        return HT_OK;
    }

    struct ht_entry *entry = mem_alloc(sizeof(*entry));
    if (entry == NULL) {
        return HT_OUT_OF_MEMORY;
    }
    entry->key = key;
    entry->value = value;
    int dst_idx = hash_table_is_rehashing(ht) ? 1 : 0;
    table_push(&ht->tables[dst_idx], entry, hash);
    grow_if_needed(ht);
    return HT_OK;
}

/* const 라 rehash 를 진행하지 않는다. 진행은 set · delete 와 주기 작업이 맡는다 */
const struct ht_entry *hash_table_find(const struct hash_table *ht, const struct str *key)
{
    return find_entry(ht, key, hash_table_hash(key));
}

enum ht_result hash_table_delete(struct hash_table *ht, const struct str *key)
{
    hash_table_rehash_step(ht, 1);
    uint64_t hash = hash_table_hash(key);
    struct ht_entry *entry = table_unlink(&ht->tables[0], key, hash);
    if (entry == NULL) {
        entry = table_unlink(&ht->tables[1], key, hash);
    }
    if (entry == NULL) {
        return HT_NOT_FOUND;
    }
    destroy_entry(ht, entry);
    shrink_if_needed(ht);
    return HT_OK;
}

/* 두 테이블 중 하나를 엔트리 수에 비례해 고른 뒤 그 안에서 뽑는다. 못 찾으면 다른 쪽도 본다 */
const struct ht_entry *hash_table_random_entry(const struct hash_table *ht)
{
    size_t total_count = hash_table_count(ht);
    if (total_count == 0) {
        return NULL;
    }
    int first_idx = random_below(total_count) < ht->tables[0].entry_count ? 0 : 1;
    const struct ht_entry *entry = random_entry_from(ht, first_idx);
    if (entry == NULL) {
        entry = random_entry_from(ht, 1 - first_idx);
    }
    return entry;
}

size_t hash_table_count(const struct hash_table *ht)
{
    return ht->tables[0].entry_count + ht->tables[1].entry_count;
}

size_t hash_table_bucket_count(const struct hash_table *ht)
{
    if (hash_table_is_rehashing(ht)) {
        return ht->tables[1].bucket_count;
    }
    return ht->tables[0].bucket_count;
}

bool hash_table_is_rehashing(const struct hash_table *ht)
{
    return ht->rehash_idx != NOT_REHASHING;
}

/* 이벤트 루프가 한 번에 오래 막히지 않도록 옮기는 버킷 수와 지나치는 빈 버킷 수를 모두 제한한다 */
void hash_table_rehash_step(struct hash_table *ht, size_t bucket_count)
{
    size_t empty_visits_left = bucket_count * HT_REHASH_EMPTY_VISITS;
    for (size_t moved = 0; moved < bucket_count && hash_table_is_rehashing(ht); moved++) {
        if (ht->tables[0].entry_count == 0) {
            finish_rehash(ht);
            return;
        }
        if (!skip_empty_buckets(ht, &empty_visits_left)) {
            return;
        }
        move_bucket(ht);
    }
}

/* 시계를 매 버킷마다 읽지 않도록 100개씩 옮긴 뒤에만 경과 시간을 확인한다.
 * 그래서 예산을 최대 한 묶음만큼 넘길 수 있다 */
void hash_table_rehash_for_ms(struct hash_table *ht, int64_t budget_ms)
{
    int64_t start_ms = clock_now_ms();
    while (hash_table_is_rehashing(ht)) {
        hash_table_rehash_step(ht, HT_REHASH_CHUNK_BUCKETS);
        if (clock_now_ms() - start_ms >= budget_ms) {
            return;
        }
    }
}

/* 키 100만 개를 한 번에 해제하면 이벤트 루프가 멈추므로 버킷 묶음 단위로 나눠 해제한다.
 * 비어 버린 테이블은 남은 빈 버킷을 훑지 않고 버킷 배열을 바로 해제한다 */
bool hash_table_destroy_step(struct hash_table *ht, size_t bucket_count)
{
    size_t empty_visits_left = bucket_count * HT_REHASH_EMPTY_VISITS;
    size_t cleared_count = 0;
    for (;;) {
        struct ht_table *table = next_table_to_clear(ht);
        if (table == NULL) {
            mem_free(ht);
            return true;
        }
        if (cleared_count == bucket_count) {
            return false;
        }
        struct ht_entry *head = table->buckets[ht->clear_idx];
        table->buckets[ht->clear_idx] = NULL;
        ht->clear_idx++;
        if (head != NULL) {
            destroy_chain(ht, table, head);
            cleared_count++;
        } else if (empty_visits_left-- == 0) {
            return false;
        }
    }
}

uint64_t hash_table_hash(const struct str *key)
{
    uint64_t k0 = load_le64(g_hash_seed, SIP_BLOCK_BYTES);
    uint64_t k1 = load_le64(g_hash_seed + SIP_BLOCK_BYTES, SIP_BLOCK_BYTES);
    struct sip_state st = {
        .v0 = k0 ^ SIP_INIT_V0,
        .v1 = k1 ^ SIP_INIT_V1,
        .v2 = k0 ^ SIP_INIT_V2,
        .v3 = k1 ^ SIP_INIT_V3,
    };

    const unsigned char *data = (const unsigned char *)key->data;
    size_t full_len = key->len - key->len % SIP_BLOCK_BYTES;
    for (size_t i = 0; i < full_len; i += SIP_BLOCK_BYTES) {
        sip_absorb(&st, load_le64(data + i, SIP_BLOCK_BYTES), SIP_COMPRESSION_ROUNDS);
    }
    uint64_t last = load_le64(data + full_len, key->len - full_len);
    last |= (uint64_t)key->len << SIP_LEN_SHIFT;
    sip_absorb(&st, last, SIP_COMPRESSION_ROUNDS);

    st.v2 ^= SIP_FINAL_XOR;
    for (int i = 0; i < SIP_FINALIZATION_ROUNDS; i++) {
        sip_round(&st);
    }
    return st.v0 ^ st.v1 ^ st.v2 ^ st.v3;
}

void hash_table_set_seed(const uint8_t seed[HT_SEED_BYTES])
{
    memcpy(g_hash_seed, seed, HT_SEED_BYTES);
}

enum ht_result hash_table_load_random_seed(void)
{
    uint8_t seed[HT_SEED_BYTES];
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (urandom == NULL) {
        return HT_IO_ERROR;
    }
    size_t read_bytes = fread(seed, 1, sizeof(seed), urandom);
    fclose(urandom);
    if (read_bytes != sizeof(seed)) {
        return HT_IO_ERROR;
    }
    hash_table_set_seed(seed);
    return HT_OK;
}

static bool table_init(struct ht_table *table, size_t bucket_count)
{
    table->buckets = alloc_buckets(bucket_count);
    if (table->buckets == NULL) {
        return false;
    }
    table->bucket_count = bucket_count;
    table->entry_count = 0;
    return true;
}

/* 모든 엔트리와 버킷 배열을 해제하고 빈 테이블(buckets == NULL)로 되돌린다 */
static void table_clear(const struct hash_table *ht, struct ht_table *table)
{
    for (size_t i = 0; i < table->bucket_count; i++) {
        struct ht_entry *entry = table->buckets[i];
        while (entry != NULL) {
            struct ht_entry *next = entry->next;
            destroy_entry(ht, entry);
            entry = next;
        }
    }
    mem_free(table->buckets);
    *table = (struct ht_table){0};
}

/* bucket_count 가 2의 거듭제곱이라 나머지 연산 대신 하위 비트 마스크로 칸을 고른다 */
static inline size_t table_index(const struct ht_table *table, uint64_t hash)
{
    return (size_t)(hash & (table->bucket_count - 1));
}

static struct ht_entry *table_find(const struct ht_table *table, const struct str *key,
                                   uint64_t hash)
{
    if (table->entry_count == 0) {
        return NULL;
    }
    struct ht_entry *entry = table->buckets[table_index(table, hash)];
    while (entry != NULL && !str_equal(entry->key, key)) {
        entry = entry->next;
    }
    return entry;
}

/* 체인 맨 앞에 붙인다 (O(1)) */
static void table_push(struct ht_table *table, struct ht_entry *entry, uint64_t hash)
{
    size_t idx = table_index(table, hash);
    entry->next = table->buckets[idx];
    table->buckets[idx] = entry;
    table->entry_count++;
}

/* key 와 같은 엔트리를 체인에서 떼어 돌려준다. 해제는 호출자가 한다. 없으면 NULL */
static struct ht_entry *table_unlink(struct ht_table *table, const struct str *key, uint64_t hash)
{
    if (table->entry_count == 0) {
        return NULL;
    }
    struct ht_entry **link = &table->buckets[table_index(table, hash)];
    while (*link != NULL && !str_equal((*link)->key, key)) {
        link = &(*link)->next;
    }
    struct ht_entry *entry = *link;
    if (entry != NULL) {
        *link = entry->next;
        table->entry_count--;
    }
    return entry;
}

/* [first_idx, bucket_count) 안의 무작위 칸에서 시작해 그 범위만 훑는다.
 * 빈 버킷이 길게 이어져도 루프가 오래 돌지 않도록 탐색 칸 수에 상한을 둔다 */
static const struct ht_entry *table_random_entry(const struct ht_table *table, size_t first_idx)
{
    if (table->entry_count == 0) {
        return NULL;
    }
    size_t range_len = table->bucket_count - first_idx;
    size_t probe_limit = range_len < HT_RANDOM_MAX_PROBES ? range_len : HT_RANDOM_MAX_PROBES;
    size_t start_offset = random_below(range_len);
    for (size_t i = 0; i < probe_limit; i++) {
        const struct ht_entry *head = table->buckets[first_idx + (start_offset + i) % range_len];
        if (head != NULL) {
            return random_in_chain(head);
        }
    }
    return NULL;
}

static struct ht_entry *find_entry(const struct hash_table *ht, const struct str *key,
                                   uint64_t hash)
{
    struct ht_entry *entry = table_find(&ht->tables[0], key, hash);
    if (entry == NULL) {
        entry = table_find(&ht->tables[1], key, hash);
    }
    return entry;
}

/* rehash 중 tables[0] 의 rehash_idx 앞 버킷은 모두 비었으므로 그 뒤에서만 뽑는다 */
static const struct ht_entry *random_entry_from(const struct hash_table *ht, int table_idx)
{
    size_t first_idx = 0;
    if (table_idx == 0 && hash_table_is_rehashing(ht)) {
        first_idx = (size_t)ht->rehash_idx;
    }
    return table_random_entry(&ht->tables[table_idx], first_idx);
}

/* 버킷은 체인 머리 포인터의 배열이라 포인터 크기로 할당하는 것이 맞다 */
static struct ht_entry **alloc_buckets(size_t bucket_count)
{
    /* NOLINTNEXTLINE(bugprone-sizeof-expression) */
    return mem_calloc(bucket_count, sizeof(struct ht_entry *));
}

static void grow_if_needed(struct hash_table *ht)
{
    const struct ht_table *main_table = &ht->tables[0];
    if (main_table->entry_count > main_table->bucket_count &&
        main_table->bucket_count <= SIZE_MAX / 2) {
        start_rehash(ht, main_table->bucket_count * 2);
    }
}

static void shrink_if_needed(struct hash_table *ht)
{
    const struct ht_table *main_table = &ht->tables[0];
    bool is_sparse =
        main_table->entry_count * PERCENT_SCALE < main_table->bucket_count * SHRINK_FILL_PERCENT;
    if (is_sparse && main_table->bucket_count > HT_INITIAL_SIZE) {
        start_rehash(ht, main_table->bucket_count / 2);
    }
}

/* tables[1] 을 만들고 rehash_idx 를 0 으로 둔다. 이미 rehash 중이거나 할당이 실패하면 무시 */
static void start_rehash(struct hash_table *ht, size_t new_bucket_count)
{
    if (hash_table_is_rehashing(ht)) {
        return;
    }
    if (!table_init(&ht->tables[1], new_bucket_count)) {
        return;
    }
    ht->rehash_idx = 0;
}

/* rehash_idx 를 비지 않은 버킷까지 옮긴다. 예산을 다 쓰면 false.
 * tables[0] 에 엔트리가 남아 있으면 rehash_idx 이후에 비지 않은 버킷이 반드시 있다 */
static bool skip_empty_buckets(struct hash_table *ht, size_t *empty_visits_left)
{
    const struct ht_table *src = &ht->tables[0];
    while (src->buckets[ht->rehash_idx] == NULL) {
        if (*empty_visits_left == 0) {
            return false;
        }
        (*empty_visits_left)--;
        ht->rehash_idx++;
    }
    return true;
}

/* tables[0] 의 rehash_idx 번 버킷을 tables[1] 로 옮기고, tables[0] 이 비면 rehash 를 끝낸다 */
static void move_bucket(struct hash_table *ht)
{
    struct ht_table *src = &ht->tables[0];
    size_t idx = (size_t)ht->rehash_idx;
    struct ht_entry *entry = src->buckets[idx];
    src->buckets[idx] = NULL;
    while (entry != NULL) {
        struct ht_entry *next = entry->next;
        table_push(&ht->tables[1], entry, hash_table_hash(entry->key));
        src->entry_count--;
        entry = next;
    }
    ht->rehash_idx++;
    if (src->entry_count == 0) {
        finish_rehash(ht);
    }
}

/* 비어 버린 tables[0] 의 버킷 배열을 해제하고 tables[1] 을 tables[0] 자리로 옮긴다.
 * rehash 중에 쌓인 삽입 · 삭제로 다시 조건이 맞으면 다음 rehash 를 바로 시작한다 */
static void finish_rehash(struct hash_table *ht)
{
    mem_free(ht->tables[0].buckets);
    ht->tables[0] = ht->tables[1];
    ht->tables[1] = (struct ht_table){0};
    ht->rehash_idx = NOT_REHASHING;
    grow_if_needed(ht);
    shrink_if_needed(ht);
}

/* 같은 값 포인터로 덮어쓰면 해제하지 않는다. 해제하면 저장된 값이 바로 dangling 이 된다 */
static void replace_value(const struct hash_table *ht, struct ht_entry *entry, void *value)
{
    if (entry->value != value) {
        destroy_value(ht, entry->value);
    }
    entry->value = value;
}

static void destroy_value(const struct hash_table *ht, void *value)
{
    if (ht->value_destroy != NULL) {
        ht->value_destroy(value);
    }
}

static void destroy_entry(const struct hash_table *ht, struct ht_entry *entry)
{
    str_destroy(entry->key);
    destroy_value(ht, entry->value);
    mem_free(entry);
}

/* 엔트리가 남은 첫 테이블을 돌려준다. 비어 버린 테이블의 버킷 배열은 여기서 해제하고
 * 다음 테이블을 처음부터 지우도록 clear_idx 를 0 으로 되돌린다. 둘 다 비었으면 NULL.
 * 엔트리가 남아 있으면 clear_idx 이후에 비지 않은 버킷이 반드시 있다 */
static struct ht_table *next_table_to_clear(struct hash_table *ht)
{
    for (int i = 0; i < 2; i++) {
        struct ht_table *table = &ht->tables[i];
        if (table->buckets == NULL) {
            continue;
        }
        if (table->entry_count > 0) {
            skip_moved_buckets(ht, i);
            return table;
        }
        mem_free(table->buckets);
        *table = (struct ht_table){0};
        ht->clear_idx = 0;
    }
    return NULL;
}

/* rehash 중 tables[0] 의 rehash_idx 앞은 이미 옮겨져 비었으므로 그 뒤부터 지운다.
 * 0번부터 훑으면 옮긴 칸이 빈 칸 상한보다 많을 때 한 엔트리도 해제하지 못하고 멈춘다 */
static void skip_moved_buckets(struct hash_table *ht, int table_idx)
{
    if (table_idx != 0 || !hash_table_is_rehashing(ht)) {
        return;
    }
    if (ht->clear_idx < (size_t)ht->rehash_idx) {
        ht->clear_idx = (size_t)ht->rehash_idx;
    }
}

/* 버킷에서 떼어 낸 체인의 엔트리를 모두 해제하고 entry_count 를 맞춘다 */
static void destroy_chain(const struct hash_table *ht, struct ht_table *table,
                          struct ht_entry *head)
{
    while (head != NULL) {
        struct ht_entry *next = head->next;
        destroy_entry(ht, head);
        table->entry_count--;
        head = next;
    }
}

static size_t random_below(size_t bound)
{
    return (size_t)random() % bound;
}

/* 체인 길이를 몰라도 한 번 훑으며 k 번째 노드를 1/k 확률로 고르면 모든 노드가 균등하게 뽑힌다 */
static const struct ht_entry *random_in_chain(const struct ht_entry *head)
{
    const struct ht_entry *picked = head;
    size_t seen_count = 1;
    for (const struct ht_entry *entry = head->next; entry != NULL; entry = entry->next) {
        seen_count++;
        if (random_below(seen_count) == 0) {
            picked = entry;
        }
    }
    return picked;
}

/* bytes 의 앞 len 바이트(최대 8)를 little-endian 정수로 읽는다. 엔디언과 정렬에 상관없이 같다 */
static uint64_t load_le64(const unsigned char *bytes, size_t len)
{
    uint64_t value = 0;
    for (size_t i = 0; i < len; i++) {
        value |= (uint64_t)bytes[i] << (i * 8);
    }
    return value;
}

static inline uint64_t rotl64(uint64_t x, int bits)
{
    return (x << bits) | (x >> (64 - bits));
}

static void sip_round(struct sip_state *st)
{
    st->v0 += st->v1;
    st->v1 = rotl64(st->v1, SIP_ROT_A) ^ st->v0;
    st->v0 = rotl64(st->v0, SIP_ROT_HALF);
    st->v2 += st->v3;
    st->v3 = rotl64(st->v3, SIP_ROT_B) ^ st->v2;
    st->v0 += st->v3;
    st->v3 = rotl64(st->v3, SIP_ROT_C) ^ st->v0;
    st->v2 += st->v1;
    st->v1 = rotl64(st->v1, SIP_ROT_D) ^ st->v2;
    st->v2 = rotl64(st->v2, SIP_ROT_HALF);
}

static void sip_absorb(struct sip_state *st, uint64_t block, int rounds)
{
    st->v3 ^= block;
    for (int i = 0; i < rounds; i++) {
        sip_round(st);
    }
    st->v0 ^= block;
}
