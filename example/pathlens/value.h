// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file value.h
/// @brief Common Value type definition and utilities for JSON-like dynamic data.
///
/// This file defines the core Value type that can represent:
/// - Primitive types: int, float, double, bool, string
/// - Math types: Vec2, Vec3, Vec4, Mat3, Mat4x3, Mat4 (fixed-size float arrays)
/// - Container types: map, vector, array, table (using immer's immutable containers)
/// - Null (std::monostate)
///
/// The Value type is templated on a memory policy, allowing users to
/// customize memory allocation strategies for the underlying immer containers.

#pragma once

#include <immer/array.hpp>
#include <immer/array_transient.hpp>
#include <immer/box.hpp>
#include <immer/map.hpp>
#include <immer/memory_policy.hpp>
#include <immer/table.hpp>
#include <immer/vector.hpp>

#include <algorithm>  // for std::copy_n
#include <array>      // for Vec2, Vec3, Vec4, Mat3, Mat4x3, Mat4
#include <compare>    // for std::strong_ordering (C++20)
#include <cstdint>
#include <iostream>
#include <ranges>     // for std::ranges::copy (C++20)
#include <span>       // for std::span (C++20)
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

/// @defgroup MathTypes Math Type Aliases
/// @brief Fixed-size float arrays for common mathematical data.
///
/// These types are used for efficient storage of common mathematical data:
///   - Vec2:   2D vector (position, UV coordinates, etc.)
///   - Vec3:   3D vector (position, direction, RGB color, etc.)
///   - Vec4:   4D vector (homogeneous coordinates, RGBA color, quaternion, etc.)
///   - Mat3:   3x3 matrix (2D transforms, rotation matrices, etc.)
///   - Mat4x3: 4x3 matrix (compact affine transform, 4 rows x 3 columns)
///   - Mat4:   4x4 matrix (full 3D transformation matrix)
///
/// Memory Layout:
///   - All types are contiguous float arrays.
///   - Matrices are stored in row-major order (consistent with DirectX convention).
///   - Mat3:  [m00, m01, m02, m10, m11, m12, m20, m21, m22]
///   - Mat4x3: [m00, m01, m02, m10, m11, m12, m20, m21, m22, m30, m31, m32]
///   - Mat4:  [m00, m01, m02, m03, m10, m11, m12, m13, ...]
/// @{
using Vec2 = std::array<float, 2>;    ///< 2D vector: [x, y]
using Vec3 = std::array<float, 3>;    ///< 3D vector: [x, y, z]
using Vec4 = std::array<float, 4>;    ///< 4D vector: [x, y, z, w]
using Mat3 = std::array<float, 9>;    ///< 3x3 matrix (row-major)
using Mat4x3 = std::array<float, 12>; ///< 4x3 matrix (row-major, 4 rows x 3 columns)
using Mat4 = std::array<float, 16>;   ///< 4x4 matrix (row-major)
/// @}

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
// - Math types: Vec2, Vec3, Vec4, Mat3, Mat4x3, Mat4
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
                 Vec2,       // 2D vector
                 Vec3,       // 3D vector
                 Vec4,       // 4D vector / quaternion
                 Mat3,       // 3x3 matrix
                 Mat4x3,     // 4x3 matrix (4 rows x 3 columns)
                 Mat4,       // 4x4 matrix
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
    // Math type constructors
    BasicValue(Vec2 v) : data(v) {}
    BasicValue(Vec3 v) : data(v) {}
    BasicValue(Vec4 v) : data(v) {}
    BasicValue(Mat3 v) : data(v) {}
    BasicValue(Mat4x3 v) : data(v) {}
    BasicValue(Mat4 v) : data(v) {}
    // Container type constructors
    BasicValue(value_map v) : data(std::move(v)) {}
    BasicValue(value_vector v) : data(std::move(v)) {}
    BasicValue(value_array v) : data(std::move(v)) {}
    BasicValue(value_table v) : data(std::move(v)) {}
    
    // ============================================================
    // Factory functions for container types
    // 
    // All factory functions use transient for O(N) batch construction.
    //
    // Container types:
    //   - object() / map()    -> value_map (HAMT, O(log N) lookup)
    //   - vector()            -> value_vector (RRB-tree, O(log N) random access)
    //   - array()             -> value_array (contiguous, O(1) random access, fixed after creation)
    //   - table()             -> value_table (HAMT with .id key, O(log N) lookup)
    //
    // Usage examples:
    //   // Map (object) from key-value pairs
    //   Value obj = Value::object({
    //       {"name", "Alice"},
    //       {"age", 25},
    //       {"active", true}
    //   });
    //   
    //   // Vector for dynamic arrays
    //   Value nums = Value::vector({1, 2, 3, 4, 5});
    //
    //   // Array for fixed-size contiguous data
    //   Value coords = Value::array({1.0, 2.0, 3.0});
    //
    //   // Table for id-indexed collections
    //   Value users = Value::table({
    //       {"user_001", Value::object({{"name", "Alice"}})},
    //       {"user_002", Value::object({{"name", "Bob"}})}
    //   });
    //
    //   // Nested structures
    //   Value scene = Value::object({
    //       {"objects", Value::table({
    //           {"obj_1", Value::object({{"name", "Cube"}, {"visible", true}})},
    //           {"obj_2", Value::object({{"name", "Sphere"}, {"visible", false}})}
    //       })},
    //       {"settings", Value::object({{"quality", "high"}})}
    //   });
    // ============================================================
    
    /// Create a map from initializer list of key-value pairs (HAMT)
    /// @param init List of {key, value} pairs
    /// @return BasicValue containing a value_map
    static BasicValue object(std::initializer_list<std::pair<std::string, BasicValue>> init) {
        auto t = value_map{}.transient();
        for (const auto& [key, val] : init) {
            t.set(key, value_box{val});
        }
        return BasicValue{t.persistent()};
    }
    
    /// Alias for object() - creates a value_map
    static BasicValue map(std::initializer_list<std::pair<std::string, BasicValue>> init) {
        return object(init);
    }
    
    /// Create a vector from initializer list (RRB-tree, supports efficient append/update)
    /// @param init List of values
    /// @return BasicValue containing a value_vector
    static BasicValue vector(std::initializer_list<BasicValue> init) {
        auto t = value_vector{}.transient();
        for (const auto& val : init) {
            t.push_back(value_box{val});
        }
        return BasicValue{t.persistent()};
    }
    
    /// Create an array from initializer list (contiguous memory, O(1) access)
    /// Note: value_array is optimized for read-heavy workloads with fixed size
    /// @param init List of values
    /// @return BasicValue containing a value_array
    static BasicValue array(std::initializer_list<BasicValue> init) {
        auto t = value_array{}.transient();
        for (const auto& val : init) {
            t.push_back(value_box{val});
        }
        return BasicValue{t.persistent()};
    }
    
    /// Create a table from initializer list of id-value pairs (HAMT with .id key)
    /// Tables are optimized for collections where each entry has a unique string id
    /// @param init List of {id, value} pairs
    /// @return BasicValue containing a value_table
    static BasicValue table(std::initializer_list<std::pair<std::string, BasicValue>> init) {
        auto t = value_table{}.transient();
        for (const auto& [id, val] : init) {
            t.insert(table_entry{id, value_box{val}});
        }
        return BasicValue{t.persistent()};
    }

    // ============================================================
    // Factory functions for math types
    // ============================================================
    static BasicValue vec2(float x, float y) {
        return BasicValue{Vec2{x, y}};
    }
    
    static BasicValue vec3(float x, float y, float z) {
        return BasicValue{Vec3{x, y, z}};
    }
    
    static BasicValue vec4(float x, float y, float z, float w) {
        return BasicValue{Vec4{x, y, z, w}};
    }
    
    // Factory functions from std::span (for interoperability with external math libraries)
    static BasicValue vec2(std::span<const float, 2> data) {
        Vec2 v;
        std::ranges::copy(data, v.begin());
        return BasicValue{v};
    }
    
    static BasicValue vec3(std::span<const float, 3> data) {
        Vec3 v;
        std::ranges::copy(data, v.begin());
        return BasicValue{v};
    }
    
    static BasicValue vec4(std::span<const float, 4> data) {
        Vec4 v;
        std::ranges::copy(data, v.begin());
        return BasicValue{v};
    }
    
    static BasicValue mat3(std::span<const float, 9> data) {
        Mat3 m;
        std::ranges::copy(data, m.begin());
        return BasicValue{m};
    }
    
    static BasicValue mat4x3(std::span<const float, 12> data) {
        Mat4x3 m;
        std::ranges::copy(data, m.begin());
        return BasicValue{m};
    }
    
    static BasicValue mat4(std::span<const float, 16> data) {
        Mat4 m;
        std::ranges::copy(data, m.begin());
        return BasicValue{m};
    }
    
    // Factory functions from raw pointers (backward compatibility)
    static BasicValue vec2(const float* ptr) {
        return vec2(std::span<const float, 2>{ptr, 2});
    }
    
    static BasicValue vec3(const float* ptr) {
        return vec3(std::span<const float, 3>{ptr, 3});
    }
    
    static BasicValue vec4(const float* ptr) {
        return vec4(std::span<const float, 4>{ptr, 4});
    }
    
    static BasicValue mat3(const float* ptr) {
        return mat3(std::span<const float, 9>{ptr, 9});
    }
    
    static BasicValue mat4x3(const float* ptr) {
        return mat4x3(std::span<const float, 12>{ptr, 12});
    }
    
    static BasicValue mat4(const float* ptr) {
        return mat4(std::span<const float, 16>{ptr, 16});
    }
    
    // Identity matrix factory functions
    static BasicValue identity_mat3() {
        return BasicValue{Mat3{
            1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 1.0f
        }};
    }
    
    static BasicValue identity_mat4() {
        return BasicValue{Mat4{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        }};
    }

    // Type inspection
    template <typename T>
    [[nodiscard]] const T* get_if() const
    {
        return std::get_if<T>(&data);
    }

    template <typename T>
    [[nodiscard]] bool is() const
    {
        return std::holds_alternative<T>(data);
    }
    
    [[nodiscard]] std::size_t type_index() const noexcept { return data.index(); }
    [[nodiscard]] bool is_null() const noexcept { return std::holds_alternative<std::monostate>(data); }
    
    // ============================================================
    // Math type inspection methods
    // ============================================================
    [[nodiscard]] bool is_vec2() const noexcept { return is<Vec2>(); }
    [[nodiscard]] bool is_vec3() const noexcept { return is<Vec3>(); }
    [[nodiscard]] bool is_vec4() const noexcept { return is<Vec4>(); }
    [[nodiscard]] bool is_mat3() const noexcept { return is<Mat3>(); }
    [[nodiscard]] bool is_mat4x3() const noexcept { return is<Mat4x3>(); }
    [[nodiscard]] bool is_mat4() const noexcept { return is<Mat4>(); }
    [[nodiscard]] bool is_math_type() const noexcept {
        return is_vec2() || is_vec3() || is_vec4() || 
               is_mat3() || is_mat4x3() || is_mat4();
    }
    
    // ============================================================
    // Container-like interface for lager::lenses::at compatibility
    // 
    // Design choice: No exceptions thrown. Instead:
    // - at() returns null Value if key/index not found or type mismatch
    // - try_at() returns std::optional for explicit "not found" handling
    // - set() returns self unchanged if type mismatch
    // - Errors are logged to stderr for debugging
    // ============================================================
    
    // Access by string key (for map-like access)
    [[nodiscard]] BasicValue at(const std::string& key) const
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
    [[nodiscard]] BasicValue at(std::size_t index) const
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
    // Access with default value
    // 
    // These methods return a default value when the key/index is not found
    // or when the value is null (monostate).
    // 
    // Note: Since Value uses std::monostate to represent "null/not found",
    // there's no need for std::optional - just check is_null() on the result.
    // ============================================================
    
    /// Get value at key with default if not found or null
    [[nodiscard]] BasicValue at_or(const std::string& key, BasicValue default_val) const
    {
        auto result = at(key);
        return result.is_null() ? std::move(default_val) : std::move(result);
    }
    
    /// Get value at index with default if not found or null
    [[nodiscard]] BasicValue at_or(std::size_t index, BasicValue default_val) const
    {
        auto result = at(index);
        return result.is_null() ? std::move(default_val) : std::move(result);
    }
    
    // Check if key/index exists
    [[nodiscard]] bool contains(const std::string& key) const
    {
        return count(key) > 0;
    }
    
    [[nodiscard]] bool contains(std::size_t index) const
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
    [[nodiscard]] BasicValue set(const std::string& key, BasicValue val) const
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
    [[nodiscard]] BasicValue set(std::size_t index, BasicValue val) const
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
                // immer::array uses update() method to modify elements
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
    [[nodiscard]] std::size_t count(const std::string& key) const
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
    [[nodiscard]] std::size_t size() const
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
// Builder Classes for Dynamic Construction
// 
// These builders provide O(n) construction for large data structures
// by using immer's transient API internally.
//
// Performance comparison:
//   - Repeated .set() calls:  O(n log n) - creates new tree nodes each time
//   - Builder API:            O(n) - uses transient for in-place mutation
//
// Usage examples:
//   // Build a map with 50,000 entries - O(n)
//   auto builder = Value::build_map();
//   for (int i = 0; i < 50000; i++) {
//       builder.set("key_" + std::to_string(i), i);
//   }
//   Value result = builder.finish();
//
//   // Build a vector with 100,000 elements - O(n)
//   auto vec_builder = Value::build_vector();
//   vec_builder.reserve(100000);  // Optional: pre-allocate
//   for (int i = 0; i < 100000; i++) {
//       vec_builder.push_back(i);
//   }
//   Value result = vec_builder.finish();
//
//   // Nested structures - all O(n)
//   Value scene = Value::build_map()
//       .set("version", 1)
//       .set("objects", Value::build_vector()
//           .push_back(Value::build_map()
//               .set("id", 1)
//               .set("name", "Object1")
//               .finish())
//           .push_back(Value::build_map()
//               .set("id", 2)
//               .set("name", "Object2")
//               .finish())
//           .finish())
//       .finish();
// ============================================================

/// Builder for constructing value_map efficiently - O(n) complexity
template <typename MemoryPolicy>
class BasicMapBuilder {
public:
    using value_type = BasicValue<MemoryPolicy>;
    using value_box = BasicValueBox<MemoryPolicy>;
    using value_map = BasicValueMap<MemoryPolicy>;
    using transient_type = typename value_map::transient_type;
    
    BasicMapBuilder() : transient_(value_map{}.transient()) {}
    
    /// Set a key-value pair
    /// @param key The key
    /// @param val The value (any type convertible to BasicValue)
    /// @return Reference to this builder for chaining
    template <typename T>
    BasicMapBuilder& set(const std::string& key, T&& val) {
        transient_.set(key, value_box{value_type{std::forward<T>(val)}});
        return *this;
    }
    
    /// Set a key with an already constructed BasicValue
    BasicMapBuilder& set(const std::string& key, value_type val) {
        transient_.set(key, value_box{std::move(val)});
        return *this;
    }
    
    /// Check if the builder contains a key
    [[nodiscard]] bool contains(const std::string& key) const {
        return transient_.count(key) > 0;
    }
    
    /// Get current size
    [[nodiscard]] std::size_t size() const {
        return transient_.size();
    }
    
    /// Finish building and return the immutable Value
    /// Note: After calling finish(), the builder is in an undefined state
    [[nodiscard]] value_type finish() {
        return value_type{transient_.persistent()};
    }
    
    /// Finish and return just the map (not wrapped in Value)
    [[nodiscard]] value_map finish_map() {
        return transient_.persistent();
    }

private:
    transient_type transient_;
};

/// Builder for constructing value_vector efficiently - O(n) complexity
template <typename MemoryPolicy>
class BasicVectorBuilder {
public:
    using value_type = BasicValue<MemoryPolicy>;
    using value_box = BasicValueBox<MemoryPolicy>;
    using value_vector = BasicValueVector<MemoryPolicy>;
    using transient_type = typename value_vector::transient_type;
    
    BasicVectorBuilder() : transient_(value_vector{}.transient()) {}
    
    /// Append a value to the end
    /// @param val The value (any type convertible to BasicValue)
    /// @return Reference to this builder for chaining
    template <typename T>
    BasicVectorBuilder& push_back(T&& val) {
        transient_.push_back(value_box{value_type{std::forward<T>(val)}});
        return *this;
    }
    
    /// Append an already constructed BasicValue
    BasicVectorBuilder& push_back(value_type val) {
        transient_.push_back(value_box{std::move(val)});
        return *this;
    }
    
    /// Set value at index (must be within current size)
    template <typename T>
    BasicVectorBuilder& set(std::size_t index, T&& val) {
        if (index < transient_.size()) {
            transient_.set(index, value_box{value_type{std::forward<T>(val)}});
        }
        return *this;
    }
    
    /// Get current size
    [[nodiscard]] std::size_t size() const {
        return transient_.size();
    }
    
    /// Finish building and return the immutable Value
    [[nodiscard]] value_type finish() {
        return value_type{transient_.persistent()};
    }
    
    /// Finish and return just the vector (not wrapped in Value)
    [[nodiscard]] value_vector finish_vector() {
        return transient_.persistent();
    }

private:
    transient_type transient_;
};

/// Builder for constructing value_array efficiently - O(n) complexity
template <typename MemoryPolicy>
class BasicArrayBuilder {
public:
    using value_type = BasicValue<MemoryPolicy>;
    using value_box = BasicValueBox<MemoryPolicy>;
    using value_array = BasicValueArray<MemoryPolicy>;
    using transient_type = typename value_array::transient_type;
    
    BasicArrayBuilder() : transient_(value_array{}.transient()) {}
    
    /// Append a value to the end
    template <typename T>
    BasicArrayBuilder& push_back(T&& val) {
        transient_.push_back(value_box{value_type{std::forward<T>(val)}});
        return *this;
    }
    
    /// Append an already constructed BasicValue
    BasicArrayBuilder& push_back(value_type val) {
        transient_.push_back(value_box{std::move(val)});
        return *this;
    }
    
    /// Get current size
    [[nodiscard]] std::size_t size() const {
        return transient_.size();
    }
    
    /// Finish building and return the immutable Value
    [[nodiscard]] value_type finish() {
        return value_type{transient_.persistent()};
    }
    
    /// Finish and return just the array (not wrapped in Value)
    [[nodiscard]] value_array finish_array() {
        return transient_.persistent();
    }

private:
    transient_type transient_;
};

/// Builder for constructing value_table efficiently - O(n) complexity
template <typename MemoryPolicy>
class BasicTableBuilder {
public:
    using value_type = BasicValue<MemoryPolicy>;
    using value_box = BasicValueBox<MemoryPolicy>;
    using value_table = BasicValueTable<MemoryPolicy>;
    using table_entry = BasicTableEntry<MemoryPolicy>;
    using transient_type = typename value_table::transient_type;
    
    BasicTableBuilder() : transient_(value_table{}.transient()) {}
    
    /// Insert or update an entry by id
    /// @param id The unique identifier
    /// @param val The value (any type convertible to BasicValue)
    /// @return Reference to this builder for chaining
    template <typename T>
    BasicTableBuilder& insert(const std::string& id, T&& val) {
        transient_.insert(table_entry{id, value_box{value_type{std::forward<T>(val)}}});
        return *this;
    }
    
    /// Insert with an already constructed BasicValue
    BasicTableBuilder& insert(const std::string& id, value_type val) {
        transient_.insert(table_entry{id, value_box{std::move(val)}});
        return *this;
    }
    
    /// Check if the builder contains an id
    [[nodiscard]] bool contains(const std::string& id) const {
        return transient_.count(id) > 0;
    }
    
    /// Get current size
    [[nodiscard]] std::size_t size() const {
        return transient_.size();
    }
    
    /// Finish building and return the immutable Value
    [[nodiscard]] value_type finish() {
        return value_type{transient_.persistent()};
    }
    
    /// Finish and return just the table (not wrapped in Value)
    [[nodiscard]] value_table finish_table() {
        return transient_.persistent();
    }

private:
    transient_type transient_;
};

// ============================================================
// Memory Policy Definitions
// ============================================================

/// Single-threaded memory policy: non-atomic refcount + no locks, highest performance
using unsafe_memory_policy = immer::memory_policy<
    immer::unsafe_free_list_heap_policy<immer::cpp_heap>,
    immer::unsafe_refcount_policy,
    immer::no_lock_policy
>;

/// Thread-safe memory policy: atomic refcount + spinlock
using thread_safe_memory_policy = immer::default_memory_policy;

// ============================================================
// UnsafeValue - Single-threaded high-performance Value
// 
// Features:
//   - Non-atomic reference counting (avoids CPU cache line bouncing)
//   - No lock overhead (no spinlock)
//   - 10-30% faster than thread-safe version
//
// Use cases:
//   - Single-threaded applications
//   - Each thread has its own independent Value tree (no sharing)
//   - Performance-critical hot paths
//
// WARNING: Sharing UnsafeValue across threads causes data races and UB!
//          Use ThreadSafeValue for multi-threaded scenarios.
// ============================================================
using UnsafeValue       = BasicValue<unsafe_memory_policy>;
using UnsafeValueBox    = BasicValueBox<unsafe_memory_policy>;
using UnsafeValueMap    = BasicValueMap<unsafe_memory_policy>;
using UnsafeValueVector = BasicValueVector<unsafe_memory_policy>;
using UnsafeValueArray  = BasicValueArray<unsafe_memory_policy>;
using UnsafeValueTable  = BasicValueTable<unsafe_memory_policy>;
using UnsafeTableEntry  = BasicTableEntry<unsafe_memory_policy>;

// ============================================================
// ThreadSafeValue - Thread-safe Value for multi-threaded scenarios
// 
// Features:
//   - Atomic reference counting (std::atomic operations)
//   - Spinlock-protected free list
//
// Use cases:
//   - Sharing the same Value tree across multiple threads
//   - Cross-thread Value passing (e.g., message queues, event systems)
//   - Integration with lager store (store may be accessed from multiple threads)
//
// Performance note:
//   - 10-30% slower than UnsafeValue (depends on contention level)
//   - Performance degradation is more pronounced under high contention
// ============================================================
using ThreadSafeValue       = BasicValue<thread_safe_memory_policy>;
using ThreadSafeValueBox    = BasicValueBox<thread_safe_memory_policy>;
using ThreadSafeValueMap    = BasicValueMap<thread_safe_memory_policy>;
using ThreadSafeValueVector = BasicValueVector<thread_safe_memory_policy>;
using ThreadSafeValueArray  = BasicValueArray<thread_safe_memory_policy>;
using ThreadSafeValueTable  = BasicValueTable<thread_safe_memory_policy>;
using ThreadSafeTableEntry  = BasicTableEntry<thread_safe_memory_policy>;

// ============================================================
// Default Value Type Aliases
// 
// Naming conventions:
//   - Value       : Default type, alias for UnsafeValue (single-threaded, high performance)
//   - SyncValue   : Convenience alias for ThreadSafeValue (multi-threaded safe)
//   - SharedValue : Cross-process shared version (see shared_value.h)
//
// Design philosophy:
//   - Base type names explicitly express safety characteristics (UnsafeValue / ThreadSafeValue)
//   - Short aliases for everyday use (Value / SyncValue)
//   - Most single-threaded scenarios can simply use Value
// ============================================================

// Value = UnsafeValue (default, single-threaded, high performance)
using Value       = UnsafeValue;
using ValueBox    = UnsafeValueBox;
using ValueMap    = UnsafeValueMap;
using ValueVector = UnsafeValueVector;
using ValueArray  = UnsafeValueArray;
using ValueTable  = UnsafeValueTable;
using TableEntry  = UnsafeTableEntry;

// SyncValue = ThreadSafeValue (multi-threaded safe)
using SyncValue       = ThreadSafeValue;
using SyncValueBox    = ThreadSafeValueBox;
using SyncValueMap    = ThreadSafeValueMap;
using SyncValueVector = ThreadSafeValueVector;
using SyncValueArray  = ThreadSafeValueArray;
using SyncValueTable  = ThreadSafeValueTable;
using SyncTableEntry  = ThreadSafeTableEntry;

// ============================================================
// Builder type aliases
// ============================================================

// Unsafe (single-threaded) builders - use with Value
using MapBuilder    = BasicMapBuilder<unsafe_memory_policy>;
using VectorBuilder = BasicVectorBuilder<unsafe_memory_policy>;
using ArrayBuilder  = BasicArrayBuilder<unsafe_memory_policy>;
using TableBuilder  = BasicTableBuilder<unsafe_memory_policy>;

// Thread-safe builders - use with SyncValue
using SyncMapBuilder    = BasicMapBuilder<thread_safe_memory_policy>;
using SyncVectorBuilder = BasicVectorBuilder<thread_safe_memory_policy>;
using SyncArrayBuilder  = BasicArrayBuilder<thread_safe_memory_policy>;
using SyncTableBuilder  = BasicTableBuilder<thread_safe_memory_policy>;

// ============================================================
// BasicValue comparison operators (C++20)
// 
// Uses spaceship operator (<=>): compiler auto-generates ==, !=, <, >, <=, >=
// Note: std::variant supports <=> in C++20, enabling lexicographic comparison
// ============================================================

/// Equality comparison for BasicValue
template <typename MemoryPolicy>
bool operator==(const BasicValue<MemoryPolicy>& a, const BasicValue<MemoryPolicy>& b)
{
    return a.data == b.data;
}

/// Three-way comparison (spaceship operator) for BasicValue
/// Enables all comparison operators: ==, !=, <, >, <=, >=
/// Returns std::partial_ordering because floating-point types may be NaN
template <typename MemoryPolicy>
std::partial_ordering operator<=>(const BasicValue<MemoryPolicy>& a, 
                                   const BasicValue<MemoryPolicy>& b)
{
    // Compare type indices first
    if (a.data.index() != b.data.index()) {
        return a.data.index() <=> b.data.index();
    }
    
    // Same type, compare values using std::visit
    return std::visit([](const auto& lhs, const auto& rhs) -> std::partial_ordering {
        using T = std::decay_t<decltype(lhs)>;
        using U = std::decay_t<decltype(rhs)>;
        
        if constexpr (std::is_same_v<T, U>) {
            // Same type, compare directly
            if constexpr (std::is_same_v<T, std::monostate>) {
                return std::partial_ordering::equivalent;
            } else if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
                // Floating-point: use partial_ordering
                return lhs <=> rhs;
            } else if constexpr (requires { lhs <=> rhs; }) {
                // Type supports <=>
                auto result = lhs <=> rhs;
                if constexpr (std::is_same_v<decltype(result), std::strong_ordering>) {
                    return static_cast<std::partial_ordering>(result);
                } else {
                    return result;
                }
            } else if constexpr (requires { lhs < rhs; lhs == rhs; }) {
                // Fall back to < and == operators
                if (lhs == rhs) return std::partial_ordering::equivalent;
                if (lhs < rhs) return std::partial_ordering::less;
                return std::partial_ordering::greater;
            } else {
                // Types without comparison (e.g., immer containers)
                // Fall back to address comparison (arbitrary but consistent)
                return std::partial_ordering::equivalent;
            }
        } else {
            // Different types shouldn't happen (same index), but handle anyway
            return std::partial_ordering::unordered;
        }
    }, a.data, b.data);
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
//   0x0A = int64 (8 bytes, little-endian)
//   0x10 = Vec2 (8 bytes, 2 floats)
//   0x11 = Vec3 (12 bytes, 3 floats)
//   0x12 = Vec4 (16 bytes, 4 floats)
//   0x13 = Mat3 (36 bytes, 9 floats)
//   0x14 = Mat4x3 (48 bytes, 12 floats)
//   0x15 = Mat4 (64 bytes, 16 floats)
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

// ============================================================
// JSON Serialization / Deserialization
// 
// Provides human-readable JSON format for:
// - Configuration files
// - Network APIs
// - Debugging and logging
// - Interoperability with other systems
//
// Special handling for math types:
// - Vec2, Vec3, Vec4: JSON arrays [x, y, ...] 
// - Mat3, Mat4x3, Mat4: JSON arrays of floats (row-major)
//
// Note: JSON has limitations:
// - Numbers are always double precision (int64 may lose precision)
// - Binary data must be base64 encoded
// - Null, true, false are reserved keywords
// ============================================================

// Convert Value to JSON string
// compact: if false, adds indentation and newlines for readability
std::string to_json(const Value& val, bool compact = false);

// Parse JSON string to Value
// Returns: parsed Value, or null Value on parse error
// error_out: if provided, receives error message on failure
Value from_json(const std::string& json_str, std::string* error_out = nullptr);

// Note: JSON Pointer functions (path_to_json_pointer, json_pointer_to_path)
// are declared in json_pointer.h

} // namespace immer_lens