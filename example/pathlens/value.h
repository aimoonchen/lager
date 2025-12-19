// value.h
// Common Value type definition and utilities for JSON-like dynamic data
//
// This file defines the core Value type that can represent:
// - Primitive types: int, float, double, bool, string
// - Container types: map, vector, array, table (using immer's immutable containers)
// - Null (std::monostate)
//
// The Value type is templated on a memory policy, allowing users to
// customize memory allocation strategies for the underlying immer containers.

#pragma once

#include <immer/array.hpp>
#include <immer/array_transient.hpp>
#include <immer/box.hpp>
#include <immer/map.hpp>
#include <immer/memory_policy.hpp>
#include <immer/table.hpp>
#include <immer/vector.hpp>

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// ============================================================
// Auto-vivification Mode Configuration
// 
// When IMMER_LENS_AUTO_VIVIFICATION is defined (default):
//   - set() operations on type-mismatched values will create new containers
//   - Allows building data structures from empty/null values
//   - Example: setting key on null creates a new map
//
// When IMMER_LENS_AUTO_VIVIFICATION is NOT defined:
//   - set() operations on type-mismatched values return the original value
//   - Errors are logged to stderr
//   - Safer for strict data validation scenarios
//
// To disable auto-vivification, define before including this header:
//   #define IMMER_LENS_NO_AUTO_VIVIFICATION
// ============================================================

#ifndef IMMER_LENS_NO_AUTO_VIVIFICATION
#define IMMER_LENS_AUTO_VIVIFICATION 1
#endif

// ============================================================
// Verbose Logging Configuration
// 
// When IMMER_LENS_VERBOSE_LOG is defined:
//   - at() and set() operations log errors to stderr
//   - Useful for debugging path access issues
//
// By default, verbose logging is DISABLED in release builds
// and ENABLED in debug builds.
//
// To explicitly enable: #define IMMER_LENS_VERBOSE_LOG 1
// To explicitly disable: #define IMMER_LENS_VERBOSE_LOG 0
// ============================================================

#ifndef IMMER_LENS_VERBOSE_LOG
#  if defined(NDEBUG)
#    define IMMER_LENS_VERBOSE_LOG 0
#  else
#    define IMMER_LENS_VERBOSE_LOG 1
#  endif
#endif

namespace immer_lens {

// ============================================================
// Memory Policy Support
// 
// The BasicValue template is parameterized by an immer memory policy,
// allowing customization of:
// - Heap allocation strategy (default: free_list_heap)
// - Reference counting policy (default: thread-safe refcount)
// - Lock policy (default: spinlock)
// - Transience policy (for transient operations)
//
// Common memory policies from immer:
// - immer::default_memory_policy (default, thread-safe with free list)
// - immer::memory_policy<immer::heap_policy<immer::cpp_heap>, 
//                        immer::refcount_policy, 
//                        immer::spinlock_policy>
// - immer::memory_policy<immer::heap_policy<immer::gc_heap>, 
//                        immer::no_refcount_policy, 
//                        immer::no_lock_policy>  (for Boehm GC)
//
// Usage:
//   using MyPolicy = immer::memory_policy<...>;
//   using MyValue = immer_lens::BasicValue<MyPolicy>;
//   using Value = immer_lens::Value;  // Same as BasicValue<default_memory_policy>
// ============================================================

// Forward declaration
template <typename MemoryPolicy>
struct BasicValue;

// ============================================================
// Container type aliases (templated on memory policy)
// ============================================================

template <typename MemoryPolicy>
using BasicValueBox = immer::box<BasicValue<MemoryPolicy>, MemoryPolicy>;

template <typename MemoryPolicy>
using BasicValueMap = immer::map<std::string, 
                                  BasicValueBox<MemoryPolicy>,
                                  std::hash<std::string>,
                                  std::equal_to<std::string>,
                                  MemoryPolicy>;

template <typename MemoryPolicy>
using BasicValueVector = immer::vector<BasicValueBox<MemoryPolicy>,
                                        MemoryPolicy>;

template <typename MemoryPolicy>
using BasicValueArray = immer::array<BasicValueBox<MemoryPolicy>,
                                      MemoryPolicy>;

// ============================================================
// BasicTableEntry - Entry type for ValueTable (templated)
// 
// Each entry has a unique string id and an associated value.
// The id is used as the key for table lookup operations.
// ============================================================
template <typename MemoryPolicy>
struct BasicTableEntry {
    std::string id;
    BasicValueBox<MemoryPolicy> value;
    
    // Required by immer::table for key extraction
    // immer's default table_key_fn looks for .id member
    
    // Required by immer::table for equality comparison
    bool operator==(const BasicTableEntry& other) const {
        return id == other.id && value == other.value;
    }
    
    bool operator!=(const BasicTableEntry& other) const {
        return !(*this == other);
    }
};

template <typename MemoryPolicy>
using BasicValueTable = immer::table<BasicTableEntry<MemoryPolicy>,
                                      immer::table_key_fn,
                                      std::hash<std::string>,
                                      std::equal_to<std::string>,
                                      MemoryPolicy>;

// Path element: either a string key or numeric index
using PathElement = std::variant<std::string, std::size_t>;
using Path        = std::vector<PathElement>;

// ============================================================
// BasicValue struct - JSON-like dynamic data type (templated)
// 
// Supports:
// - Primitive types: int, float, double, bool, string
// - Container types: value_map, value_vector, value_array, value_table
// - Null: std::monostate
// 
// The struct also implements a container-like interface
// for compatibility with lager::lenses::at
// ============================================================
template <typename MemoryPolicy = immer::default_memory_policy>
struct BasicValue
{
    // Memory policy type
    using memory_policy = MemoryPolicy;
    
    // Container type aliases for this memory policy
    using value_box     = BasicValueBox<MemoryPolicy>;
    using value_map     = BasicValueMap<MemoryPolicy>;
    using value_vector  = BasicValueVector<MemoryPolicy>;
    using value_array   = BasicValueArray<MemoryPolicy>;
    using value_table   = BasicValueTable<MemoryPolicy>;
    using table_entry   = BasicTableEntry<MemoryPolicy>;

    std::variant<int,
                 int64_t,
                 float,
                 double,
                 bool,
                 std::string,
                 value_map,
                 value_vector,
                 value_array,
                 value_table,
                 std::monostate>
        data;

    // Constructors
    BasicValue() : data(std::monostate{}) {}
    BasicValue(int v) : data(v) {}
    BasicValue(int64_t v) : data(v) {}
    BasicValue(float v) : data(v) {}
    BasicValue(double v) : data(v) {}
    BasicValue(bool v) : data(v) {}
    BasicValue(const std::string& v) : data(v) {}
    BasicValue(std::string&& v) : data(std::move(v)) {}
    BasicValue(const char* v) : data(std::string(v)) {}
    BasicValue(value_map v) : data(std::move(v)) {}
    BasicValue(value_vector v) : data(std::move(v)) {}
    BasicValue(value_array v) : data(std::move(v)) {}
    BasicValue(value_table v) : data(std::move(v)) {}

    // Type inspection
    template <typename T>
    const T* get_if() const
    {
        return std::get_if<T>(&data);
    }

    template <typename T>
    bool is() const
    {
        return std::holds_alternative<T>(data);
    }
    
    std::size_t type_index() const noexcept { return data.index(); }
    bool is_null() const noexcept { return std::holds_alternative<std::monostate>(data); }
    
    // ============================================================
    // Container-like interface for lager::lenses::at compatibility
    // 
    // Design choice: No exceptions thrown. Instead:
    // - at() returns null Value if key/index not found or type mismatch
    // - set() returns self unchanged if type mismatch
    // - Errors are logged to stderr for debugging
    // ============================================================
    
    // Access by string key (for map-like access)
    BasicValue at(const std::string& key) const
    {
        // Try map
        if (auto* m = get_if<value_map>()) {
            if (auto* found = m->find(key)) {
                return found->get();
            }
        }
        // Try table
        if (auto* t = get_if<value_table>()) {
            if (auto* found = t->find(key)) {
                return found->value.get();
            }
        }
#if IMMER_LENS_VERBOSE_LOG
        std::cerr << "[Value::at] key '" << key << "' not found or type mismatch\n";
#endif
        return BasicValue{};
    }
    
    // Access by index (for vector-like access)
    BasicValue at(std::size_t index) const
    {
        // Try vector
        if (auto* v = get_if<value_vector>()) {
            if (index < v->size()) {
                return (*v)[index].get();
            }
        }
        // Try array
        if (auto* a = get_if<value_array>()) {
            if (index < a->size()) {
                return (*a)[index].get();
            }
        }
#if IMMER_LENS_VERBOSE_LOG
        std::cerr << "[Value::at] index " << index << " out of range or type mismatch\n";
#endif
        return BasicValue{};
    }
    
    // ============================================================
    // Optional access methods - distinguish "not found" from "value is null"
    // ============================================================
    
    // Try to access by string key, returns std::nullopt if not found
    std::optional<BasicValue> try_at(const std::string& key) const
    {
        // Try map
        if (auto* m = get_if<value_map>()) {
            if (auto* found = m->find(key)) {
                return found->get();
            }
        }
        // Try table
        if (auto* t = get_if<value_table>()) {
            if (auto* found = t->find(key)) {
                return found->value.get();
            }
        }
        return std::nullopt;
    }
    
    // Try to access by index, returns std::nullopt if out of range
    std::optional<BasicValue> try_at(std::size_t index) const
    {
        // Try vector
        if (auto* v = get_if<value_vector>()) {
            if (index < v->size()) {
                return (*v)[index].get();
            }
        }
        // Try array
        if (auto* a = get_if<value_array>()) {
            if (index < a->size()) {
                return (*a)[index].get();
            }
        }
        return std::nullopt;
    }
    
    // Check if key/index exists
    bool contains(const std::string& key) const
    {
        return count(key) > 0;
    }
    
    bool contains(std::size_t index) const
    {
        if (auto* v = get_if<value_vector>()) {
            return index < v->size();
        }
        if (auto* a = get_if<value_array>()) {
            return index < a->size();
        }
        return false;
    }
    
    // Immutable set by string key
    BasicValue set(const std::string& key, BasicValue val) const
    {
        // Try map
        if (auto* m = get_if<value_map>()) {
            return m->set(key, value_box{std::move(val)});
        }
        // Try table
        if (auto* t = get_if<value_table>()) {
            return t->insert(table_entry{key, value_box{std::move(val)}});
        }
        
#if IMMER_LENS_AUTO_VIVIFICATION
        // Auto-create map for null or type mismatch
        if (is_null()) {
            return value_map{}.set(key, value_box{std::move(val)});
        }
#endif

#if IMMER_LENS_VERBOSE_LOG
        std::cerr << "[Value::set] cannot set key '" << key << "' on non-map type\n";
#endif
        return *this;
    }
    
    // Immutable set by index
    BasicValue set(std::size_t index, BasicValue val) const
    {
        // Try vector
        if (auto* v = get_if<value_vector>()) {
            if (index < v->size()) {
                return v->set(index, value_box{std::move(val)});
            }
#if IMMER_LENS_AUTO_VIVIFICATION
            // Extend vector if index is at the end
            if (index == v->size()) {
                return v->push_back(value_box{std::move(val)});
            }
#endif
        }
        // Try array
        if (auto* a = get_if<value_array>()) {
            if (index < a->size()) {
                // immer::array 使用 update() 方法来修改元素
                return a->update(index, [&val](const value_box&) {
                    return value_box{std::move(val)};
                });
            }
        }
        
#if IMMER_LENS_AUTO_VIVIFICATION
        // Auto-create vector for null
        if (is_null() && index == 0) {
            return value_vector{}.push_back(value_box{std::move(val)});
        }
#endif

#if IMMER_LENS_VERBOSE_LOG
        std::cerr << "[Value::set] cannot set index " << index << " on non-vector type\n";
#endif
        return *this;
    }
    
    // Check if key exists
    std::size_t count(const std::string& key) const
    {
        if (auto* m = get_if<value_map>()) {
            return m->count(key);
        }
        if (auto* t = get_if<value_table>()) {
            return t->count(key) ? 1 : 0;
        }
        return 0;
    }
    
    // Get size
    std::size_t size() const
    {
        if (auto* m = get_if<value_map>()) return m->size();
        if (auto* v = get_if<value_vector>()) return v->size();
        if (auto* a = get_if<value_array>()) return a->size();
        if (auto* t = get_if<value_table>()) return t->size();
        return 0;
    }
    
    // Size type alias (required by lager::lenses::at)
    using size_type = std::size_t;
};

// ============================================================
// Default Value type (uses default_memory_policy)
// This is the primary type for most use cases and ensures
// backward compatibility with existing code.
// 
// Note: default_memory_policy is THREAD-SAFE:
//   - Uses atomic reference counting
//   - Uses spinlock for free list synchronization
// ============================================================
using Value = BasicValue<immer::default_memory_policy>;

// Type aliases for default memory policy (backward compatible)
using ValueBox    = BasicValueBox<immer::default_memory_policy>;
using ValueMap    = BasicValueMap<immer::default_memory_policy>;
using ValueVector = BasicValueVector<immer::default_memory_policy>;
using ValueArray  = BasicValueArray<immer::default_memory_policy>;
using ValueTable  = BasicValueTable<immer::default_memory_policy>;
using TableEntry  = BasicTableEntry<immer::default_memory_policy>;

// ============================================================
// Single-threaded optimized Value type (LocalValue)
// 
// For applications that don't share Value across threads,
// this provides better performance by avoiding:
//   - Atomic operations for reference counting
//   - Lock acquisition for free list access
//
// Performance difference can be significant (10-30%) for
// workloads with many small modifications.
//
// 类型命名说明：
//   - Value       : 通用版本（线程安全）
//   - LocalValue  : 单线程高性能版本（本类型）
//   - SharedValue : 跨进程共享版本（见 shared_value.h）
//
// WARNING: Using LocalValue in multi-threaded scenarios
// will cause data races and undefined behavior!
// ============================================================

// Single-threaded memory policy (local/non-shared)
using local_memory_policy = immer::memory_policy<
    immer::unsafe_free_list_heap_policy<immer::cpp_heap>,  // 非线程安全空闲链表
    immer::unsafe_refcount_policy,                          // 非原子引用计数
    immer::no_lock_policy                                   // 无锁
>;

// Single-threaded Value types (LocalValue family)
using LocalValue       = BasicValue<local_memory_policy>;
using LocalValueBox    = BasicValueBox<local_memory_policy>;
using LocalValueMap    = BasicValueMap<local_memory_policy>;
using LocalValueVector = BasicValueVector<local_memory_policy>;
using LocalValueArray  = BasicValueArray<local_memory_policy>;
using LocalValueTable  = BasicValueTable<local_memory_policy>;
using LocalTableEntry  = BasicTableEntry<local_memory_policy>;

// Backward compatibility aliases (deprecated, use LocalValue instead)
using unsafe_memory_policy = local_memory_policy;
using UnsafeValue       = LocalValue;
using UnsafeValueBox    = LocalValueBox;
using UnsafeValueMap    = LocalValueMap;
using UnsafeValueVector = LocalValueVector;
using UnsafeValueArray  = LocalValueArray;
using UnsafeValueTable  = LocalValueTable;
using UnsafeTableEntry  = LocalTableEntry;

// ============================================================
// BasicValue equality comparison
// ============================================================
template <typename MemoryPolicy>
bool operator==(const BasicValue<MemoryPolicy>& a, const BasicValue<MemoryPolicy>& b)
{
    return a.data == b.data;
}

template <typename MemoryPolicy>
bool operator!=(const BasicValue<MemoryPolicy>& a, const BasicValue<MemoryPolicy>& b)
{
    return !(a == b);
}

// ============================================================
// Utility functions
// ============================================================

// Convert Value to human-readable string
std::string value_to_string(const Value& val);

// Print Value with indentation
void print_value(const Value& val, const std::string& prefix = "", std::size_t depth = 0);

// Convert Path to dot-notation string (e.g., ".users[0].name")
std::string path_to_string(const Path& path);

// ============================================================
// Common test data factory
// 
// Creates a sample data structure for demos:
// {
//   "users": [
//     { "name": "Alice", "age": 25 },
//     { "name": "Bob", "age": 30 }
//   ],
//   "config": { "version": 1, "theme": "dark" }
// }
// ============================================================
Value create_sample_data();

// ============================================================
// Serialization / Deserialization
// 
// Binary format for efficient memory storage and transfer.
// The format is compact and supports all Value types.
//
// Type tags (1 byte):
//   0x00 = null (monostate)
//   0x01 = int (4 bytes, little-endian)
//   0x02 = float (4 bytes, IEEE 754)
//   0x03 = double (8 bytes, IEEE 754)
//   0x04 = bool (1 byte: 0x00=false, 0x01=true)
//   0x05 = string (4-byte length + UTF-8 data)
//   0x06 = map (4-byte count + entries)
//   0x07 = vector (4-byte count + elements)
//   0x08 = array (4-byte count + elements)
//   0x09 = table (4-byte count + entries)
//
// All multi-byte integers are stored in little-endian format.
// ============================================================

// Byte buffer type for serialization
using ByteBuffer = std::vector<uint8_t>;

// Serialize Value to binary buffer
// Returns: byte buffer containing serialized data
ByteBuffer serialize(const Value& val);

// Deserialize Value from binary buffer
// Returns: reconstructed Value, or null Value on error
// Note: throws std::runtime_error on invalid data format
Value deserialize(const ByteBuffer& buffer);

// Deserialize from raw pointer and size
// Useful for memory-mapped data or network buffers
Value deserialize(const uint8_t* data, std::size_t size);

// ============================================================
// Serialization utilities
// ============================================================

// Get serialized size without actually serializing
// Useful for pre-allocating buffers
std::size_t serialized_size(const Value& val);

// Serialize to pre-allocated buffer
// Returns: number of bytes written
// Note: buffer must have at least serialized_size(val) bytes
std::size_t serialize_to(const Value& val, uint8_t* buffer, std::size_t buffer_size);

} // namespace immer_lens
