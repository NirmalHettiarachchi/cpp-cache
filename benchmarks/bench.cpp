#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "cache.hpp"

namespace {

using Clock = std::chrono::high_resolution_clock;

struct BenchResult {
  double ops_per_second = 0.0;
  double hit_rate = 0.0;
};

template <template <typename, typename> class Policy>
struct CacheFactory {
  static std::unique_ptr<cpp_cache::Cache<int, int, Policy>> make(std::size_t capacity) {
    return std::make_unique<cpp_cache::Cache<int, int, Policy>>(capacity);
  }
};

template <>
struct CacheFactory<cpp_cache::TTLPolicy> {
  static std::unique_ptr<cpp_cache::Cache<int, int, cpp_cache::TTLPolicy>> make(
      std::size_t capacity) {
    return std::make_unique<cpp_cache::Cache<int, int, cpp_cache::TTLPolicy>>(
        capacity, std::chrono::hours(1));
  }
};

std::vector<int> sequential_keys(std::size_t operations, int key_space) {
  std::vector<int> keys;
  keys.reserve(operations);
  for (std::size_t i = 0; i < operations; ++i) {
    keys.push_back(static_cast<int>(i % static_cast<std::size_t>(key_space)));
  }
  return keys;
}

std::vector<int> random_keys(std::size_t operations, int key_space) {
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(0, key_space - 1);
  std::vector<int> keys;
  keys.reserve(operations);
  for (std::size_t i = 0; i < operations; ++i) {
    keys.push_back(dist(rng));
  }
  return keys;
}

std::vector<int> zipf_keys(std::size_t operations, int key_space, double exponent = 1.15) {
  std::vector<double> weights(static_cast<std::size_t>(key_space));
  for (int i = 1; i <= key_space; ++i) {
    weights[static_cast<std::size_t>(i - 1)] = 1.0 / std::pow(static_cast<double>(i), exponent);
  }
  const double total = std::accumulate(weights.begin(), weights.end(), 0.0);
  std::vector<double> cdf;
  cdf.reserve(weights.size());
  double running = 0.0;
  for (double weight : weights) {
    running += weight / total;
    cdf.push_back(running);
  }

  std::mt19937 rng(43);
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  std::vector<int> keys;
  keys.reserve(operations);
  for (std::size_t i = 0; i < operations; ++i) {
    const double sample = dist(rng);
    const auto found = std::lower_bound(cdf.begin(), cdf.end(), sample);
    keys.push_back(static_cast<int>(std::distance(cdf.begin(), found)));
  }
  return keys;
}

template <template <typename, typename> class Policy>
BenchResult run_pattern(const std::vector<int>& keys, std::size_t capacity) {
  auto cache = CacheFactory<Policy>::make(capacity);
  for (std::size_t i = 0; i < capacity; ++i) {
    cache->put(static_cast<int>(i), static_cast<int>(i));
  }

  const auto start = Clock::now();
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i % 10 == 0) {
      cache->put(keys[i], keys[i]);
    } else {
      (void)cache->get(keys[i]);
    }
  }
  const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  const auto stats = cache->stats();
  return {static_cast<double>(keys.size()) / elapsed, stats.hit_rate};
}

template <template <typename, typename> class Policy>
BenchResult run_threaded(std::size_t operations, std::size_t capacity) {
  auto cache = CacheFactory<Policy>::make(capacity);
  for (std::size_t i = 0; i < capacity; ++i) {
    cache->put(static_cast<int>(i), static_cast<int>(i));
  }

  const auto start = Clock::now();
  std::vector<std::thread> threads;
  for (int thread_id = 0; thread_id < 8; ++thread_id) {
    threads.emplace_back([thread_id, operations, &cache]() {
      std::mt19937 rng(100 + thread_id);
      std::uniform_int_distribution<int> dist(0, 8191);
      for (std::size_t i = 0; i < operations / 8; ++i) {
        const int key = dist(rng);
        if ((i + static_cast<std::size_t>(thread_id)) % 5 == 0) {
          cache->put(key, key);
        } else {
          (void)cache->get(key);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  const auto stats = cache->stats();
  return {static_cast<double>(operations) / elapsed, stats.hit_rate};
}

void print_result(const std::string& policy, const std::string& scenario, BenchResult result) {
  std::cout << std::left << std::setw(8) << policy << std::setw(14) << scenario << std::right
            << std::setw(14) << static_cast<std::uint64_t>(result.ops_per_second)
            << " ops/sec  hit_rate=" << std::fixed << std::setprecision(3) << result.hit_rate
            << '\n';
}

template <template <typename, typename> class Policy>
void run_policy(const std::string& name, const std::vector<int>& sequential,
                const std::vector<int>& random, const std::vector<int>& zipf) {
  constexpr std::size_t capacity = 1024;
  print_result(name, "sequential", run_pattern<Policy>(sequential, capacity));
  print_result(name, "random", run_pattern<Policy>(random, capacity));
  print_result(name, "zipfian", run_pattern<Policy>(zipf, capacity));
  print_result(name, "8-thread", run_threaded<Policy>(sequential.size(), capacity));
}

void benchmark_memoize() {
  constexpr std::size_t operations = 200000;
  volatile int sink = 0;
  auto raw = [](int value) { return (value * 31) ^ 17; };

  const auto raw_start = Clock::now();
  for (std::size_t i = 0; i < operations; ++i) {
    sink += raw(static_cast<int>(i % 1024));
  }
  const auto raw_elapsed = std::chrono::duration<double>(Clock::now() - raw_start).count();

  auto memoized = cpp_cache::make_memoized<cpp_cache::LRUPolicy>(1024, raw);
  const auto memo_start = Clock::now();
  for (std::size_t i = 0; i < operations; ++i) {
    sink += memoized(static_cast<int>(i % 1024));
  }
  const auto memo_elapsed = std::chrono::duration<double>(Clock::now() - memo_start).count();

  std::cout << "\nMemoize overhead\n";
  std::cout << "raw       " << static_cast<std::uint64_t>(operations / raw_elapsed)
            << " ops/sec\n";
  std::cout << "memoized  " << static_cast<std::uint64_t>(operations / memo_elapsed)
            << " ops/sec  hit_rate=" << memoized.stats().hit_rate << '\n';
  (void)sink;
}

void benchmark_get_or_insert_contention() {
  constexpr std::size_t operations = 8000;
  cpp_cache::Cache<int, int, cpp_cache::LRUPolicy> cache(64);
  std::atomic<int> loader_calls{0};

  auto loader = [&loader_calls](const int& key) {
    ++loader_calls;
    return key * key;
  };

  const auto start = Clock::now();
  std::vector<std::thread> threads;
  for (int thread_id = 0; thread_id < 8; ++thread_id) {
    threads.emplace_back([&cache, &loader]() {
      for (int i = 0; i < static_cast<int>(operations / 8); ++i) {
        (void)cache.get_or_insert(i % 16, loader);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();

  std::cout << "\nget_or_insert contention\n";
  std::cout << static_cast<std::uint64_t>(operations / elapsed)
            << " ops/sec  loader_calls=" << loader_calls.load()
            << " hit_rate=" << cache.stats().hit_rate << '\n';
}

}  // namespace

int main() {
  constexpr std::size_t operations = 100000;
  constexpr int key_space = 8192;

  const auto sequential = sequential_keys(operations, key_space);
  const auto random = random_keys(operations, key_space);
  const auto zipf = zipf_keys(operations, key_space);

  std::cout << "Policy  Scenario             Throughput\n";
  run_policy<cpp_cache::LRUPolicy>("LRU", sequential, random, zipf);
  run_policy<cpp_cache::LFUPolicy>("LFU", sequential, random, zipf);
  run_policy<cpp_cache::FIFOPolicy>("FIFO", sequential, random, zipf);
  run_policy<cpp_cache::SLRUPolicy>("SLRU", sequential, random, zipf);
  run_policy<cpp_cache::TTLPolicy>("TTL", sequential, random, zipf);

  benchmark_memoize();
  benchmark_get_or_insert_contention();
}

// history marker: zipf distribution
