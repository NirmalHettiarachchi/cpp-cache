# cpp-cache

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Header-Only](https://img.shields.io/badge/header--only-yes-brightgreen)
![License](https://img.shields.io/badge/license-MIT-green)
![Thread-Safe](https://img.shields.io/badge/thread--safe-shared__mutex-orange)
![Dependencies](https://img.shields.io/badge/dependencies-STL%20only-lightgrey)

A production-grade, header-only C++ cache library with swappable eviction policies,
thread-safe APIs, cache-aside loading, memoization, callbacks, snapshots, resizing, and
timing-wheel TTL expiry.

## Features

- Header-only: drop `include/cache.hpp` into any C++17 project.
- Policies: `LRUPolicy`, true O(1) `LFUPolicy`, `FIFOPolicy`, `TTLPolicy`, and `SLRUPolicy`.
- `make_memoized`: memoize any unary callable with the policy you choose.
- `get_or_insert`: cache-aside loader API that prevents thundering-herd recomputation.
- Eviction and insertion callbacks.
- Timing-wheel TTL internals instead of a priority queue.
- `snapshot()` exports key/value pairs in eviction order.
- `resize()` changes capacity at runtime and evicts deterministically.
- Stats include hits, misses, evictions, expired entries, inserts, hit rate, and eviction rate.
- No external runtime dependencies.

## Quick Start

Copy `include/cache.hpp` into your project and include it:

```cpp
#include "cache.hpp"

#include <iostream>
#include <string>

int main() {
  cpp_cache::Cache<std::string, std::string, cpp_cache::LRUPolicy> cache(2);
  cache.put("a", "alpha");
  cache.put("b", "bravo");
  cache.get("a");
  cache.put("c", "charlie");

  std::cout << cache.get("b").value_or("<evicted>") << "\n";
}
```

Compile with any C++17 compiler:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Iinclude examples/basic_lru.cpp -o basic_lru
```

## API Reference

```cpp
template <typename K, typename V,
          template <typename, typename> class Policy = LRUPolicy>
class Cache;
```

Constructors:

```cpp
Cache(size_t capacity);
Cache(size_t capacity, std::chrono::duration ttl);  // TTLPolicy
Cache(size_t capacity, double protected_ratio);     // SLRUPolicy
```

Methods:

```cpp
std::optional<V> get(const K& key);
void put(const K& key, const V& value);
bool contains(const K& key) const;
bool erase(const K& key);
void clear();
size_t size() const;
size_t capacity() const;
V get_or_insert(const K& key, std::function<V(const K&)> loader);
void set_on_evict(std::function<void(const K&, const V&)> cb);
void set_on_insert(std::function<void(const K&, const V&)> cb);
CacheStats stats() const;
std::vector<std::pair<K, V>> snapshot() const;
void resize(size_t new_capacity);
```

Stats:

```cpp
struct CacheStats {
  size_t hits;
  size_t misses;
  size_t evictions;
  size_t expired;
  size_t inserts;
  double hit_rate;
  double eviction_rate;
};
```

## Eviction Strategies

| Policy | Get | Put | Best For | Scan-Resistant? |
|---|---:|---:|---|---|
| `LRUPolicy` | O(1) | O(1) | General-purpose recency caching | No |
| `LFUPolicy` | O(1) | O(1) | Hotspot and skewed workloads | Partially |
| `FIFOPolicy` | O(1) | O(1) | Insertion-order retention | No |
| `TTLPolicy` | O(1) lazy check | O(1) average | Sessions, tokens, expiring records | No |
| `SLRUPolicy` | O(1) | O(1) | Scan-heavy workloads with hot keys | Yes |

## Choosing The Right Policy

```text
Need time-based expiry?
  yes -> TTLPolicy
  no
   |
   v
Skewed/hotspot workload?
  yes -> LFUPolicy
  no
   |
   v
Concerned about scan pollution?
  yes -> SLRUPolicy
  no
   |
   v
Insertion order matters?
  yes -> FIFOPolicy
  no  -> LRUPolicy
```

## make_memoized

`make_memoized` wraps a unary callable and stores results in a backing `Cache`.

```cpp
#include "cache.hpp"

#include <cmath>
#include <iostream>

int main() {
  auto memo_sqrt = cpp_cache::make_memoized<cpp_cache::LRUPolicy>(1000, [](double x) {
    return std::sqrt(x);
  });

  std::cout << memo_sqrt(2.0) << "\n";  // computed
  std::cout << memo_sqrt(2.0) << "\n";  // cache hit
  std::cout << memo_sqrt.stats().hit_rate << "\n";
}
```

The returned object also exposes `stats()`, `clear()`, and `cache()`.

## get_or_insert / Cache-Aside

`get_or_insert` returns an existing value on hit. On miss, it calls the loader under the
cache lock, stores the result, and returns it. Holding the lock during the loader prevents
multiple threads from computing the same missing key at once.

```cpp
cpp_cache::Cache<int, std::string, cpp_cache::LRUPolicy> users(256);

auto load_user = [](const int& id) {
  return "user-" + std::to_string(id);
};

std::string first = users.get_or_insert(42, load_user);   // loads
std::string second = users.get_or_insert(42, load_user);  // hits
```

## Eviction Callbacks

```cpp
cpp_cache::Cache<int, std::string, cpp_cache::LRUPolicy> cache(2);

cache.set_on_evict([](const int& key, const std::string& value) {
  std::cout << "evicted " << key << "=" << value << "\n";
});

cache.set_on_insert([](const int& key, const std::string& value) {
  std::cout << "inserted " << key << "=" << value << "\n";
});

cache.put(1, "one");
cache.put(2, "two");
cache.put(3, "three");
```

## Thread Safety

Each `Cache` owns a `std::shared_mutex`; there is no global state. Mutating operations take
exclusive access. Read-style operations such as `get`, `contains`, `size`, `capacity`, `stats`,
and `snapshot` take shared access. Because LRU, LFU, and SLRU update policy metadata on reads,
`get` also uses a small internal mutex around policy/stat updates to keep concurrent reads
race-free.

`TTLPolicy` caches own a background expiry thread. The thread sleeps on
`condition_variable_any`, advances one timing-wheel bucket per tick, and is joined cleanly in
the cache destructor.

## Benchmark Results

Run locally with:

```sh
cmake -S . -B build -DCPP_CACHE_BUILD_TESTS=OFF
cmake --build build --target cpp_cache_bench --config Release
./build/cpp_cache_bench
```

| Policy | Sequential | Random | Zipfian | 8-Thread | Hit Rate (Zipfian) |
|--------|-----------:|-------:|--------:|---------:|-------------------:|
| LRU | 10,462,658 ops/s | 8,868,078 ops/s | 11,750,605 ops/s | 3,308,771 ops/s | 86.2% |
| LFU | 2,642,831 ops/s | 4,807,276 ops/s | 5,321,639 ops/s | 1,399,458 ops/s | 86.9% |
| FIFO | 10,098,561 ops/s | 8,694,820 ops/s | 10,771,567 ops/s | 3,776,862 ops/s | 80.8% |
| SLRU | 10,387,236 ops/s | 7,530,460 ops/s | 9,162,543 ops/s | 3,214,410 ops/s | 82.8% |
| TTL | 1,619,354 ops/s | 1,677,658 ops/s | 1,677,689 ops/s | 697,002 ops/s | 83.3% |

> Benchmarked on Windows 11, g++ 16.1.0 (MSYS2 UCRT64), -O2, x86-64.
> Access patterns: sequential (0..N), uniform random, Zipfian 80/20 skew,
> 8-thread mixed read/write. Timing via std::chrono::high_resolution_clock.

## Compared To Other C++ Cache Libraries

| Library | Policies | Thread-Safe | Memoize | Loader | Callbacks | C++ Std |
|---|---|---:|---:|---:|---:|---:|
| mohaps/lrucache11 | LRU | No | No | No | No | C++11 |
| nitnelave/lru_cache | LRU | No | No | No | No | C++11 |
| vpetrigo/caches | LRU, LFU, ARC-style variants | No | No | No | Limited | C++11 |
| cpp-cache | LRU, LFU, FIFO, TTL, SLRU | Yes | Yes | Yes | Yes | C++17 |

## Building & Running

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

Build with ThreadSanitizer on supported compilers:

```sh
cmake -S . -B build-tsan -DCPP_CACHE_ENABLE_TSAN=ON
cmake --build build-tsan --config Release
ctest --test-dir build-tsan --output-on-failure
```

Run examples:

```sh
./build/basic_lru
./build/lfu_example
./build/ttl_session
./build/memoize_example
./build/loader_example
```

## Contributing

Contributions should keep the library header-only, STL-only, C++17-compatible, and clean under
`-Wall -Wextra`. Add focused tests for policy semantics, callbacks, TTL expiry, and threaded
access when behavior changes.

## License

MIT. See [LICENSE](LICENSE).
