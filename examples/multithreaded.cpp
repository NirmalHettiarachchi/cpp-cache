#include <iostream>
#include <thread>
#include <vector>

#include "cache.hpp"

int main() {
  cpp_cache::Cache<int, int, cpp_cache::LRUPolicy> cache(128);
  std::vector<std::thread> threads;

  for (int writer = 0; writer < 2; ++writer) {
    threads.emplace_back([writer, &cache]() {
      for (int i = 0; i < 5000; ++i) {
        const int key = writer * 5000 + i;
        cache.put(key, key * key);
      }
    });
  }

  for (int reader = 0; reader < 2; ++reader) {
    threads.emplace_back([reader, &cache]() {
      for (int i = 0; i < 5000; ++i) {
        (void)cache.get((i + reader * 17) % 10000);
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  const auto stats = cache.stats();
  std::cout << "All threads done\n";
  std::cout << "Final CacheStats final_size=" << cache.size() << " hits=" << stats.hits
            << " misses=" << stats.misses << " evictions=" << stats.evictions << '\n';
}
