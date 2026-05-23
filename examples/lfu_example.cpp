#include <iostream>
#include <string>

#include "cache.hpp"

int main() {
  cpp_cache::Cache<int, std::string, cpp_cache::LFUPolicy> cache(3);

  cache.put(1, "one");
  cache.put(2, "two");
  cache.put(3, "three");

  (void)cache.get(1);
  (void)cache.get(1);
  (void)cache.get(2);

  cache.put(4, "four");
  std::cout << "Least frequent eviction: key 3 evicted because it had the lowest access count -> "
            << cache.get(3).value_or("<evicted>") << '\n';

  // Build an equal-frequency tie between 2 and 4. Key 2 is older in its bucket,
  // so it loses the LFU tie-break.
  (void)cache.get(4);
  cache.put(5, "five");

  std::cout << "Tie-breaking: keys 2 and 4 had equal frequency; LRU key 2 was evicted\n";
  std::cout << "key 1: " << cache.get(1).value_or("<missing>") << '\n';
  std::cout << "key 2: " << cache.get(2).value_or("<evicted>") << '\n';
  std::cout << "key 4: " << cache.get(4).value_or("<missing>") << '\n';
  std::cout << "key 5: " << cache.get(5).value_or("<missing>") << '\n';

  const auto stats = cache.stats();
  std::cout << "hits=" << stats.hits << " misses=" << stats.misses
            << " evictions=" << stats.evictions << '\n';
}
