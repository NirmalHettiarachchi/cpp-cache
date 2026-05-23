#include <iostream>
#include <string>
#include <vector>

#include "cache.hpp"

int main() {
  cpp_cache::Cache<int, std::string, cpp_cache::LRUPolicy> lru(2);
  std::vector<int> evicted_keys;
  int insertions = 0;

  lru.set_on_evict([&evicted_keys](const int& key, const std::string& value) {
    std::cout << "on_evict " << key << "=" << value << '\n';
    evicted_keys.push_back(key);
  });
  lru.set_on_insert([&insertions](const int& key, const std::string& value) {
    ++insertions;
    std::cout << "on_insert " << key << "=" << value << '\n';
  });

  lru.put(1, "one");
  lru.put(2, "two");
  lru.put(3, "three");

  cpp_cache::Cache<int, std::string, cpp_cache::SLRUPolicy> slru(5);
  slru.set_on_evict([](const int& key, const std::string& value) {
    std::cout << "SLRU probationary eviction, not protected: " << key << "=" << value << '\n';
  });
  slru.put(1, "hot");
  (void)slru.get(1);  // Promote to protected.
  slru.put(2, "scan-2");
  slru.put(3, "scan-3");
  slru.put(4, "scan-4");
  slru.put(5, "scan-5");

  std::cout << "SLRU protected key 1 survives scan: " << slru.get(1).value_or("<missing>")
            << '\n';
  std::cout << "LRU insertions=" << insertions << " evicted_count=" << evicted_keys.size()
            << '\n';
}
