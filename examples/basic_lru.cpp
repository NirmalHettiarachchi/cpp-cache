#include <iostream>
#include <string>

#include "cache.hpp"

int main() {
  cpp_cache::Cache<std::string, std::string, cpp_cache::LRUPolicy> cache(3);

  cache.put("a", "alpha");
  cache.put("b", "bravo");
  cache.put("c", "charlie");
  cache.put("d", "delta");

  const auto evicted = cache.get("a");
  std::cout << "Evicted: a not found -> " << evicted.value_or("<missing>") << '\n';
  std::cout << "b: " << cache.get("b").value_or("<missing>") << '\n';
  std::cout << "c: " << cache.get("c").value_or("<missing>") << '\n';
  std::cout << "d: " << cache.get("d").value_or("<missing>") << '\n';

  const auto stats = cache.stats();
  std::cout << "CacheStats hits=" << stats.hits << " misses=" << stats.misses
            << " evictions=" << stats.evictions << " hit_rate=" << stats.hit_rate << '\n';
}
