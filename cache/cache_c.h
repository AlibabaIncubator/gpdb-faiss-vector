
#ifndef CACHE_C_H_
#define CACHE_C_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif
    typedef struct cache_t cache_t;
    typedef struct handle_t handle_t;

    cache_t *cache_create_lru(size_t capacity);
    void cache_destroy(cache_t *cache);

    handle_t *cache_insert(cache_t *cache, const char *key, size_t keylen, void *value, size_t charge, void (*deleter)(const char *key, size_t keylen, void *value));
    handle_t *cache_lookup(cache_t *cache, const char *key, size_t keylen);
    void cache_release(cache_t *cache, handle_t *handle);
    void *cache_value(cache_t *cache, handle_t *handle);
    void cache_erase(cache_t *cache, const char *key, size_t keylen);
    uint64_t cache_new_id(cache_t *cache);
    void cache_prune(cache_t *cache);
    size_t cache_total_charge(cache_t *cache);

#ifdef __cplusplus
} /* end extern "C" */
#endif

#endif /* CACHE_C_H_ */
