// lager_lens.cpp
// Implementation of lager::lens<Value, Value> scheme (Scheme 2)

#include <lager_ext/lager_lens.h>
#include <lager_ext/path_utils.h>
#include <zug/compose.hpp>
#include <iostream>
#include <unordered_map>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <functional>

namespace lager_ext {

namespace {

// Hash function for Path
struct PathHash {
    std::size_t operator()(const Path& path) const {
        std::size_t hash = 0;
        for (const auto& elem : path) {
            std::size_t elem_hash = std::visit([](const auto& v) -> std::size_t {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    return std::hash<std::string>{}(v);
                } else {
                    return std::hash<std::size_t>{}(v);
                }
            }, elem);
            // Combine hashes using FNV-1a style mixing
            hash ^= elem_hash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

// ============================================================
// Threading Policy for LRUCache
//
// OPTIMIZATION: Allows compile-time selection of thread safety.
// Use SingleThreadPolicy for single-threaded scenarios to avoid lock overhead.
// Use ThreadSafePolicy (default) for multi-threaded environments.
// ============================================================

// Single-threaded policy: No locking overhead
struct SingleThreadPolicy {
    struct SharedLock {
        explicit SharedLock(const SingleThreadPolicy&) noexcept {}
    };
    struct UniqueLock {
        explicit UniqueLock(const SingleThreadPolicy&) noexcept {}
    };

    // Non-atomic counter for single-threaded use
    mutable std::size_t hits_{0};
    mutable std::size_t misses_{0};

    void record_hit() const noexcept { ++hits_; }
    void record_miss() const noexcept { ++misses_; }
    std::size_t get_hits() const noexcept { return hits_; }
    std::size_t get_misses() const noexcept { return misses_; }
    void reset_stats() noexcept { hits_ = 0; misses_ = 0; }
};

// Thread-safe policy: Uses shared_mutex for read-write locking
struct ThreadSafePolicy {
    struct SharedLock {
        std::shared_lock<std::shared_mutex> lock_;
        explicit SharedLock(const ThreadSafePolicy& policy)
            : lock_(policy.rw_mutex_) {}
    };
    struct UniqueLock {
        std::unique_lock<std::shared_mutex> lock_;
        explicit UniqueLock(const ThreadSafePolicy& policy)
            : lock_(policy.rw_mutex_) {}
    };

    mutable std::shared_mutex rw_mutex_;
    mutable std::atomic<std::size_t> hits_{0};
    mutable std::atomic<std::size_t> misses_{0};

    void record_hit() const noexcept {
        hits_.fetch_add(1, std::memory_order_relaxed);
    }
    void record_miss() const noexcept {
        misses_.fetch_add(1, std::memory_order_relaxed);
    }
    std::size_t get_hits() const noexcept {
        return hits_.load(std::memory_order_relaxed);
    }
    std::size_t get_misses() const noexcept {
        return misses_.load(std::memory_order_relaxed);
    }
    void reset_stats() noexcept {
        hits_.store(0, std::memory_order_relaxed);
        misses_.store(0, std::memory_order_relaxed);
    }
};

// ============================================================
// LRU Cache for lens objects
//
// Optimizations applied:
// 1. Configurable threading policy (single-thread vs thread-safe)
// 2. Uses std::shared_mutex for reader-writer lock pattern (thread-safe mode)
// 3. Read operations (get) use shared locks for maximum concurrency
// 4. Statistics use atomic counters (lock-free) in thread-safe mode
// 5. LRU updates are deferred during reads to minimize lock upgrades
// ============================================================
template <typename Key, typename Value,
          typename Hash = std::hash<Key>,
          typename ThreadingPolicy = ThreadSafePolicy>
class LRUCache : private ThreadingPolicy {
public:
    using SharedLock = typename ThreadingPolicy::SharedLock;
    using UniqueLock = typename ThreadingPolicy::UniqueLock;

    explicit LRUCache(std::size_t capacity) : capacity_(capacity) {}

    // Try to get value from cache
    // OPTIMIZED: Uses shared lock for read, defers LRU update
    // Returns nullptr if not found
    const Value* get(const Key& key) {
        // First try with shared lock (read-only)
        {
            SharedLock read_lock(*this);

            auto it = cache_map_.find(key);
            if (it == cache_map_.end()) {
                return nullptr;
            }

            // Found! Return the value pointer
            // Note: We defer LRU update to avoid lock upgrade
            // The LRU order will be updated on next write operation
            return &it->second->second;
        }
    }

    // Try to get value from cache with LRU update
    // Uses exclusive lock, updates LRU order
    // Use this when LRU accuracy is critical
    const Value* get_with_lru_update(const Key& key) {
        UniqueLock write_lock(*this);

        auto it = cache_map_.find(key);
        if (it == cache_map_.end()) {
            return nullptr;
        }

        // Move to front (most recently used)
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return &it->second->second;
    }

    // Insert or update value in cache
    void put(const Key& key, Value value) {
        UniqueLock write_lock(*this);

        auto it = cache_map_.find(key);
        if (it != cache_map_.end()) {
            // Update existing entry and move to front
            it->second->second = std::move(value);
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            return;
        }

        // Evict if at capacity
        if (cache_map_.size() >= capacity_) {
            // Remove least recently used (back of list)
            auto& lru = lru_list_.back();
            cache_map_.erase(lru.first);
            lru_list_.pop_back();
        }

        // Insert new entry at front
        lru_list_.emplace_front(key, std::move(value));
        cache_map_[key] = lru_list_.begin();
    }

    // Get or create: atomic get-or-put operation
    // If key exists, returns existing value
    // If key doesn't exist, creates new value using factory and caches it
    template<typename Factory>
    Value get_or_create(const Key& key, Factory&& factory) {
        // First try read-only lookup
        {
            SharedLock read_lock(*this);
            auto it = cache_map_.find(key);
            if (it != cache_map_.end()) {
                return it->second->second;  // Return copy
            }
        }

        // Not found, need to create and insert
        UniqueLock write_lock(*this);

        // Double-check after acquiring write lock
        auto it = cache_map_.find(key);
        if (it != cache_map_.end()) {
            // Someone else inserted while we were waiting
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            return it->second->second;
        }

        // Create new value
        Value new_value = factory();

        // Evict if at capacity
        if (cache_map_.size() >= capacity_) {
            auto& lru = lru_list_.back();
            cache_map_.erase(lru.first);
            lru_list_.pop_back();
        }

        // Insert new entry at front
        lru_list_.emplace_front(key, new_value);
        cache_map_[key] = lru_list_.begin();

        return new_value;
    }

    // Clear the cache
    void clear() {
        UniqueLock write_lock(*this);
        cache_map_.clear();
        lru_list_.clear();
        this->reset_stats();
    }

    // Get current cache size
    std::size_t size() const {
        SharedLock read_lock(*this);
        return cache_map_.size();
    }

    // Get cache statistics
    struct Stats {
        std::size_t hits = 0;
        std::size_t misses = 0;
        std::size_t size = 0;
        std::size_t capacity = 0;

        double hit_rate() const {
            auto total = hits + misses;
            return total > 0 ? static_cast<double>(hits) / total : 0.0;
        }
    };

    Stats stats() const {
        SharedLock read_lock(*this);
        return Stats{
            this->get_hits(),
            this->get_misses(),
            cache_map_.size(),
            capacity_
        };
    }

    // Expose statistics recording from policy
    using ThreadingPolicy::record_hit;
    using ThreadingPolicy::record_miss;

private:
    using ListType = std::list<std::pair<Key, Value>>;
    using MapType = std::unordered_map<Key, typename ListType::iterator, Hash>;

    std::size_t capacity_;
    ListType lru_list_;
    MapType cache_map_;
};

// Type aliases for common use cases
template <typename Key, typename Value, typename Hash = std::hash<Key>>
using ThreadSafeLRUCache = LRUCache<Key, Value, Hash, ThreadSafePolicy>;

template <typename Key, typename Value, typename Hash = std::hash<Key>>
using SingleThreadLRUCache = LRUCache<Key, Value, Hash, SingleThreadPolicy>;

// Global lens cache with default capacity of 256
LRUCache<Path, LagerValueLens, PathHash>& get_lens_cache() {
    static LRUCache<Path, LagerValueLens, PathHash> cache(256);
    return cache;
}

} // anonymous namespace

// ============================================================
// Basic getset lens implementations
// ============================================================

auto key_lens(const std::string& key)
{
    return lager::lenses::getset(
        // Getter
        [key](const Value& obj) -> Value {
            if (auto* map = obj.get_if<ValueMap>()) {
                if (auto found = map->find(key); found != nullptr) {
                    return **found;
                }
            }
            return Value{};
        },
        // Setter (strict mode)
        // Note: For auto-vivification, use set_at_path_vivify() from path_utils.h
        [key](Value obj, Value value) -> Value {
            if (auto* map = obj.get_if<ValueMap>()) {
                auto new_map = map->set(key, immer::box<Value>{std::move(value)});
                return Value{std::move(new_map)};
            }
            // Strict mode: log error and return unchanged
            std::cerr << "[key_lens] Not a map, cannot set key: " << key << "\n";
            return obj;
        });
}

auto index_lens(std::size_t index)
{
    return lager::lenses::getset(
        // Getter
        [index](const Value& obj) -> Value {
            if (auto* vec = obj.get_if<ValueVector>()) {
                if (index < vec->size()) {
                    return *(*vec)[index];
                }
            }
            return Value{};
        },
        // Setter (strict mode)
        // Note: For auto-vivification, use set_at_path_vivify() from path_utils.h
        [index](Value obj, Value value) -> Value {
            if (auto* vec = obj.get_if<ValueVector>()) {
                if (index < vec->size()) {
                    auto new_vec = vec->update(index, [&](auto&& box) {
                        return immer::box<Value>{std::move(value)};
                    });
                    return Value{std::move(new_vec)};
                }
            }
            // Strict mode: log error and return unchanged
            std::cerr << "[index_lens] Not a vector or index out of range: " << index << "\n";
            return obj;
        });
}

// ============================================================
// Type-erased lens wrappers
// ============================================================

LagerValueLens lager_key_lens(const std::string& key)
{
    return key_lens(key);
}

LagerValueLens lager_index_lens(std::size_t index)
{
    return index_lens(index);
}

// Helper: Convert a PathElement to a lens (kept for backward compatibility)
namespace {
inline LagerValueLens path_element_to_lens(const PathElement& elem)
{
    return std::visit(
        [](const auto& value) -> LagerValueLens {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::string>) {
                return lager_key_lens(value);
            } else {
                return lager_index_lens(value);
            }
        },
        elem);
}

// ============================================================
// Optimized Path Lens Implementation
//
// Uses shared path_utils.h for direct traversal functions,
// avoiding code duplication with erased_lens.cpp
// ============================================================

// Build lens from path without caching (internal helper)
// OPTIMIZED: Uses path_utils.h for direct path traversal
LagerValueLens build_path_lens_uncached(const Path& path)
{
    if (path.empty()) {
        return zug::identity;
    }

    // Single lens optimization: capture path once, traverse directly
    // Uses path_utils.h functions for efficient path access
    return lager::lenses::getset(
        // Getter: Single-pass traversal using path_utils
        [path](const Value& root) -> Value {
            return get_at_path_direct(root, path);
        },
        // Setter: Recursive rebuild using path_utils
        [path](Value root, Value new_val) -> Value {
            return set_at_path_direct(root, path, std::move(new_val));
        }
    );
}
} // namespace

// Build lens from path using lager::lens<Value, Value>
// Uses LRU cache for frequently accessed paths
//
// Note: We use zug::comp() instead of operator| because:
// - operator| is defined in the zug namespace as a free function
// - ADL (Argument-Dependent Lookup) may not find it when both operands
//   are lager::lens<> (which lives in the lager namespace)
// - Using zug::comp() explicitly avoids this lookup issue
LagerValueLens lager_path_lens(const Path& path)
{
    auto& cache = get_lens_cache();

    // Try cache first
    if (const auto* cached = cache.get(path)) {
        cache.record_hit();
        return *cached;
    }

    // Build lens and cache it
    cache.record_miss();
    LagerValueLens lens = build_path_lens_uncached(path);
    cache.put(path, lens);

    return lens;
}

// ============================================================
// Cache management functions
// ============================================================

void clear_lens_cache()
{
    get_lens_cache().clear();
}

LensCacheStats get_lens_cache_stats()
{
    auto stats = get_lens_cache().stats();
    return LensCacheStats{
        stats.hits,
        stats.misses,
        stats.size,
        stats.capacity,
        stats.hit_rate()
    };
}

// ============================================================
// Demo function
// ============================================================

void demo_lager_lens()
{
    std::cout << "\n=== Scheme 2: lager::lens<Value, Value> Demo ===\n\n";

    // Use common test data
    Value data = create_sample_data();

    std::cout << "Data structure:\n";
    print_value(data, "", 1);

    // Test lager_path_lens with lager::view
    std::cout << "\n--- Test 1: GET using lager::view ---\n";
    Path name_path = {std::string{"users"}, size_t{0}, std::string{"name"}};
    auto lens = lager_path_lens(name_path);

    std::cout << "Path: " << path_to_string(name_path) << "\n";
    std::cout << "lager::view(lens, data) = " << value_to_string(lager::view(lens, data)) << "\n";

    // Test lager::set
    std::cout << "\n--- Test 2: SET using lager::set ---\n";
    Value updated = lager::set(lens, data, Value{std::string{"Alicia"}});
    std::cout << "After lager::set(lens, data, \"Alicia\"):\n";
    std::cout << "New value: " << value_to_string(lager::view(lens, updated)) << "\n";

    // Test lager::over
    std::cout << "\n--- Test 3: OVER using lager::over ---\n";
    Path age_path = {std::string{"users"}, size_t{1}, std::string{"age"}};
    auto age_lens = lager_path_lens(age_path);

    std::cout << "Original age: " << value_to_string(lager::view(age_lens, data)) << "\n";
    Value incremented = lager::over(age_lens, data, [](Value v) {
        if (auto* n = v.get_if<int>()) {
            return Value{*n + 5};
        }
        return v;
    });
    std::cout << "After lager::over +5: " << value_to_string(lager::view(age_lens, incremented)) << "\n";

    // Test composition
    std::cout << "\n--- Test 4: Composition with zug::comp ---\n";
    LagerValueLens config_version = zug::comp(lager_key_lens("config"), lager_key_lens("version"));
    std::cout << "config.version = " << value_to_string(lager::view(config_version, data)) << "\n";

    // Compare with static_path_lens (compile-time known path)
    std::cout << "\n--- Test 5: static_path_lens (compile-time) ---\n";
    auto static_lens = static_path_lens("users", 0, "name");
    std::cout << "static_path_lens(\"users\", 0, \"name\") = "
              << value_to_string(lager::view(static_lens, data)) << "\n";

    // Test cache (access same path multiple times)
    std::cout << "\n--- Test 6: Lens Cache Demo ---\n";
    clear_lens_cache();

    for (int i = 0; i < 5; ++i) {
        auto lens_again = lager_path_lens(name_path);
        lager::view(lens_again, data);
    }

    auto cache_stats = get_lens_cache_stats();
    std::cout << "Cache stats after 5 accesses to same path:\n";
    std::cout << "  Hits: " << cache_stats.hits << "\n";
    std::cout << "  Misses: " << cache_stats.misses << "\n";
    std::cout << "  Hit rate: " << (cache_stats.hit_rate * 100.0) << "%\n";
    std::cout << "  Cache size: " << cache_stats.size << "/" << cache_stats.capacity << "\n";

    std::cout << "\n=== Demo End ===\n\n";
}

// ============================================================
// Structured Error Reporting Implementation
// ============================================================

namespace {

/// Get error message for a path error code
std::string get_error_message(PathErrorCode code, const PathElement& elem, std::size_t index)
{
    auto elem_str = std::visit([](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return "key \"" + v + "\"";
        } else {
            return "index " + std::to_string(v);
        }
    }, elem);

    switch (code) {
        case PathErrorCode::Success:
            return "Success";
        case PathErrorCode::KeyNotFound:
            return "Key not found: " + elem_str + " at path position " + std::to_string(index);
        case PathErrorCode::IndexOutOfRange:
            return "Index out of range: " + elem_str + " at path position " + std::to_string(index);
        case PathErrorCode::TypeMismatch:
            return "Type mismatch: expected container at " + elem_str + " (path position " + std::to_string(index) + ")";
        case PathErrorCode::NullValue:
            return "Null value encountered at " + elem_str + " (path position " + std::to_string(index) + ")";
        case PathErrorCode::EmptyPath:
            return "Empty path";
        default:
            return "Unknown error";
    }
}

/// Try to access a single path element, returns result with error info
std::pair<Value, PathErrorCode> try_get_element(const Value& current, const PathElement& elem)
{
    return std::visit([&current](const auto& key) -> std::pair<Value, PathErrorCode> {
        using T = std::decay_t<decltype(key)>;

        if (current.is_null()) {
            return {Value{}, PathErrorCode::NullValue};
        }

        if constexpr (std::is_same_v<T, std::string>) {
            if (auto* map = current.get_if<ValueMap>()) {
                if (auto found = map->find(key); found != nullptr) {
                    return {**found, PathErrorCode::Success};
                }
                return {Value{}, PathErrorCode::KeyNotFound};
            }
            return {Value{}, PathErrorCode::TypeMismatch};
        } else {
            if (auto* vec = current.get_if<ValueVector>()) {
                if (key < vec->size()) {
                    return {*(*vec)[key], PathErrorCode::Success};
                }
                return {Value{}, PathErrorCode::IndexOutOfRange};
            }
            return {Value{}, PathErrorCode::TypeMismatch};
        }
    }, elem);
}

} // anonymous namespace

PathAccessResult get_at_path_safe(const Value& root, const Path& path)
{
    PathAccessResult result;
    result.value = root;

    if (path.empty()) {
        result.success = true;
        result.error_code = PathErrorCode::EmptyPath;
        result.error_message = "Empty path (returns root)";
        return result;
    }

    Value current = root;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const auto& elem = path[i];
        auto [next_val, error_code] = try_get_element(current, elem);

        if (error_code != PathErrorCode::Success) {
            result.success = false;
            result.error_code = error_code;
            result.error_message = get_error_message(error_code, elem, i);
            result.failed_at_index = i;
            result.value = Value{};  // Null on error
            return result;
        }

        result.resolved_path.push_back(elem);
        current = std::move(next_val);
    }

    result.success = true;
    result.error_code = PathErrorCode::Success;
    result.value = std::move(current);
    return result;
}

PathAccessResult set_at_path_safe(const Value& root, const Path& path, Value new_val)
{
    PathAccessResult result;

    if (path.empty()) {
        result.success = true;
        result.error_code = PathErrorCode::EmptyPath;
        result.error_message = "Empty path (replaces root)";
        result.value = std::move(new_val);
        return result;
    }

    // First validate the path exists (except the last element which will be set)
    Value current = root;
    Path parent_path;

    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const auto& elem = path[i];
        auto [next_val, error_code] = try_get_element(current, elem);

        if (error_code != PathErrorCode::Success) {
            result.success = false;
            result.error_code = error_code;
            result.error_message = get_error_message(error_code, elem, i);
            result.failed_at_index = i;
            result.value = root;  // Return original on error
            return result;
        }

        result.resolved_path.push_back(elem);
        parent_path.push_back(elem);
        current = std::move(next_val);
    }

    // Check if the parent can accept the set operation
    const auto& last_elem = path.back();
    bool can_set = std::visit([&current](const auto& key) -> bool {
        using T = std::decay_t<decltype(key)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return current.is_null() || current.get_if<ValueMap>() != nullptr;
        } else {
            return current.get_if<ValueVector>() != nullptr;
        }
    }, last_elem);

    if (!can_set) {
        result.success = false;
        result.error_code = PathErrorCode::TypeMismatch;
        result.error_message = get_error_message(PathErrorCode::TypeMismatch, last_elem, path.size() - 1);
        result.failed_at_index = path.size() - 1;
        result.value = root;
        return result;
    }

    // All validations passed, perform the set using path_utils
    result.success = true;
    result.error_code = PathErrorCode::Success;
    result.resolved_path = path;
    result.value = set_at_path_direct(root, path, std::move(new_val));
    return result;
}

} // namespace lager_ext
