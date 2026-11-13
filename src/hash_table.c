#include "hash_table.h"

#include <stdint.h>
#include <stdlib.h>

#include "mem.h"

static const uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
static const uint64_t FNV_PRIME = 0x100000001b3ULL;

enum {
    SHRINK_FILL_PERCENT = 10,
    PERCENT_SCALE = 100,
};

/* 불변식: bucket_count 는 2의 거듭제곱이고, entry_count 는 모든 체인의 노드 수와 같다.
 * 늘리기가 실패하지 않는 한 entry_count <= bucket_count (load factor 1.0 이하) 를 유지한다 */
struct hash_table {
    struct ht_entry **buckets;
    size_t bucket_count;
    size_t entry_count;
    ht_value_destroy_fn *value_destroy;
};

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
    uint64_t hash = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < key->len; i++) {
        hash ^= (unsigned char)key->data[i];
        hash *= FNV_PRIME;
    }
    return hash;
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
