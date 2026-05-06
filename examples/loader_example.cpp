#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "cache.hpp"

int main() {
  using namespace std::chrono_literals;

  cpp_cache::Cache<int, std::string, cpp_cache::LRUPolicy> users(32);
  std::atomic<int> database_calls{0};

  auto fetch_user = [&database_calls](const int& id) {
    ++database_calls;
    std::this_thread::sleep_for(100ms);
    return std::string("user-") + std::to_string(id);
  };

  std::cout << users.get_or_insert(7, fetch_user) << '\n';
  std::cout << users.get_or_insert(7, fetch_user) << '\n';
  std::cout << "database_calls_after_repeated_lookup=" << database_calls.load() << '\n';

  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([&users, &fetch_user]() {
      std::cout << users.get_or_insert(42, fetch_user) << '\n';
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  std::cout << "database_calls_after_contention=" << database_calls.load() << '\n';
  std::cout << "cache_hits=" << users.stats().hits << " misses=" << users.stats().misses << '\n';
}
