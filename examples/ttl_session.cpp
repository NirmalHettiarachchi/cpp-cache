#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "cache.hpp"

int main() {
  using namespace std::chrono_literals;

  cpp_cache::Cache<std::string, std::string, cpp_cache::TTLPolicy> sessions(10, 2s);
  sessions.put("session:42", "nirmal");

  std::cout << "Key found: " << sessions.get("session:42").value_or("<expired>") << '\n';
  std::this_thread::sleep_for(3s);
  std::cout << "Key expired: " << sessions.get("session:42").value_or("<expired>") << '\n';

  sessions.put("session:43", "fresh");
  std::cout << "fresh session: " << sessions.get("session:43").value_or("<expired>") << '\n';

  const auto stats = sessions.stats();
  std::cout << "CacheStats hits=" << stats.hits << " misses=" << stats.misses
            << " expired=" << stats.expired << '\n';
}
