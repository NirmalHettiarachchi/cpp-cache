#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "cache.hpp"
#include "doctest/doctest.h"

using namespace std::chrono_literals;

TEST_CASE("LRU basic put/get and eviction") {
  cpp_cache::Cache<int, std::string, cpp_cache::LRUPolicy> cache(2);
  cache.put(1, "one");
  cache.put(2, "two");

  CHECK(cache.get(1) == "one");
  cache.put(3, "three");

  CHECK_FALSE(cache.contains(2));
  CHECK(cache.get(1) == "one");
  CHECK(cache.get(3) == "three");
}

TEST_CASE("LRU re-access updates recency and capacity one works") {
  cpp_cache::Cache<int, int, cpp_cache::LRUPolicy> cache(2);
  cache.put(1, 10);
  cache.put(2, 20);
  CHECK(cache.get(1) == 10);
  cache.put(3, 30);
  CHECK(cache.contains(1));
  CHECK_FALSE(cache.contains(2));

  cpp_cache::Cache<int, int, cpp_cache::LRUPolicy> one(1);
  one.put(1, 1);
  one.put(2, 2);
  CHECK_FALSE(one.contains(1));
  CHECK(one.get(2) == 2);
}

TEST_CASE("LRU overwrite and resize") {
  cpp_cache::Cache<int, std::string, cpp_cache::LRUPolicy> cache(3);
  cache.put(1, "one");
  cache.put(2, "two");
  cache.put(1, "uno");
  cache.put(3, "three");
  cache.put(4, "four");

  CHECK(cache.get(1) == "uno");
  CHECK_FALSE(cache.contains(2));

  cache.resize(2);
  CHECK(cache.size() == 2);
  cache.resize(4);
  cache.put(5, "five");
  cache.put(6, "six");
  CHECK(cache.size() == 4);
}

TEST_CASE("LFU evicts least frequent, not least recent") {
  cpp_cache::Cache<int, std::string, cpp_cache::LFUPolicy> cache(3);
  cache.put(1, "one");
  cache.put(2, "two");
  cache.put(3, "three");
  CHECK(cache.get(1) == "one");
  CHECK(cache.get(1) == "one");
  CHECK(cache.get(2) == "two");

  cache.put(4, "four");
  CHECK_FALSE(cache.contains(3));
  CHECK(cache.contains(1));
  CHECK(cache.contains(2));
  CHECK(cache.contains(4));
}

TEST_CASE("LFU tie-breaking uses LRU among equal frequencies") {
  cpp_cache::Cache<int, int, cpp_cache::LFUPolicy> cache(2);
  cache.put(1, 1);
  cache.put(2, 2);
  cache.put(3, 3);

  CHECK_FALSE(cache.contains(1));
  CHECK(cache.contains(2));
  CHECK(cache.contains(3));
}

TEST_CASE("LFU overwrite increments and preserves frequency") {
  cpp_cache::Cache<int, int, cpp_cache::LFUPolicy> cache(2);
  cache.put(1, 1);
  cache.put(2, 2);
  cache.put(1, 10);
  cache.put(3, 3);

  CHECK(cache.get(1) == 10);
  CHECK_FALSE(cache.contains(2));
  CHECK(cache.contains(3));
}

TEST_CASE("FIFO get does not affect insertion order") {
  cpp_cache::Cache<int, int, cpp_cache::FIFOPolicy> cache(2);
  cache.put(1, 1);
  cache.put(2, 2);
  CHECK(cache.get(1) == 1);
  cache.put(3, 3);

  CHECK_FALSE(cache.contains(1));
  CHECK(cache.contains(2));
  CHECK(cache.contains(3));
}

TEST_CASE("FIFO overwrite does not reset insertion order") {
  cpp_cache::Cache<int, int, cpp_cache::FIFOPolicy> cache(2);
  cache.put(1, 1);
  cache.put(2, 2);
  cache.put(1, 10);
  cache.put(3, 3);

  CHECK_FALSE(cache.contains(1));
  CHECK(cache.contains(2));
  CHECK(cache.contains(3));
}

TEST_CASE("SLRU new entries are probationary and second access promotes") {
  cpp_cache::Cache<int, int, cpp_cache::SLRUPolicy> cache(4, 0.5);
  cache.put(1, 1);
  cache.put(2, 2);
  cache.put(3, 3);
  CHECK_FALSE(cache.contains(1));

  cpp_cache::Cache<int, int, cpp_cache::SLRUPolicy> promoted(4, 0.5);
  promoted.put(1, 1);
  promoted.put(2, 2);
  CHECK(promoted.get(1) == 1);
  promoted.put(3, 3);
  promoted.put(4, 4);
  promoted.put(5, 5);
  CHECK(promoted.get(1) == 1);
}

TEST_CASE("SLRU evicts probationary tail before protected entries") {
  cpp_cache::Cache<int, int, cpp_cache::SLRUPolicy> cache(5);
  cache.put(1, 1);
  CHECK(cache.get(1) == 1);

  for (int key = 2; key < 20; ++key) {
    cache.put(key, key);
  }

  CHECK(cache.get(1) == 1);
  CHECK(cache.size() <= cache.capacity());
}

TEST_CASE("SLRU protected entries are only lost after resize pressure") {
  cpp_cache::Cache<int, int, cpp_cache::SLRUPolicy> cache(3, 0.67);
  cache.put(1, 1);
  CHECK(cache.get(1) == 1);
  cache.put(2, 2);
  CHECK(cache.get(2) == 2);
  cache.put(3, 3);
  cache.put(4, 4);

  CHECK(cache.contains(1));
  CHECK(cache.contains(2));
  cache.resize(1);
  CHECK(cache.size() == 1);
}

TEST_CASE("TTL entries expire lazily and update expired stats") {
  cpp_cache::Cache<int, std::string, cpp_cache::TTLPolicy> cache(2, 40ms);
  cache.put(1, "one");
  CHECK(cache.get(1) == "one");
  std::this_thread::sleep_for(70ms);
  CHECK_FALSE(cache.get(1).has_value());
  CHECK(cache.stats().expired >= 1);
}

TEST_CASE("TTL expired entries do not count toward capacity") {
  cpp_cache::Cache<int, int, cpp_cache::TTLPolicy> cache(1, 30ms);
  cache.put(1, 1);
  std::this_thread::sleep_for(60ms);
  cache.put(2, 2);

  CHECK(cache.get(2) == 2);
  CHECK(cache.stats().evictions == 0);
  CHECK(cache.stats().expired >= 1);
}

TEST_CASE("TTL timing wheel prunes expired entries in the background") {
  cpp_cache::Cache<int, int, cpp_cache::TTLPolicy> cache(4, 30ms);
  cache.put(1, 1);
  cache.put(2, 2);
  std::this_thread::sleep_for(150ms);

  CHECK(cache.size() == 0);
  CHECK(cache.stats().expired >= 2);
}

TEST_CASE("make_memoized returns values and caches repeated calls") {
  std::atomic<int> calls{0};
  auto memoized = cpp_cache::make_memoized<cpp_cache::LRUPolicy>(8, [&calls](int value) {
    ++calls;
    return value * 2;
  });

  CHECK(memoized(21) == 42);
  CHECK(memoized(21) == 42);
  CHECK(calls.load() == 1);
  CHECK(memoized.stats().hits == 1);
}

TEST_CASE("make_memoized is thread-safe for a single hot key") {
  std::atomic<int> calls{0};
  auto memoized = cpp_cache::make_memoized<cpp_cache::LRUPolicy>(8, [&calls](int value) {
    ++calls;
    std::this_thread::sleep_for(20ms);
    return value + 1;
  });

  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&memoized, &failures]() {
      if (memoized(10) != 11) {
        ++failures;
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  CHECK(failures.load() == 0);
  CHECK(calls.load() == 1);
}

TEST_CASE("get_or_insert returns existing value without loading") {
  cpp_cache::Cache<int, int, cpp_cache::LRUPolicy> cache(2);
  cache.put(1, 10);
  bool called = false;

  CHECK(cache.get_or_insert(1, [&called](const int&) {
          called = true;
          return 99;
        }) == 10);
  CHECK_FALSE(called);
}

TEST_CASE("get_or_insert loads on miss and prevents thundering herd") {
  cpp_cache::Cache<int, int, cpp_cache::LRUPolicy> cache(8);
  std::atomic<int> calls{0};
  auto loader = [&calls](const int& key) {
    ++calls;
    std::this_thread::sleep_for(20ms);
    return key * 3;
  };

  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&cache, &loader, &failures]() {
      if (cache.get_or_insert(7, loader) != 21) {
        ++failures;
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  CHECK(failures.load() == 0);
  CHECK(calls.load() == 1);
  CHECK(cache.get(7) == 21);
}

TEST_CASE("callbacks fire with correct key and value") {
  cpp_cache::Cache<int, std::string, cpp_cache::LRUPolicy> cache(1);
  int inserted = 0;
  int evicted_key = 0;
  std::string evicted_value;

  cache.set_on_insert([&inserted](const int&, const std::string&) { ++inserted; });
  cache.set_on_evict([&evicted_key, &evicted_value](const int& key, const std::string& value) {
    evicted_key = key;
    evicted_value = value;
  });

  cache.put(1, "one");
  cache.put(2, "two");

  CHECK(inserted == 2);
  CHECK(evicted_key == 1);
  CHECK(evicted_value == "one");
}

template <template <typename, typename> class Policy>
void check_callbacks_for_policy() {
  cpp_cache::Cache<int, int, Policy> cache(2);
  int evictions = 0;
  int insertions = 0;
  cache.set_on_insert([&insertions](const int&, const int&) { ++insertions; });
  cache.set_on_evict([&evictions](const int&, const int&) { ++evictions; });
  cache.put(1, 1);
  cache.put(2, 2);
  cache.put(3, 3);
  CHECK(insertions == 3);
  CHECK(evictions >= 1);
}

TEST_CASE("callbacks work across non-TTL policies") {
  check_callbacks_for_policy<cpp_cache::LRUPolicy>();
  check_callbacks_for_policy<cpp_cache::LFUPolicy>();
  check_callbacks_for_policy<cpp_cache::FIFOPolicy>();
  check_callbacks_for_policy<cpp_cache::SLRUPolicy>();
}

TEST_CASE("callbacks work with TTL capacity eviction") {
  cpp_cache::Cache<int, int, cpp_cache::TTLPolicy> cache(1, 1h);
  int evictions = 0;
  cache.set_on_evict([&evictions](const int&, const int&) { ++evictions; });
  cache.put(1, 1);
  cache.put(2, 2);
  CHECK(evictions == 1);
}

TEST_CASE("concurrent puts and gets are race-free and bounded") {
  cpp_cache::Cache<int, int, cpp_cache::LRUPolicy> cache(64);
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back([i, &cache]() {
      for (int j = 0; j < 1000; ++j) {
        cache.put(i * 1000 + j, j);
        (void)cache.get(j);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  CHECK(cache.size() <= cache.capacity());
  CHECK(cache.stats().hits + cache.stats().misses == 4000);
}

TEST_CASE("snapshot remains consistent under concurrent writes") {
  cpp_cache::Cache<int, int, cpp_cache::LRUPolicy> cache(32);
  std::atomic<bool> done{false};
  std::thread writer([&cache, &done]() {
    for (int i = 0; i < 2000; ++i) {
      cache.put(i, i);
    }
    done = true;
  });

  while (!done.load()) {
    const auto snapshot = cache.snapshot();
    CHECK(snapshot.size() <= cache.capacity());
  }
  writer.join();
}
