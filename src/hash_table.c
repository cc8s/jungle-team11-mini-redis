#include "hash_table.h"

#include "mem.h"

static const uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
static const uint64_t FNV_PRIME = 0x100000001b3ULL;

/* 불변식: bucket_count 는 2의 거듭제곱이고, entry_count 는 모든 체인의 노드 수와 같다 */
struct hash_table {
    struct ht_entry **buckets;
    size_t bucket_count;
    size_t entry_count;
    ht_value_destroy_fn *value_destroy;
};

static struct ht_entry **alloc_buckets(size_t bucket_count);
static void destroy_chain(const struct hash_table *ht, struct ht_entry *entry);
static inline size_t bucket_index(const struct hash_table *ht, uint64_t hash);
static struct ht_entry *find_in_chain(struct ht_entry *entry, const struct str *key);
static void replace_value(const struct hash_table *ht, struct ht_entry *entry, void *value);
static void destroy_value(const struct hash_table *ht, void *value);

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
    return HT_OK;
}

const struct ht_entry *hash_table_find(const struct hash_table *ht, const struct str *key)
{
    size_t idx = bucket_index(ht, hash_table_hash(key));
    return find_in_chain(ht->buckets[idx], key);
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
        str_destroy(entry->key);
        destroy_value(ht, entry->value);
        mem_free(entry);
        entry = next;
    }
}

/* bucket_count 가 2의 거듭제곱이라 나머지 연산 대신 하위 비트 마스크로 칸을 고른다 */
static inline size_t bucket_index(const struct hash_table *ht, uint64_t hash)
{
    return (size_t)(hash & (ht->bucket_count - 1));
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
