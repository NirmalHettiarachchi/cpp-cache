#include <cmath>
#include <atomic>
#include <iostream>

#include "cache.hpp"

int slow_fibonacci(int n) {
  if (n < 2) {
    return n;
  }
  return slow_fibonacci(n - 1) + slow_fibonacci(n - 2);
}

int main() {
  std::atomic<int> fibonacci_calls{0};
  auto memo_fib = cpp_cache::make_memoized<cpp_cache::LRUPolicy>(64, [&fibonacci_calls](int n) {
    ++fibonacci_calls;
    return slow_fibonacci(n);
  });

  std::cout << "fib(35)=" << memo_fib(35) << '\n';
  std::cout << "fib(35) again=" << memo_fib(35) << '\n';

  const auto fib_stats = memo_fib.stats();
  std::cout << "fibonacci cache hits=" << fib_stats.hits
            << " misses=" << fib_stats.misses << '\n';
  std::cout << "fibonacci compute_calls=" << fibonacci_calls.load()
            << " total_invocations=2\n";

  std::atomic<int> sqrt_calls{0};
  auto memo_sqrt = cpp_cache::make_memoized<cpp_cache::LFUPolicy>(100, [&sqrt_calls](double value) {
    ++sqrt_calls;
    return std::sqrt(value);
  });

  std::cout << "sqrt(2)=" << memo_sqrt(2.0) << '\n';
  std::cout << "sqrt(2) again=" << memo_sqrt(2.0) << '\n';
  std::cout << "sqrt cache hit_rate=" << memo_sqrt.stats().hit_rate << '\n';
  std::cout << "sqrt compute_calls=" << sqrt_calls.load() << " total_invocations=2\n";
}
