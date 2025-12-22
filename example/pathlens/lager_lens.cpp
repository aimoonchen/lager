// lager_lens.cpp
// Implementation of lager::lens<Value, Value> scheme (Scheme 2)

#include "lager_lens.h"
#include <zug/compose.hpp>
#include <iostream>
#include <unordered_map>
#include <list>
#include <mutex>
#include <functional>

namespace immer_lens {

// ============================================================
// LRU Cache for path lens
// ============================================================

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

// LRU Cache implementation
template <typename Key, typename Value, typename Hash = std::hash<Key>>
class LRUCache {
public:
    explicit LRUCache(std::size_t capacity) : capacity_(capacity) {}
    
    // Try to get value from cache
    // Returns nullptr if not found
    const Value* get(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        
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
        std::lock_guard<std::mutex> lock(mutex_);
        
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
    
    // Clear the cache
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_map_.clear();
        lru_list_.clear();
    }
    
    // Get current cache size
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
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
        std::lock_guard<std::mutex> lock(mutex_);
        return Stats{hits_, misses_, cache_map_.size(), capacity_};
    }
    
    void record_hit() const {
        std::lock_guard<std::mutex> lock(mutex_);
        ++hits_;
    }
    
    void record_miss() const {
        std::lock_guard<std::mutex> lock(mutex_);
        ++misses_;
    }

private:
    using ListType = std::list<std::pair<Key, Value>>;
    using MapType = std::unordered_map<Key, typename ListType::iterator, Hash>;
    
    std::size_t capacity_;
    ListType lru_list_;
    MapType cache_map_;
    mutable std::mutex mutex_;
    mutable std::size_t hits_ = 0;
    mutable std::size_t misses_ = 0;
};

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
        // Setter
        [key](Value obj, Value value) -> Value {
            if (auto* map = obj.get_if<ValueMap>()) {
                auto new_map = map->set(key, immer::box<Value>{std::move(value)});
                return Value{std::move(new_map)};
            }
#ifdef IMMER_LENS_AUTO_VIVIFICATION
            // Auto-vivification: create new map
            ValueMap new_map;
            new_map = new_map.set(key, immer::box<Value>{std::move(value)});
            return Value{std::move(new_map)};
#else
            // Strict mode: log error and return unchanged
            std::cerr << "[key_lens] Not a map, cannot set key: " << key << "\n";
            return obj;
#endif
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
        // Setter
        [index](Value obj, Value value) -> Value {
            if (auto* vec = obj.get_if<ValueVector>()) {
                if (index < vec->size()) {
                    auto new_vec = vec->update(index, [&](auto&& box) {
                        return immer::box<Value>{std::move(value)};
                    });
                    return Value{std::move(new_vec)};
                } else {
                    // Extend vector to accommodate index
                    auto new_vec = *vec;
                    for (std::size_t i = vec->size(); i <= index; ++i) {
                        new_vec = new_vec.push_back(immer::box<Value>{Value{}});
                    }
                    new_vec = new_vec.update(index, [&](auto&&) {
                        return immer::box<Value>{std::move(value)};
                    });
                    return Value{std::move(new_vec)};
                }
            }
#ifdef IMMER_LENS_AUTO_VIVIFICATION
            // Auto-vivification: create new vector
            ValueVector new_vec;
            for (std::size_t i = 0; i <= index; ++i) {
                new_vec = new_vec.push_back(immer::box<Value>{Value{}});
            }
            new_vec = new_vec.update(index, [&](auto&&) {
                return immer::box<Value>{std::move(value)};
            });
            return Value{std::move(new_vec)};
#else
            // Strict mode: log error and return unchanged
            std::cerr << "[index_lens] Not a vector, cannot set index: " << index << "\n";
            return obj;
#endif
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

// Helper: Convert a PathElement to a lens
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

// Build lens from path without caching (internal helper)
LagerValueLens build_path_lens_uncached(const Path& path)
{
    if (path.empty()) {
        return zug::identity;
    }
    
    // Build the first lens from the first path element
    LagerValueLens result = path_element_to_lens(path[0]);
    
    // Compose with remaining path elements
    for (std::size_t i = 1; i < path.size(); ++i) {
        result = zug::comp(result, path_element_to_lens(path[i]));
    }
    
    return result;
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

} // namespace immer_lens