#include "hash_table.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* 불변식: bucket_count 는 2의 거듭제곱이고, entry_count 는 모든 체인의 노드 수와 같다.
 * 늘리기가 실패하지 않는 한 entry_count <= bucket_count (load factor 1.0 이하) 를 유지한다 */
struct sip_state {
    uint64_t v0;
    uint64_t v1;
    uint64_t v2;
    uint64_t v3;
};

struct hash_table {
    struct ht_entry **buckets;
    size_t bucket_count;
    size_t entry_count;
    ht_value_destroy_fn *value_destroy;
};

/* 모든 테이블이 같은 seed 를 쓴다. 설정 전에는 0 으로 채워져 있다 */
static uint8_t g_hash_seed[HT_SEED_BYTES];

static struct ht_entry **alloc_buckets(size_t bucket_count);
static void destroy_chain(const struct hash_table *ht, struct ht_entry *entry);
static inline size_t bucket_index(const struct hash_table *ht, uint64_t hash);
static inline size_t index_for(uint64_t hash, size_t bucket_count);
static void grow_if_needed(struct hash_table *ht);
static void shrink_if_needed(struct hash_table *ht);
static void resize(struct hash_table *ht, size_t new_bucket_count);
static void move_chain(struct ht_entry *entry, struct ht_entry **new_buckets, size_t new_count);
static struct ht_entry *find_in_chain(struct ht_entry *entry, const struct str *key);
static void replace_value(const struct hash_table *ht, struct ht_entry *entry, void *value);
static void destroy_value(const struct hash_table *ht, void *value);
static void destroy_entry(const struct hash_table *ht, struct ht_entry *entry);
static size_t random_below(size_t bound);
static uint64_t load_le64(const unsigned char *bytes, size_t len);
static inline uint64_t rotl64(uint64_t x, int bits);
static void sip_round(struct sip_state *st);
static void sip_absorb(struct sip_state *st, uint64_t block, int rounds);
static const struct ht_entry *random_in_chain(const struct ht_entry *head);

struct hash_table *hash_table_create(ht_value_destroy_fn *value_destroy)
{
    struct hash_table *ht = mem_alloc(sizeof(*ht));
    if (ht == NULL) {
        return NULL;
    }
    ht->buckets = alloc_buckets(HT_INITIAL_SIZE);
    if (ht->buckets == NULL) {
        mem_free(ht);
        return NULL;
    }
    ht->bucket_count = HT_INITIAL_SIZE;
    ht->entry_count = 0;
    ht->value_destroy = value_destroy;
    return ht;
}

void hash_table_destroy(struct hash_table *ht)
{
    if (ht == NULL) {
        return;
    }
    for (size_t i = 0; i < ht->bucket_count; i++) {
        destroy_chain(ht, ht->buckets[i]);
    }
    mem_free(ht->buckets);
    mem_free(ht);
}

enum ht_result hash_table_set(struct hash_table *ht, struct str *key, void *value)
{
    size_t idx = bucket_index(ht, hash_table_hash(key));
    struct ht_entry *existing = find_in_chain(ht->buckets[idx], key);
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
    entry->next = ht->buckets[idx];
    ht->buckets[idx] = entry;
    ht->entry_count++;
    grow_if_needed(ht);
    return HT_OK;
}

const struct ht_entry *hash_table_find(const struct hash_table *ht, const struct str *key)
{
    size_t idx = bucket_index(ht, hash_table_hash(key));
    return find_in_chain(ht->buckets[idx], key);
}

enum ht_result hash_table_delete(struct hash_table *ht, const struct str *key)
{
    struct ht_entry **link = &ht->buckets[bucket_index(ht, hash_table_hash(key))];
    while (*link != NULL && !str_equal((*link)->key, key)) {
        link = &(*link)->next;
    }
    if (*link == NULL) {
        return HT_NOT_FOUND;
    }

    struct ht_entry *entry = *link;
    *link = entry->next;
    destroy_entry(ht, entry);
    ht->entry_count--;
    shrink_if_needed(ht);
    return HT_OK;
}

/* 빈 버킷이 길게 이어져도 루프가 오래 돌지 않도록 탐색 칸 수에 상한을 둔다 */
const struct ht_entry *hash_table_random_entry(const struct hash_table *ht)
{
    if (ht->entry_count == 0) {
        return NULL;
    }
    size_t probe_limit = ht->bucket_count;
    if (probe_limit > HT_RANDOM_MAX_PROBES) {
        probe_limit = HT_RANDOM_MAX_PROBES;
    }
    size_t start_idx = random_below(ht->bucket_count);
    for (size_t i = 0; i < probe_limit; i++) {
        const struct ht_entry *head = ht->buckets[(start_idx + i) & (ht->bucket_count - 1)];
        if (head != NULL) {
            return random_in_chain(head);
        }
    }
    return NULL;
}

size_t hash_table_count(const struct hash_table *ht)
{
    return ht->entry_count;
}

size_t hash_table_bucket_count(const struct hash_table *ht)
{
    return ht->bucket_count;
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

/* 버킷은 체인 머리 포인터의 배열이라 포인터 크기로 할당하는 것이 맞다 */
static struct ht_entry **alloc_buckets(size_t bucket_count)
{
    /* NOLINTNEXTLINE(bugprone-sizeof-expression) */
    return mem_calloc(bucket_count, sizeof(struct ht_entry *));
}

static void destroy_chain(const struct hash_table *ht, struct ht_entry *entry)
{
    while (entry != NULL) {
        struct ht_entry *next = entry->next;
        destroy_entry(ht, entry);
        entry = next;
    }
}

/* bucket_count 가 2의 거듭제곱이라 나머지 연산 대신 하위 비트 마스크로 칸을 고른다 */
static inline size_t bucket_index(const struct hash_table *ht, uint64_t hash)
{
    return index_for(hash, ht->bucket_count);
}

static inline size_t index_for(uint64_t hash, size_t bucket_count)
{
    return (size_t)(hash & (bucket_count - 1));
}

static void grow_if_needed(struct hash_table *ht)
{
    if (ht->entry_count > ht->bucket_count && ht->bucket_count <= SIZE_MAX / 2) {
        resize(ht, ht->bucket_count * 2);
    }
}

static void shrink_if_needed(struct hash_table *ht)
{
    bool is_sparse = ht->entry_count * PERCENT_SCALE < ht->bucket_count * SHRINK_FILL_PERCENT;
    if (is_sparse && ht->bucket_count > HT_INITIAL_SIZE) {
        resize(ht, ht->bucket_count / 2);
    }
}

/* 새 버킷 할당이 실패하면 크기를 그대로 둔다. 느려질 뿐 데이터는 잃지 않는다 */
static void resize(struct hash_table *ht, size_t new_bucket_count)
{
    struct ht_entry **new_buckets = alloc_buckets(new_bucket_count);
    if (new_buckets == NULL) {
        return;
    }
    for (size_t i = 0; i < ht->bucket_count; i++) {
        move_chain(ht->buckets[i], new_buckets, new_bucket_count);
    }
    mem_free(ht->buckets);
    ht->buckets = new_buckets;
    ht->bucket_count = new_bucket_count;
}

static void move_chain(struct ht_entry *entry, struct ht_entry **new_buckets, size_t new_count)
{
    while (entry != NULL) {
        struct ht_entry *next = entry->next;
        size_t idx = index_for(hash_table_hash(entry->key), new_count);
        entry->next = new_buckets[idx];
        new_buckets[idx] = entry;
        entry = next;
    }
}

static struct ht_entry *find_in_chain(struct ht_entry *entry, const struct str *key)
{
    while (entry != NULL && !str_equal(entry->key, key)) {
        entry = entry->next;
    }
    return entry;
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
