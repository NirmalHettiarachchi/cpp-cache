#ifndef CPP_CACHE_CACHE_HPP_
#define CPP_CACHE_CACHE_HPP_

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace cpp_cache {

struct CacheStats {
  std::size_t hits = 0;
  std::size_t misses = 0;
  std::size_t evictions = 0;
  std::size_t expired = 0;
  std::size_t inserts = 0;
  double hit_rate = 0.0;
  double eviction_rate = 0.0;
};

template <typename K, typename V>
class LRUPolicy;

template <typename K, typename V, template <typename, typename> class Policy = LRUPolicy>
class Cache {
 public:
  explicit Cache(std::size_t capacity);
  std::optional<V> get(const K& key);
  void put(const K& key, const V& value);
  bool contains(const K& key) const;
  bool erase(const K& key);
  void clear();
  std::size_t size() const;
  std::size_t capacity() const;
  V get_or_insert(const K& key, std::function<V(const K&)> loader);
  void set_on_evict(std::function<void(const K&, const V&)> cb);
  void set_on_insert(std::function<void(const K&, const V&)> cb);
  CacheStats stats() const;
  std::vector<std::pair<K, V>> snapshot() const;
  void resize(std::size_t new_capacity);
};

}  // namespace cpp_cache

#endif  // CPP_CACHE_CACHE_HPP_
