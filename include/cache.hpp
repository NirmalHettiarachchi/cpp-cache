#ifndef CPP_CACHE_CACHE_HPP_
#define CPP_CACHE_CACHE_HPP_

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <shared_mutex>

namespace cpp_cache {

/**
 * @brief Runtime counters collected by Cache.
 */
struct CacheStats {
  std::size_t hits = 0;
  std::size_t misses = 0;
  std::size_t evictions = 0;
  std::size_t expired = 0;
  std::size_t inserts = 0;
  double hit_rate = 0.0;
  double eviction_rate = 0.0;
};

namespace detail {

template <typename K, typename V>
struct PolicyGetResult {
  std::optional<V> value;
  std::size_t expired = 0;
};

template <typename K, typename V>
struct PolicyPutResult {
  std::vector<std::pair<K, V>> evicted;
  std::size_t expired = 0;
  bool stored = false;
  bool inserted_new = false;
};

template <typename K, typename V>
struct PolicyResizeResult {
  std::vector<std::pair<K, V>> evicted;
  std::size_t expired = 0;
};

template <typename T>
class function_traits;

template <typename R, typename Arg>
class function_traits<R (*)(Arg)> {
 public:
  using return_type = R;
  using argument_type = Arg;
};

template <typename R, typename Arg>
class function_traits<R(Arg)> {
 public:
  using return_type = R;
  using argument_type = Arg;
};

template <typename R, typename Arg>
class function_traits<std::function<R(Arg)>> {
 public:
  using return_type = R;
  using argument_type = Arg;
};

template <typename C, typename R, typename Arg>
class function_traits<R (C::*)(Arg)> {
 public:
  using return_type = R;
  using argument_type = Arg;
};

template <typename C, typename R, typename Arg>
class function_traits<R (C::*)(Arg) const> {
 public:
  using return_type = R;
  using argument_type = Arg;
};

template <typename Fn>
class function_traits : public function_traits<decltype(&Fn::operator())> {};

}  // namespace detail

/**
 * @brief Least-recently-used eviction policy with O(1) get and put.
 *
 * The front of the list is the most recently used entry. The back of the list
 * is the next entry to evict.
 */
template <typename K, typename V>
class LRUPolicy {
 public:
  static constexpr bool kHasBackgroundExpiry = false;

  /**
   * @brief Construct an LRU policy.
   * @param capacity Maximum number of entries to retain.
   */
  explicit LRUPolicy(std::size_t capacity) : capacity_(capacity) {}

  /**
   * @brief Return the value for key and mark it most recently used.
   */
  detail::PolicyGetResult<K, V> get(const K& key) {
    auto found = index_.find(key);
    if (found == index_.end()) {
      return {};
    }
    items_.splice(items_.begin(), items_, found->second);
    return {found->second->second, 0};
  }

  /**
   * @brief Insert or update key/value and evict the LRU tail if needed.
   */
  detail::PolicyPutResult<K, V> put(const K& key, const V& value) {
    detail::PolicyPutResult<K, V> result;
    auto found = index_.find(key);
    if (found != index_.end()) {
      found->second->second = value;
      items_.splice(items_.begin(), items_, found->second);
      result.stored = true;
      return result;
    }

    if (capacity_ == 0) {
      return result;
    }

    items_.emplace_front(key, value);
    index_.insert_or_assign(key, items_.begin());
    result.stored = true;
    result.inserted_new = true;
    while (items_.size() > capacity_) {
      result.evicted.push_back(remove_tail());
    }
    return result;
  }

  /**
   * @brief Return true when key exists.
   */
  bool contains(const K& key) const { return index_.find(key) != index_.end(); }

  /**
   * @brief Remove key if present.
   */
  bool erase(const K& key) {
    auto found = index_.find(key);
    if (found == index_.end()) {
      return false;
    }
    items_.erase(found->second);
    index_.erase(found);
    return true;
  }

  /**
   * @brief Remove every entry.
   */
  void clear() {
    items_.clear();
    index_.clear();
  }

  /**
   * @brief Return number of stored entries.
   */
  std::size_t size() const { return index_.size(); }

  /**
   * @brief Return maximum number of entries.
   */
  std::size_t capacity() const { return capacity_; }

  /**
   * @brief Change capacity and evict LRU entries until size fits.
   */
  detail::PolicyResizeResult<K, V> resize(std::size_t new_capacity) {
    capacity_ = new_capacity;
    detail::PolicyResizeResult<K, V> result;
    while (items_.size() > capacity_) {
      result.evicted.push_back(remove_tail());
    }
    return result;
  }

  /**
   * @brief Return entries in eviction order, from LRU to MRU.
   */
  std::vector<std::pair<K, V>> snapshot() const {
    std::vector<std::pair<K, V>> result;
    result.reserve(items_.size());
    for (auto iter = items_.rbegin(); iter != items_.rend(); ++iter) {
      result.push_back(*iter);
    }
    return result;
  }

 private:
  using List = std::list<std::pair<K, V>>;
  using Iterator = typename List::iterator;

  std::pair<K, V> remove_tail() {
    auto item = std::move(items_.back());
    index_.erase(item.first);
    items_.pop_back();
    return item;
  }

  std::size_t capacity_;
  List items_;
  std::unordered_map<K, Iterator> index_;
};

/**
 * @brief First-in-first-out eviction policy with O(1) operations.
 *
 * Reads never change eviction order. Updating an existing key preserves its
 * original insertion position.
 */
template <typename K, typename V>
class FIFOPolicy {
 public:
  static constexpr bool kHasBackgroundExpiry = false;

  /**
   * @brief Construct a FIFO policy.
   */
  explicit FIFOPolicy(std::size_t capacity) : capacity_(capacity) {}

  /**
   * @brief Return the value for key without changing eviction order.
   */
  detail::PolicyGetResult<K, V> get(const K& key) {
    auto found = index_.find(key);
    if (found == index_.end()) {
      return {};
    }
    return {found->second.value, 0};
  }

  /**
   * @brief Insert or update key/value and evict the oldest inserted key.
   */
  detail::PolicyPutResult<K, V> put(const K& key, const V& value) {
    detail::PolicyPutResult<K, V> result;
    auto found = index_.find(key);
    if (found != index_.end()) {
      found->second.value = value;
      result.stored = true;
      return result;
    }

    if (capacity_ == 0) {
      return result;
    }

    order_.push_back(key);
    index_.emplace(key, Entry{value, std::prev(order_.end())});
    result.stored = true;
    result.inserted_new = true;
    while (index_.size() > capacity_) {
      result.evicted.push_back(remove_oldest());
    }
    return result;
  }

  /**
   * @brief Return true when key exists.
   */
  bool contains(const K& key) const { return index_.find(key) != index_.end(); }

  /**
   * @brief Remove key if present.
   */
  bool erase(const K& key) {
    auto found = index_.find(key);
    if (found == index_.end()) {
      return false;
    }
    order_.erase(found->second.order);
    index_.erase(found);
    return true;
  }

  /**
   * @brief Remove every entry.
   */
  void clear() {
    order_.clear();
    index_.clear();
  }

  /**
   * @brief Return number of stored entries.
   */
  std::size_t size() const { return index_.size(); }

  /**
   * @brief Return maximum number of entries.
   */
  std::size_t capacity() const { return capacity_; }

  /**
   * @brief Change capacity and evict oldest entries until size fits.
   */
  detail::PolicyResizeResult<K, V> resize(std::size_t new_capacity) {
    capacity_ = new_capacity;
    detail::PolicyResizeResult<K, V> result;
    while (index_.size() > capacity_) {
      result.evicted.push_back(remove_oldest());
    }
    return result;
  }

  /**
   * @brief Return entries in FIFO eviction order.
   */
  std::vector<std::pair<K, V>> snapshot() const {
    std::vector<std::pair<K, V>> result;
    result.reserve(index_.size());
    for (const auto& key : order_) {
      auto found = index_.find(key);
      if (found != index_.end()) {
        result.emplace_back(key, found->second.value);
      }
    }
    return result;
  }

 private:
  struct Entry {
    V value;
    typename std::list<K>::iterator order;
  };

  std::pair<K, V> remove_oldest() {
    const K key = order_.front();
    auto found = index_.find(key);
    std::pair<K, V> item{found->first, found->second.value};
    order_.pop_front();
    index_.erase(found);
    return item;
  }

  std::size_t capacity_;
  std::list<K> order_;
  std::unordered_map<K, Entry> index_;
};

/**
 * @brief True O(1) least-frequently-used policy with LRU tie-breaking.
 *
 * key_map stores value, frequency, and the key's iterator inside its frequency
 * bucket. freq_map stores one recency list per frequency; front is most recent
 * and back is least recent among keys with that frequency.
 */
template <typename K, typename V>
class LFUPolicy {
 public:
  static constexpr bool kHasBackgroundExpiry = false;

  /**
   * @brief Construct an LFU policy.
   */
  explicit LFUPolicy(std::size_t capacity) : capacity_(capacity) {}

  /**
   * @brief Return the value for key and increment its frequency.
   */
  detail::PolicyGetResult<K, V> get(const K& key) {
    auto found = items_.find(key);
    if (found == items_.end()) {
      return {};
    }
    increment_frequency(found);
    return {found->second.value, 0};
  }

  /**
   * @brief Insert or update key/value. Updates also increment frequency.
   */
  detail::PolicyPutResult<K, V> put(const K& key, const V& value) {
    detail::PolicyPutResult<K, V> result;
    auto found = items_.find(key);
    if (found != items_.end()) {
      found->second.value = value;
      increment_frequency(found);
      result.stored = true;
      return result;
    }

    if (capacity_ == 0) {
      return result;
    }

    if (items_.size() >= capacity_) {
      result.evicted.push_back(remove_lfu());
    }
    freq_map_[1].push_front(key);
    items_.emplace(key, Entry{value, 1, freq_map_[1].begin()});
    min_freq_ = 1;
    result.stored = true;
    result.inserted_new = true;
    return result;
  }

  /**
   * @brief Return true when key exists.
   */
  bool contains(const K& key) const { return items_.find(key) != items_.end(); }

  /**
   * @brief Remove key if present.
   */
  bool erase(const K& key) {
    auto found = items_.find(key);
    if (found == items_.end()) {
      return false;
    }
    remove_from_frequency_bucket(found);
    items_.erase(found);
    normalize_min_frequency();
    return true;
  }

  /**
   * @brief Remove every entry.
   */
  void clear() {
    items_.clear();
    freq_map_.clear();
    min_freq_ = 0;
  }

  /**
   * @brief Return number of stored entries.
   */
  std::size_t size() const { return items_.size(); }

  /**
   * @brief Return maximum number of entries.
   */
  std::size_t capacity() const { return capacity_; }

  /**
   * @brief Change capacity and evict LFU/LRU entries until size fits.
   */
  detail::PolicyResizeResult<K, V> resize(std::size_t new_capacity) {
    capacity_ = new_capacity;
    detail::PolicyResizeResult<K, V> result;
    while (items_.size() > capacity_) {
      result.evicted.push_back(remove_lfu());
    }
    if (items_.empty()) {
      min_freq_ = 0;
    }
    return result;
  }

  /**
   * @brief Return entries in eviction order: low frequency first, LRU ties first.
   */
  std::vector<std::pair<K, V>> snapshot() const {
    std::vector<std::size_t> frequencies;
    frequencies.reserve(freq_map_.size());
    for (const auto& bucket : freq_map_) {
      if (!bucket.second.empty()) {
        frequencies.push_back(bucket.first);
      }
    }
    std::sort(frequencies.begin(), frequencies.end());

    std::vector<std::pair<K, V>> result;
    result.reserve(items_.size());
    for (std::size_t frequency : frequencies) {
      auto bucket = freq_map_.find(frequency);
      if (bucket == freq_map_.end()) {
        continue;
      }
      for (auto iter = bucket->second.rbegin(); iter != bucket->second.rend(); ++iter) {
        auto item = items_.find(*iter);
        if (item != items_.end()) {
          result.emplace_back(item->first, item->second.value);
        }
      }
    }
    return result;
  }

 private:
  struct Entry {
    V value;
    std::size_t frequency;
    typename std::list<K>::iterator frequency_iter;
  };

  using MapIterator = typename std::unordered_map<K, Entry>::iterator;

  void increment_frequency(MapIterator item) {
    const std::size_t old_frequency = item->second.frequency;
    auto& old_bucket = freq_map_[old_frequency];
    old_bucket.erase(item->second.frequency_iter);
    if (old_bucket.empty()) {
      freq_map_.erase(old_frequency);
      if (min_freq_ == old_frequency) {
        ++min_freq_;
      }
    }

    const std::size_t new_frequency = old_frequency + 1;
    freq_map_[new_frequency].push_front(item->first);
    item->second.frequency = new_frequency;
    item->second.frequency_iter = freq_map_[new_frequency].begin();
  }

  void remove_from_frequency_bucket(MapIterator item) {
    auto bucket = freq_map_.find(item->second.frequency);
    if (bucket == freq_map_.end()) {
      return;
    }
    bucket->second.erase(item->second.frequency_iter);
    if (bucket->second.empty()) {
      freq_map_.erase(bucket);
    }
  }

  std::pair<K, V> remove_lfu() {
    normalize_min_frequency();
    auto bucket = freq_map_.find(min_freq_);
    if (bucket == freq_map_.end() || bucket->second.empty()) {
      throw std::logic_error("LFU policy has no entry to evict");
    }
    const K key = bucket->second.back();
    bucket->second.pop_back();
    auto found = items_.find(key);
    std::pair<K, V> item{found->first, found->second.value};
    items_.erase(found);
    if (bucket->second.empty()) {
      freq_map_.erase(bucket);
      normalize_min_frequency();
    }
    return item;
  }

  void normalize_min_frequency() {
    if (items_.empty()) {
      min_freq_ = 0;
      return;
    }
    if (min_freq_ != 0) {
      auto found = freq_map_.find(min_freq_);
      if (found != freq_map_.end() && !found->second.empty()) {
        return;
      }
    }
    min_freq_ = items_.begin()->second.frequency;
    for (const auto& item : items_) {
      min_freq_ = std::min(min_freq_, item.second.frequency);
    }
  }

  std::size_t capacity_;
  std::size_t min_freq_ = 0;
  std::unordered_map<K, Entry> items_;
  std::unordered_map<std::size_t, std::list<K>> freq_map_;
};

/**
 * @brief Segmented LRU policy with protected and probationary segments.
 *
 * By default, 70% of capacity is reserved for the protected segment and 30% for
 * the probationary segment. New entries enter probationary. A second access
 * promotes an entry to protected. Sequential scans are contained to the
 * probationary segment and do not displace protected hot entries.
 */
template <typename K, typename V>
class SLRUPolicy {
 public:
  static constexpr bool kHasBackgroundExpiry = false;
  static constexpr double kDefaultProtectedRatio = 0.7;

  /**
   * @brief Construct an SLRU policy.
   * @param capacity Total capacity across both segments.
   * @param protected_ratio Fraction reserved for the protected segment.
   */
  explicit SLRUPolicy(std::size_t capacity, double protected_ratio = kDefaultProtectedRatio)
      : capacity_(capacity), protected_ratio_(clamp_ratio(protected_ratio)) {
    recompute_limits();
  }

  /**
   * @brief Return value and update segment recency/promotion state.
   */
  detail::PolicyGetResult<K, V> get(const K& key) {
    auto found = index_.find(key);
    if (found == index_.end()) {
      return {};
    }
    V value = iterator_value(found->second);
    if (found->second.segment == Segment::kProtected) {
      protected_items_.splice(protected_items_.begin(), protected_items_, found->second.iter);
    } else {
      promote_to_protected(found);
    }
    return {value, 0};
  }

  /**
   * @brief Insert or update key/value using SLRU promotion rules.
   */
  detail::PolicyPutResult<K, V> put(const K& key, const V& value) {
    detail::PolicyPutResult<K, V> result;
    auto found = index_.find(key);
    if (found != index_.end()) {
      iterator_value(found->second) = value;
      if (found->second.segment == Segment::kProtected) {
        protected_items_.splice(protected_items_.begin(), protected_items_, found->second.iter);
      } else {
        promote_to_protected(found, &result.evicted);
      }
      result.stored = true;
      return result;
    }

    if (capacity_ == 0 || probationary_capacity_ == 0) {
      return result;
    }

    probationary_items_.emplace_front(key, value);
    index_.emplace(key, Entry{Segment::kProbationary, probationary_items_.begin()});
    result.stored = true;
    result.inserted_new = true;
    enforce_limits(&result.evicted);
    return result;
  }

  /**
   * @brief Return true when key exists.
   */
  bool contains(const K& key) const { return index_.find(key) != index_.end(); }

  /**
   * @brief Remove key if present.
   */
  bool erase(const K& key) {
    auto found = index_.find(key);
    if (found == index_.end()) {
      return false;
    }
    erase_iterator(found);
    return true;
  }

  /**
   * @brief Remove every entry.
   */
  void clear() {
    protected_items_.clear();
    probationary_items_.clear();
    index_.clear();
  }

  /**
   * @brief Return number of stored entries.
   */
  std::size_t size() const { return index_.size(); }

  /**
   * @brief Return maximum number of entries.
   */
  std::size_t capacity() const { return capacity_; }

  /**
   * @brief Change total capacity and recompute the protected/probationary split.
   */
  detail::PolicyResizeResult<K, V> resize(std::size_t new_capacity) {
    capacity_ = new_capacity;
    recompute_limits();
    detail::PolicyResizeResult<K, V> result;
    enforce_limits(&result.evicted);
    return result;
  }

  /**
   * @brief Return entries in eviction order.
   *
   * Probationary LRU entries appear first. Protected entries follow in the
   * order they would be demoted if the cache had to keep shrinking.
   */
  std::vector<std::pair<K, V>> snapshot() const {
    std::vector<std::pair<K, V>> result;
    result.reserve(index_.size());
    for (auto iter = probationary_items_.rbegin(); iter != probationary_items_.rend(); ++iter) {
      result.push_back(*iter);
    }
    for (auto iter = protected_items_.rbegin(); iter != protected_items_.rend(); ++iter) {
      result.push_back(*iter);
    }
    return result;
  }

 private:
  enum class Segment { kProbationary, kProtected };
  using List = std::list<std::pair<K, V>>;
  using Iterator = typename List::iterator;

  struct Entry {
    Segment segment;
    Iterator iter;
  };

  using IndexIterator = typename std::unordered_map<K, Entry>::iterator;

  static double clamp_ratio(double ratio) {
    if (ratio < 0.0) {
      return 0.0;
    }
    if (ratio > 1.0) {
      return 1.0;
    }
    return ratio;
  }

  V& iterator_value(Entry& entry) {
    return entry.segment == Segment::kProtected ? entry.iter->second : entry.iter->second;
  }

  const V& iterator_value(const Entry& entry) const {
    return entry.segment == Segment::kProtected ? entry.iter->second : entry.iter->second;
  }

  void recompute_limits() {
    if (capacity_ == 0) {
      protected_capacity_ = 0;
      probationary_capacity_ = 0;
      return;
    }
    protected_capacity_ =
        static_cast<std::size_t>(std::round(static_cast<double>(capacity_) * protected_ratio_));
    if (protected_capacity_ >= capacity_) {
      protected_capacity_ = capacity_ > 1 ? capacity_ - 1 : 0;
    }
    probationary_capacity_ = capacity_ - protected_capacity_;
    if (probationary_capacity_ == 0) {
      probationary_capacity_ = 1;
      protected_capacity_ = capacity_ - 1;
    }
  }

  void promote_to_protected(IndexIterator item,
                            std::vector<std::pair<K, V>>* evicted = nullptr) {
    auto node = std::move(*item->second.iter);
    probationary_items_.erase(item->second.iter);
    protected_items_.push_front(std::move(node));
    item->second.segment = Segment::kProtected;
    item->second.iter = protected_items_.begin();
    enforce_limits(evicted);
  }

  void demote_protected_tail() {
    auto node = std::move(protected_items_.back());
    protected_items_.pop_back();
    probationary_items_.push_front(std::move(node));
    auto found = index_.find(probationary_items_.front().first);
    if (found != index_.end()) {
      found->second = Entry{Segment::kProbationary, probationary_items_.begin()};
    }
  }

  std::pair<K, V> evict_probationary_tail() {
    if (probationary_items_.empty() && !protected_items_.empty()) {
      demote_protected_tail();
    }
    auto item = std::move(probationary_items_.back());
    index_.erase(item.first);
    probationary_items_.pop_back();
    return item;
  }

  void enforce_limits(std::vector<std::pair<K, V>>* evicted) {
    while (protected_items_.size() > protected_capacity_) {
      demote_protected_tail();
    }
    while (probationary_items_.size() > probationary_capacity_) {
      auto removed = evict_probationary_tail();
      if (evicted != nullptr) {
        evicted->push_back(std::move(removed));
      }
    }
    while (index_.size() > capacity_) {
      auto removed = evict_probationary_tail();
      if (evicted != nullptr) {
        evicted->push_back(std::move(removed));
      }
    }
  }

  void erase_iterator(IndexIterator item) {
    if (item->second.segment == Segment::kProtected) {
      protected_items_.erase(item->second.iter);
    } else {
      probationary_items_.erase(item->second.iter);
    }
    index_.erase(item);
  }

  std::size_t capacity_;
  double protected_ratio_;
  std::size_t protected_capacity_ = 0;
  std::size_t probationary_capacity_ = 0;
  List protected_items_;
  List probationary_items_;
  std::unordered_map<K, Entry> index_;
};

/**
 * @brief Fixed time-to-live policy backed by a timing wheel.
 *
 * Timing wheel design:
 *
 * - The wheel is a circular array of 64 buckets.
 * - Each bucket represents one time slot, sized as ceil(ttl / 64).
 * - Every insert records an absolute expiry time and places a lightweight
 *   {key, generation} reference into the bucket selected by expiry_time modulo
 *   the wheel size.
 * - The background worker owned by Cache sleeps for one slot duration, advances
 *   the wheel pointer, and prunes only the current bucket. Stale references
 *   caused by overwrites are ignored by comparing generation numbers.
 * - Actual expiry correctness does not depend on the wheel: get() lazily checks
 *   the entry's absolute expiry. The wheel is the low-overhead bulk pruning
 *   mechanism, making background work proportional to the number of references
 *   in the current slot rather than O(log n) priority-queue maintenance.
 */
template <typename K, typename V>
class TTLPolicy {
 public:
  using Clock = std::chrono::steady_clock;
  using Duration = Clock::duration;
  using TimePoint = Clock::time_point;

  static constexpr bool kHasBackgroundExpiry = true;

  /**
   * @brief Construct a TTL policy with a default 60 second TTL.
   */
  explicit TTLPolicy(std::size_t capacity) : TTLPolicy(capacity, std::chrono::seconds(60)) {}

  /**
   * @brief Construct a TTL policy.
   * @param capacity Maximum number of live entries.
   * @param ttl Fixed time-to-live applied on each insertion/update.
   */
  template <typename Rep, typename Period>
  TTLPolicy(std::size_t capacity, std::chrono::duration<Rep, Period> ttl) : capacity_(capacity) {
    ttl_ = std::chrono::duration_cast<Duration>(ttl);
    if (ttl_ <= Duration::zero()) {
      throw std::invalid_argument("TTLPolicy requires a positive TTL");
    }
    tick_ = compute_tick_duration(ttl_);
    last_cleanup_ = Clock::now();
    current_bucket_ = bucket_for(last_cleanup_);
  }

  /**
   * @brief Return the value for key if present and unexpired.
   */
  detail::PolicyGetResult<K, V> get(const K& key) {
    detail::PolicyGetResult<K, V> result;
    auto found = entries_.find(key);
    if (found == entries_.end()) {
      return result;
    }
    const auto now = Clock::now();
    if (is_expired(found->second, now)) {
      remove_entry(found);
      result.expired = 1;
      return result;
    }
    result.value = found->second.value;
    return result;
  }

  /**
   * @brief Insert or update key/value and set expiry to now + ttl.
   */
  detail::PolicyPutResult<K, V> put(const K& key, const V& value) {
    detail::PolicyPutResult<K, V> result;
    const auto now = Clock::now();
    result.expired = purge_expired_scan(now);
    auto found = entries_.find(key);
    if (found != entries_.end()) {
      touch_existing(found, value, now);
      result.stored = true;
      return result;
    }

    if (capacity_ == 0) {
      return result;
    }

    while (entries_.size() >= capacity_) {
      result.evicted.push_back(remove_oldest());
    }

    order_.push_back(key);
    Entry entry{value, now + ttl_, ++generation_, std::prev(order_.end())};
    const std::size_t bucket = bucket_for(entry.expiry);
    wheel_[bucket].push_back(WheelRef{key, entry.generation});
    entries_.emplace(key, std::move(entry));
    result.stored = true;
    result.inserted_new = true;
    return result;
  }

  /**
   * @brief Return true when key exists and has not expired.
   */
  bool contains(const K& key) const {
    auto found = entries_.find(key);
    return found != entries_.end() && !is_expired(found->second, Clock::now());
  }

  /**
   * @brief Remove key if present.
   */
  bool erase(const K& key) {
    auto found = entries_.find(key);
    if (found == entries_.end()) {
      return false;
    }
    remove_entry(found);
    return true;
  }

  /**
   * @brief Remove every entry and clear all wheel buckets.
   */
  void clear() {
    entries_.clear();
    order_.clear();
    for (auto& bucket : wheel_) {
      bucket.clear();
    }
  }

  /**
   * @brief Return number of entries that are still live.
   */
  std::size_t size() const {
    const auto now = Clock::now();
    std::size_t count = 0;
    for (const auto& entry : entries_) {
      if (!is_expired(entry.second, now)) {
        ++count;
      }
    }
    return count;
  }

  /**
   * @brief Return maximum number of live entries.
   */
  std::size_t capacity() const { return capacity_; }

  /**
   * @brief Change capacity and evict oldest live entries until size fits.
   */
  detail::PolicyResizeResult<K, V> resize(std::size_t new_capacity) {
    detail::PolicyResizeResult<K, V> result;
    result.expired = purge_expired_scan(Clock::now());
    capacity_ = new_capacity;
    while (entries_.size() > capacity_) {
      result.evicted.push_back(remove_oldest());
    }
    return result;
  }

  /**
   * @brief Return unexpired entries ordered by soonest expiry first.
   */
  std::vector<std::pair<K, V>> snapshot() const {
    struct SnapshotItem {
      TimePoint expiry;
      K key;
      V value;
    };

    const auto now = Clock::now();
    std::vector<SnapshotItem> items;
    items.reserve(entries_.size());
    for (const auto& entry : entries_) {
      if (!is_expired(entry.second, now)) {
        items.push_back(SnapshotItem{entry.second.expiry, entry.first, entry.second.value});
      }
    }
    std::sort(items.begin(), items.end(), [](const SnapshotItem& left, const SnapshotItem& right) {
      return left.expiry < right.expiry;
    });

    std::vector<std::pair<K, V>> result;
    result.reserve(items.size());
    for (const auto& item : items) {
      result.emplace_back(item.key, item.value);
    }
    return result;
  }

  /**
   * @brief Duration between timing-wheel bucket advances.
   */
  Duration tick_duration() const { return tick_; }

  /**
   * @brief Advance one timing-wheel slot and prune expired entries in that bucket.
   */
  std::size_t cleanup_expired(TimePoint now) {
    std::size_t expired = 0;
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_cleanup_).count();
    const auto tick_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tick_).count();
    std::size_t steps = tick_ns > 0 && elapsed_ns > 0
                            ? static_cast<std::size_t>(elapsed_ns / tick_ns)
                            : 1;
    steps = std::max<std::size_t>(1, std::min<std::size_t>(steps, kWheelSize));

    for (std::size_t i = 0; i < steps; ++i) {
      current_bucket_ = (current_bucket_ + 1) % kWheelSize;
      expired += prune_bucket(current_bucket_, now);
    }
    last_cleanup_ = now;
    return expired;
  }

 private:
  static constexpr std::size_t kWheelSize = 64;

  struct Entry {
    V value;
    TimePoint expiry;
    std::size_t generation;
    typename std::list<K>::iterator order_iter;
  };

  struct WheelRef {
    K key;
    std::size_t generation;
  };

  using MapIterator = typename std::unordered_map<K, Entry>::iterator;

  static Duration compute_tick_duration(Duration ttl) {
    const auto ttl_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(ttl).count();
    const auto tick_ns = std::max<std::int64_t>(1, (ttl_ns + static_cast<std::int64_t>(kWheelSize) -
                                                       1) /
                                                      static_cast<std::int64_t>(kWheelSize));
    auto tick = std::chrono::duration_cast<Duration>(std::chrono::nanoseconds(tick_ns));
    if (tick <= Duration::zero()) {
      tick = Duration(1);
    }
    return tick;
  }

  std::size_t bucket_for(TimePoint time) const {
    const auto tick_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tick_).count();
    const auto time_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    const auto slot = tick_ns > 0 ? time_ns / tick_ns : time_ns;
    return static_cast<std::size_t>(slot % static_cast<std::int64_t>(kWheelSize));
  }

  bool is_expired(const Entry& entry, TimePoint now) const { return now >= entry.expiry; }

  std::size_t prune_bucket(std::size_t bucket_index, TimePoint now) {
    auto refs = std::move(wheel_[bucket_index]);
    wheel_[bucket_index].clear();

    std::size_t expired = 0;
    for (const auto& ref : refs) {
      auto found = entries_.find(ref.key);
      if (found == entries_.end() || found->second.generation != ref.generation) {
        continue;
      }
      if (is_expired(found->second, now)) {
        remove_entry(found);
        ++expired;
      } else {
        const std::size_t bucket = bucket_for(found->second.expiry);
        wheel_[bucket].push_back(WheelRef{ref.key, ref.generation});
      }
    }
    return expired;
  }

  void touch_existing(MapIterator item, const V& value, TimePoint now) {
    order_.erase(item->second.order_iter);
    order_.push_back(item->first);
    item->second.value = value;
    item->second.expiry = now + ttl_;
    item->second.generation = ++generation_;
    item->second.order_iter = std::prev(order_.end());
    const std::size_t bucket = bucket_for(item->second.expiry);
    wheel_[bucket].push_back(WheelRef{item->first, item->second.generation});
  }

  std::pair<K, V> remove_entry(MapIterator item) {
    std::pair<K, V> removed{item->first, item->second.value};
    order_.erase(item->second.order_iter);
    entries_.erase(item);
    return removed;
  }

  std::pair<K, V> remove_oldest() {
    const K key = order_.front();
    auto found = entries_.find(key);
    if (found == entries_.end()) {
      order_.pop_front();
      return remove_oldest();
    }
    return remove_entry(found);
  }

  std::size_t purge_expired_scan(TimePoint now) {
    std::vector<K> expired_keys;
    expired_keys.reserve(entries_.size());
    for (const auto& entry : entries_) {
      if (is_expired(entry.second, now)) {
        expired_keys.push_back(entry.first);
      }
    }
    for (const auto& key : expired_keys) {
      auto found = entries_.find(key);
      if (found != entries_.end()) {
        remove_entry(found);
      }
    }
    return expired_keys.size();
  }

  std::size_t capacity_;
  Duration ttl_;
  Duration tick_;
  std::size_t current_bucket_ = 0;
  TimePoint last_cleanup_;
  std::size_t generation_ = 0;
  std::list<K> order_;
  std::unordered_map<K, Entry> entries_;
  std::array<std::list<WheelRef>, kWheelSize> wheel_;
};

/**
 * @brief Thread-safe cache facade parameterized by key, value, and policy.
 *
 * Policy implementations are intentionally not synchronized. Cache owns the
 * shared_mutex, callback hooks, statistics, and the TTL background worker.
 */
template <typename K, typename V, template <typename, typename> class Policy = LRUPolicy>
class Cache {
 public:
  using key_type = K;
  using value_type = V;
  using policy_type = Policy<K, V>;
  using on_event_callback = std::function<void(const K&, const V&)>;

  /**
   * @brief Construct a cache with a capacity.
   * @param capacity Maximum number of entries.
   */
  explicit Cache(std::size_t capacity) : policy_(capacity) { start_expiry_thread_if_needed(); }

  /**
   * @brief Construct a cache with a capacity and policy-specific argument.
   *
   * Used by TTLPolicy for the TTL duration and by SLRUPolicy for the protected
   * segment ratio.
   */
  template <typename First, typename... Rest,
            typename = std::enable_if_t<
                std::is_constructible<policy_type, std::size_t, First, Rest...>::value>>
  explicit Cache(std::size_t capacity, First&& first, Rest&&... rest)
      : policy_(capacity, std::forward<First>(first), std::forward<Rest>(rest)...) {
    start_expiry_thread_if_needed();
  }

  /**
   * @brief Stop background workers and release resources.
   */
  ~Cache() { stop_expiry_thread_if_needed(); }

  /**
   * @brief Caches are not copyable because they own locks and worker threads.
   */
  Cache(const Cache&) = delete;

  /**
   * @brief Caches are not copy-assignable because they own locks and worker threads.
   */
  Cache& operator=(const Cache&) = delete;

  /**
   * @brief Caches are not movable because worker threads capture this instance.
   */
  Cache(Cache&&) = delete;

  /**
   * @brief Caches are not move-assignable because worker threads capture this instance.
   */
  Cache& operator=(Cache&&) = delete;

  /**
   * @brief Return the value for key or std::nullopt on miss.
   */
  std::optional<V> get(const K& key) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::lock_guard<std::mutex> policy_lock(policy_mutex_);
    auto result = policy_.get(key);
    stats_.expired += result.expired;
    if (result.value.has_value()) {
      ++stats_.hits;
    } else {
      ++stats_.misses;
    }
    return result.value;
  }

  /**
   * @brief Insert or update key/value.
   */
  void put(const K& key, const V& value) {
    std::vector<std::pair<K, V>> evicted;
    on_event_callback on_evict;
    on_event_callback on_insert;
    bool inserted = false;
    {
      std::unique_lock<std::shared_mutex> lock(mutex_);
      auto result = policy_.put(key, value);
      stats_.expired += result.expired;
      if (result.stored) {
        ++stats_.inserts;
        inserted = true;
        on_insert = on_insert_;
      }
      if (!result.evicted.empty()) {
        stats_.evictions += result.evicted.size();
        evicted = std::move(result.evicted);
        on_evict = on_evict_;
      }
    }
    fire_evictions(on_evict, evicted);
    if (inserted && on_insert) {
      on_insert(key, value);
    }
  }

  /**
   * @brief Return true when key exists in the cache.
   */
  bool contains(const K& key) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::lock_guard<std::mutex> policy_lock(policy_mutex_);
    return policy_.contains(key);
  }

  /**
   * @brief Remove key if present.
   * @return true if an entry was removed.
   */
  bool erase(const K& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    return policy_.erase(key);
  }

  /**
   * @brief Remove all entries without resetting statistics.
   */
  void clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    policy_.clear();
  }

  /**
   * @brief Return the number of current entries.
   */
  std::size_t size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::lock_guard<std::mutex> policy_lock(policy_mutex_);
    return policy_.size();
  }

  /**
   * @brief Return the configured capacity.
   */
  std::size_t capacity() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::lock_guard<std::mutex> policy_lock(policy_mutex_);
    return policy_.capacity();
  }

  /**
   * @brief Get an existing value or compute, store, and return it.
   *
   * The loader runs while the cache is exclusively locked. This intentionally
   * prevents a thundering herd: concurrent misses for the same key cannot all
   * run the loader.
   */
  V get_or_insert(const K& key, std::function<V(const K&)> loader) {
    std::vector<std::pair<K, V>> evicted;
    on_event_callback on_evict;
    on_event_callback on_insert;
    std::optional<V> loaded_value;
    bool inserted = false;
    {
      std::unique_lock<std::shared_mutex> lock(mutex_);
      auto access = policy_.get(key);
      stats_.expired += access.expired;
      if (access.value.has_value()) {
        ++stats_.hits;
        return *access.value;
      }

      ++stats_.misses;
      loaded_value.emplace(loader(key));
      auto result = policy_.put(key, *loaded_value);
      stats_.expired += result.expired;
      if (result.stored) {
        ++stats_.inserts;
        inserted = true;
        on_insert = on_insert_;
      }
      if (!result.evicted.empty()) {
        stats_.evictions += result.evicted.size();
        evicted = std::move(result.evicted);
        on_evict = on_evict_;
      }
    }
    fire_evictions(on_evict, evicted);
    if (inserted && on_insert) {
      on_insert(key, *loaded_value);
    }
    return *loaded_value;
  }

  /**
   * @brief Set the callback invoked for capacity-driven evictions.
   */
  void set_on_evict(on_event_callback callback) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    on_evict_ = std::move(callback);
  }

  /**
   * @brief Set the callback invoked after a value is stored.
   */
  void set_on_insert(on_event_callback callback) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    on_insert_ = std::move(callback);
  }

  /**
   * @brief Return a statistics snapshot with rates computed from counters.
   */
  CacheStats stats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::lock_guard<std::mutex> policy_lock(policy_mutex_);
    CacheStats result = stats_;
    const std::size_t lookups = result.hits + result.misses;
    result.hit_rate = lookups == 0 ? 0.0 : static_cast<double>(result.hits) / lookups;
    result.eviction_rate =
        result.inserts == 0 ? 0.0 : static_cast<double>(result.evictions) / result.inserts;
    return result;
  }

  /**
   * @brief Return a copy of entries in policy eviction order.
   */
  std::vector<std::pair<K, V>> snapshot() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::lock_guard<std::mutex> policy_lock(policy_mutex_);
    return policy_.snapshot();
  }

  /**
   * @brief Change capacity at runtime and evict until the cache fits.
   */
  void resize(std::size_t new_capacity) {
    std::vector<std::pair<K, V>> evicted;
    on_event_callback on_evict;
    {
      std::unique_lock<std::shared_mutex> lock(mutex_);
      auto result = policy_.resize(new_capacity);
      stats_.expired += result.expired;
      if (!result.evicted.empty()) {
        stats_.evictions += result.evicted.size();
        evicted = std::move(result.evicted);
        on_evict = on_evict_;
      }
    }
    fire_evictions(on_evict, evicted);
  }

 private:
  static void fire_evictions(const on_event_callback& callback,
                             const std::vector<std::pair<K, V>>& evicted) {
    if (!callback) {
      return;
    }
    for (const auto& item : evicted) {
      callback(item.first, item.second);
    }
  }

  void start_expiry_thread_if_needed() {
    if constexpr (policy_type::kHasBackgroundExpiry) {
      expiry_thread_ = std::thread([this]() { expiry_loop(); });
    }
  }

  void stop_expiry_thread_if_needed() {
    if constexpr (policy_type::kHasBackgroundExpiry) {
      {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        stop_expiry_ = true;
      }
      expiry_cv_.notify_all();
      if (expiry_thread_.joinable()) {
        expiry_thread_.join();
      }
    }
  }

  void expiry_loop() {
    while (true) {
      std::unique_lock<std::shared_mutex> lock(mutex_);
      const auto wait_for = policy_.tick_duration();
      expiry_cv_.wait_for(lock, wait_for, [this]() { return stop_expiry_; });
      if (stop_expiry_) {
        break;
      }
      stats_.expired += policy_.cleanup_expired(std::chrono::steady_clock::now());
    }
  }

  mutable std::shared_mutex mutex_;
  mutable std::mutex policy_mutex_;
  policy_type policy_;
  CacheStats stats_;
  on_event_callback on_evict_;
  on_event_callback on_insert_;
  std::thread expiry_thread_;
  std::condition_variable_any expiry_cv_;
  bool stop_expiry_ = false;
};

/**
 * @brief Callable wrapper returned by make_memoized.
 */
template <template <typename, typename> class Policy, typename Fn>
class MemoizedFunction {
 private:
  using traits = detail::function_traits<typename std::decay<Fn>::type>;

 public:
  using argument_type = typename traits::argument_type;
  using key_type = typename std::decay<argument_type>::type;
  using return_type = typename traits::return_type;
  using value_type = typename std::decay<return_type>::type;
  using cache_type = Cache<key_type, value_type, Policy>;

  static_assert(!std::is_void<return_type>::value, "make_memoized does not support void returns");

  /**
   * @brief Construct a memoized callable around fn.
   */
  template <typename Callable>
  MemoizedFunction(std::size_t capacity, Callable&& fn)
      : cache_(std::make_shared<cache_type>(capacity)), fn_(std::forward<Callable>(fn)) {}

  /**
   * @brief Invoke the function, using the cache on repeated arguments.
   */
  value_type operator()(argument_type arg) {
    key_type key(arg);
    return cache_->get_or_insert(key, [this](const key_type& loaded_key) {
      return static_cast<value_type>(std::invoke(fn_, loaded_key));
    });
  }

  /**
   * @brief Return the backing cache statistics.
   */
  CacheStats stats() const { return cache_->stats(); }

  /**
   * @brief Clear cached values without resetting statistics.
   */
  void clear() { cache_->clear(); }

  /**
   * @brief Return a reference to the backing cache.
   */
  cache_type& cache() { return *cache_; }

  /**
   * @brief Return a const reference to the backing cache.
   */
  const cache_type& cache() const { return *cache_; }

 private:
  std::shared_ptr<cache_type> cache_;
  typename std::decay<Fn>::type fn_;
};

/**
 * @brief Wrap a unary callable in a thread-safe memoizing cache.
 *
 * @tparam Policy Eviction policy template such as LRUPolicy or LFUPolicy.
 * @tparam Fn Unary callable type.
 * @param capacity Backing cache capacity.
 * @param fn Callable to memoize.
 * @return A callable object with operator()(argument_type) and stats().
 */
template <template <typename, typename> class Policy, typename Fn>
auto make_memoized(std::size_t capacity, Fn&& fn) {
  return MemoizedFunction<Policy, typename std::decay<Fn>::type>(capacity, std::forward<Fn>(fn));
}

}  // namespace cpp_cache

#endif  // CPP_CACHE_CACHE_HPP_

// history marker: get_or_insert api
