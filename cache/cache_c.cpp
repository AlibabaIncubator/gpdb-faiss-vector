/*  Copyright 2022 Alibaba Group. All rights reserved.

    Distributed under MIT license.
    See file LICENSE for detail or copy at https://opensource.org/licenses/MIT
*/

#include <functional>
#include <iostream>

#include "cache_c.h"
#include "cache.h"

#ifndef CATCH_AND_HANDLE
#define CATCH_AND_HANDLE                                     \
  catch (std::exception & e)                                 \
  {                                                          \
    std::cerr << "c++ exception: " << e.what() << std::endl; \
    assert(0);                                               \
  }
#endif /* CATCH_AND_HANDLE */

cache_t *cache_create_lru(size_t capacity)
{
  try
  {
    return reinterpret_cast<cache_t *>(NewLRUCache(capacity));
  }
  CATCH_AND_HANDLE
}

void cache_destroy(cache_t *cache)
{
  try
  {
    delete reinterpret_cast<Cache *>(cache);
  }
  CATCH_AND_HANDLE
}

handle_t *cache_insert(cache_t *cache, const char *key, size_t keylen, void *value, size_t charge, void (*deleter)(const char *key, size_t keylen, void *value))
{
  try
  {
    return reinterpret_cast<handle_t *>(reinterpret_cast<Cache *>(cache)->Insert(Slice(key, keylen), value, charge, deleter));
  }
  CATCH_AND_HANDLE
}

handle_t *cache_lookup(cache_t *cache, const char *key, size_t keylen)
{
  try
  {
    return reinterpret_cast<handle_t *>(reinterpret_cast<Cache *>(cache)->Lookup(Slice(key, keylen)));
  }
  CATCH_AND_HANDLE
}

void cache_release(cache_t *cache, handle_t *handle)
{
  try
  {
    reinterpret_cast<Cache *>(cache)->Release(reinterpret_cast<Cache::Handle *>(handle));
  }
  CATCH_AND_HANDLE
}

void *cache_value(cache_t *cache, handle_t *handle)
{
  try
  {
    return reinterpret_cast<Cache *>(cache)->Value(reinterpret_cast<Cache::Handle *>(handle));
  }
  CATCH_AND_HANDLE
}

void cache_erase(cache_t *cache, const char *key, size_t keylen)
{
  try
  {
    reinterpret_cast<Cache *>(cache)->Erase(Slice(key, keylen));
  }
  CATCH_AND_HANDLE
}

uint64_t cache_new_id(cache_t *cache)
{
  try
  {
    return reinterpret_cast<Cache *>(cache)->NewId();
  }
  CATCH_AND_HANDLE
}

void cache_prune(cache_t *cache)
{
  try
  {
    reinterpret_cast<Cache *>(cache)->Prune();
  }
  CATCH_AND_HANDLE
}

size_t cache_total_charge(cache_t *cache)
{
  try
  {
    return reinterpret_cast<Cache *>(cache)->TotalCharge();
  }
  CATCH_AND_HANDLE
}
