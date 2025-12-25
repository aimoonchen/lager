// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file lager_lens.h
/// @brief Scheme 2: Using lager::lens<Value, Value> for type erasure.
///
/// This approach uses lager's built-in type-erased lens instead of
/// custom ErasedLens. Both approaches work for dynamic JSON-like data.
///
/// Key insight: Since our data model is Value -> Value -> Value...,
/// we can use lager::lens<Value, Value> as a uniform type for
/// dynamically composed lenses.

#pragma once

#include <lager_ext/value.h>
#include <lager/lens.hpp>
#include <lager/lenses.hpp>
#include <zug/compose.hpp>

#include <concepts>
#include <type_traits>

namespace lager_ext {

// ============================================================
// C++20 Concepts for lens element types
// ============================================================

/// Concept for types that can be used as map keys (string-like)
template<typename T>
concept StringLike = std::is_convertible_v<T, std::string> ||
                     std::is_same_v<std::decay_t<T>, const char*> ||
                     std::is_same_v<std::decay_t<T>, std::string_view>;

/// Concept for types that can be used as array indices
template<typename T>
concept IndexLike = std::is_integral_v<std::decay_t<T>> && !StringLike<T>;

/// Concept for valid path element types
template<typename T>
concept PathElementType = StringLike<T> || IndexLike<T>;

// ============================================================
// Unified Lens Interface (C++20 Concepts)
//
// This provides a common interface for all lens types:
// - ErasedLens (Scheme 1)
// - LagerValueLens (Scheme 2)
// - StaticPath (compile-time)
// - lager::lenses::* (lager built-in)
//
// Usage:
//   template<ValueLens L>
//   void process(const L& lens, const Value& data) { ... }
// ============================================================

namespace detail {

// Helper to detect if a type has .get() method
template<typename L>
concept HasGetMethod = requires(const L& lens, const Value& v) {
    { lens.get(v) } -> std::convertible_to<Value>;
};

// Helper to detect if a type has .set() method
template<typename L>
concept HasSetMethod = requires(const L& lens, Value v, Value p) {
    { lens.set(std::move(v), std::move(p)) } -> std::convertible_to<Value>;
};

// Helper to detect if a type works with lager::view
template<typename L>
concept WorksWithLagerView = requires(const L& lens, const Value& v) {
    { lager::view(lens, v) } -> std::convertible_to<Value>;
};

// Helper to detect if a type works with lager::set
template<typename L>
concept WorksWithLagerSet = requires(const L& lens, Value v, Value p) {
    { lager::set(lens, std::move(v), std::move(p)) } -> std::convertible_to<Value>;
};

} // namespace detail

/// Concept for any lens that can get a Value from a Value
template<typename L>
concept ValueGetter = detail::HasGetMethod<L> || detail::WorksWithLagerView<L>;

/// Concept for any lens that can set a Value in a Value
template<typename L>
concept ValueSetter = detail::HasSetMethod<L> || detail::WorksWithLagerSet<L>;

/// Concept for a complete lens (supports both get and set)
template<typename L>
concept ValueLens = ValueGetter<L> && ValueSetter<L>;

// ============================================================
// Unified Lens Operations
//
// These free functions work with any lens type that satisfies
// the ValueLens concept, providing a consistent API.
// ============================================================

/// Get value using any lens type
/// @param lens The lens (ErasedLens, LagerValueLens, or lager lens)
/// @param whole The root value
/// @return The focused value
template<ValueGetter L>
[[nodiscard]] Value lens_get(const L& lens, const Value& whole) {
    if constexpr (detail::HasGetMethod<L>) {
        return lens.get(whole);
    } else {
        return lager::view(lens, whole);
    }
}

/// Set value using any lens type
/// @param lens The lens
/// @param whole The root value
/// @param part The new value to set
/// @return New root value with the update applied
template<ValueSetter L>
[[nodiscard]] Value lens_set(const L& lens, Value whole, Value part) {
    if constexpr (detail::HasSetMethod<L>) {
        return lens.set(std::move(whole), std::move(part));
    } else {
        return lager::set(lens, std::move(whole), std::move(part));
    }
}

/// Update value using any lens type
/// @param lens The lens
/// @param whole The root value
/// @param fn Function to transform the focused value
/// @return New root value with the update applied
template<ValueLens L, typename Fn>
[[nodiscard]] Value lens_over(const L& lens, Value whole, Fn&& fn) {
    if constexpr (requires { lens.over(whole, fn); }) {
        return lens.over(std::move(whole), std::forward<Fn>(fn));
    } else if constexpr (detail::HasGetMethod<L> && detail::HasSetMethod<L>) {
        Value current = lens.get(whole);
        Value updated = std::forward<Fn>(fn)(std::move(current));
        return lens.set(std::move(whole), std::move(updated));
    } else {
        return lager::over(lens, std::move(whole), std::forward<Fn>(fn));
    }
}

// ============================================================
// Lens Composition Helpers
// ============================================================

/// Compose two lenses (left-to-right: outer then inner)
/// Works with any lens types that satisfy ValueLens
template<ValueLens L1, ValueLens L2>
[[nodiscard]] auto lens_compose(L1&& outer, L2&& inner) {
    return lager::lenses::getset(
        // Getter: apply outer, then inner
        [outer = std::forward<L1>(outer), inner = std::forward<L2>(inner)]
        (const Value& whole) -> Value {
            return lens_get(inner, lens_get(outer, whole));
        },
        // Setter: get outer part, set inner, then set outer
        [outer, inner](Value whole, Value new_part) -> Value {
            Value outer_part = lens_get(outer, whole);
            Value new_outer = lens_set(inner, std::move(outer_part), std::move(new_part));
            return lens_set(outer, std::move(whole), std::move(new_outer));
        }
    );
}

// Type alias for lager's type-erased lens with Value as both Whole and Part
using LagerValueLens = lager::lens<Value, Value>;

// ============================================================
// Lens factory functions
// ============================================================

// Create a getset lens that focuses on a map key
[[nodiscard]] auto key_lens(const std::string& key);

// Create a getset lens that focuses on a vector index
[[nodiscard]] auto index_lens(std::size_t index);

// Create a type-erased lens for a map key
[[nodiscard]] LagerValueLens lager_key_lens(const std::string& key);

// Create a type-erased lens for a vector index
[[nodiscard]] LagerValueLens lager_index_lens(std::size_t index);

// Build a type-erased lens from a path
[[nodiscard]] LagerValueLens lager_path_lens(const Path& path);

// ============================================================
// Static path lens using fold expression
// Use when path elements are known at compile time
// ============================================================

namespace detail {

/// Convert a string-like element to a key lens
auto element_to_lens(StringLike auto&& elem) {
    return key_lens(std::string{std::forward<decltype(elem)>(elem)});
}

/// Convert an index-like element to an index lens
auto element_to_lens(IndexLike auto&& elem) {
    return index_lens(static_cast<std::size_t>(elem));
}

} // namespace detail

/// Build a composed lens from compile-time known path elements.
/// @tparam Elements Types that satisfy PathElementType concept
/// @param elements Path elements (strings for keys, integers for indices)
/// @return Composed lens for the path
template <PathElementType... Elements>
auto static_path_lens(Elements&&... elements)
{
    return (zug::identity | ... | detail::element_to_lens(std::forward<Elements>(elements)));
}

// ============================================================
// Lens cache management
// ============================================================

// Cache statistics structure
struct LensCacheStats {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t size = 0;
    std::size_t capacity = 0;
    double hit_rate = 0.0;
};

// Clear the lens cache (useful for testing or memory management)
void clear_lens_cache();

// Get lens cache statistics
[[nodiscard]] LensCacheStats get_lens_cache_stats();

// ============================================================
// Chainable Path Builder API
//
// Provides a fluent interface for constructing paths:
//   auto path = PathBuilder().key("users").index(0).key("name");
//   auto lens = path.to_lens();
//   Value v = path.get(data);
//   Value u = path.set(data, new_val);
//
// Also supports operator/ for concise path construction:
//   auto lens = (root / "users" / 0 / "name").to_lens();
// ============================================================

/// Chainable path builder for fluent lens construction
class PathBuilder {
public:
    /// Create an empty path builder (root path)
    PathBuilder() = default;

    /// Create from existing path
    explicit PathBuilder(Path path) : path_(std::move(path)) {}

    /// Add a string key segment
    [[nodiscard]] PathBuilder key(const std::string& k) const & {
        PathBuilder result = *this;
        result.path_.push_back(k);
        return result;
    }

    /// Add a string key segment (move optimization)
    [[nodiscard]] PathBuilder key(const std::string& k) && {
        path_.push_back(k);
        return std::move(*this);
    }

    /// Add an index segment
    [[nodiscard]] PathBuilder index(std::size_t i) const & {
        PathBuilder result = *this;
        result.path_.push_back(i);
        return result;
    }

    /// Add an index segment (move optimization)
    [[nodiscard]] PathBuilder index(std::size_t i) && {
        path_.push_back(i);
        return std::move(*this);
    }

    /// Operator / for string keys (chainable)
    [[nodiscard]] PathBuilder operator/(const std::string& k) const & {
        return key(k);
    }

    [[nodiscard]] PathBuilder operator/(const std::string& k) && {
        return std::move(*this).key(k);
    }

    /// Operator / for const char* keys
    [[nodiscard]] PathBuilder operator/(const char* k) const & {
        return key(std::string{k});
    }

    [[nodiscard]] PathBuilder operator/(const char* k) && {
        return std::move(*this).key(std::string{k});
    }

    /// Operator / for index (chainable)
    [[nodiscard]] PathBuilder operator/(std::size_t i) const & {
        return index(i);
    }

    [[nodiscard]] PathBuilder operator/(std::size_t i) && {
        return std::move(*this).index(i);
    }

    /// Operator / for int index (convenience, avoids ambiguity)
    [[nodiscard]] PathBuilder operator/(int i) const & {
        return index(static_cast<std::size_t>(i));
    }

    [[nodiscard]] PathBuilder operator/(int i) && {
        return std::move(*this).index(static_cast<std::size_t>(i));
    }

    /// Convert to runtime Path
    [[nodiscard]] const Path& path() const noexcept { return path_; }
    [[nodiscard]] Path& path() noexcept { return path_; }

    /// Convert to type-erased lens (uses caching)
    [[nodiscard]] LagerValueLens to_lens() const {
        return lager_path_lens(path_);
    }

    /// Get value at this path
    [[nodiscard]] Value get(const Value& root) const {
        return lager::view(to_lens(), root);
    }

    /// Set value at this path (returns new root)
    [[nodiscard]] Value set(const Value& root, Value new_val) const {
        return lager::set(to_lens(), root, std::move(new_val));
    }

    /// Update value at this path with a function
    template<typename Fn>
    [[nodiscard]] Value over(const Value& root, Fn&& fn) const {
        return lager::over(to_lens(), root, std::forward<Fn>(fn));
    }

    /// Check if path is empty (root path)
    [[nodiscard]] bool empty() const noexcept { return path_.empty(); }

    /// Get path depth
    [[nodiscard]] std::size_t depth() const noexcept { return path_.size(); }

    /// Concatenate two paths
    [[nodiscard]] PathBuilder concat(const PathBuilder& other) const {
        PathBuilder result = *this;
        for (const auto& elem : other.path_) {
            result.path_.push_back(elem);
        }
        return result;
    }

    /// Get parent path (removes last segment)
    [[nodiscard]] PathBuilder parent() const {
        if (path_.empty()) return *this;
        PathBuilder result;
        result.path_.assign(path_.begin(), path_.end() - 1);
        return result;
    }

    /// Convert to string representation
    [[nodiscard]] std::string to_string() const {
        return path_to_string(path_);
    }

    /// Equality comparison
    bool operator==(const PathBuilder& other) const {
        return path_ == other.path_;
    }

    bool operator!=(const PathBuilder& other) const {
        return path_ != other.path_;
    }

    /// Compare with raw Path
    bool operator==(const Path& other) const {
        return path_ == other;
    }

    bool operator!=(const Path& other) const {
        return path_ != other;
    }

private:
    Path path_;
};

/// Root path constant for starting chains
inline const PathBuilder root;

/// Create a path from variadic arguments
/// Usage: auto p = make_path("users", 0, "name");
template<PathElementType... Elements>
[[nodiscard]] PathBuilder make_path(Elements&&... elements) {
    PathBuilder builder;
    (builder.path().push_back(
        [](auto&& e) -> PathElement {
            using T = std::decay_t<decltype(e)>;
            if constexpr (StringLike<T>) {
                return std::string{std::forward<decltype(e)>(e)};
            } else {
                return static_cast<std::size_t>(e);
            }
        }(std::forward<Elements>(elements))
    ), ...);
    return builder;
}

// ============================================================
// Convenience free functions using PathBuilder
// ============================================================

/// Get value at path (variadic arguments)
template<PathElementType... Elements>
[[nodiscard]] Value get_at(const Value& root, Elements&&... path_elements) {
    return make_path(std::forward<Elements>(path_elements)...).get(root);
}

/// Set value at path (variadic arguments)
template<PathElementType... Elements>
[[nodiscard]] Value set_at(const Value& root, Value new_val, Elements&&... path_elements) {
    return make_path(std::forward<Elements>(path_elements)...).set(root, std::move(new_val));
}

/// Update value at path with function (variadic arguments)
template<typename Fn, PathElementType... Elements>
[[nodiscard]] Value over_at(const Value& root, Fn&& fn, Elements&&... path_elements) {
    return make_path(std::forward<Elements>(path_elements)...).over(root, std::forward<Fn>(fn));
}

// ============================================================
// Path Access Result (Structured Error Reporting)
//
// Provides detailed information about path access operations,
// including success/failure status, error messages, and
// partially resolved paths.
// ============================================================

/// Error codes for path access operations
enum class PathErrorCode {
    Success = 0,
    KeyNotFound,        // Map key doesn't exist
    IndexOutOfRange,    // Vector/array index out of bounds
    TypeMismatch,       // Expected container type, got primitive
    NullValue,          // Attempted access on null value
    EmptyPath,          // Path is empty (not an error, just info)
};

/// Detailed result of a path access operation
struct PathAccessResult {
    Value value;                    // The accessed value (or null on error)
    bool success = false;           // Whether the access succeeded
    PathErrorCode error_code = PathErrorCode::Success;
    std::string error_message;      // Human-readable error description
    Path resolved_path;             // The portion of path that was successfully resolved
    std::size_t failed_at_index = 0; // Index in path where access failed (if failed)

    /// Check if access was successful
    explicit operator bool() const noexcept { return success; }

    /// Get the value (throws if not successful)
    const Value& get() const {
        if (!success) {
            throw std::runtime_error("Path access failed: " + error_message);
        }
        return value;
    }

    /// Get the value or a default
    Value get_or(Value default_val) const {
        return success ? value : std::move(default_val);
    }
};

/// Access value at path with detailed error reporting
/// @param root The root value
/// @param path The path to access
/// @return PathAccessResult with detailed information
[[nodiscard]] PathAccessResult get_at_path_safe(const Value& root, const Path& path);

/// Set value at path with detailed error reporting
/// @param root The root value
/// @param path The path to access
/// @param new_val The new value to set
/// @return PathAccessResult containing the new root (or original on error)
[[nodiscard]] PathAccessResult set_at_path_safe(const Value& root, const Path& path, Value new_val);

// ============================================================
// HashedPath - Path with pre-computed hash
//
// For frequent cache lookups, pre-computing the hash value
// avoids redundant computation on each access.
// ============================================================

class HashedPath {
public:
    /// Create from existing Path (computes hash immediately)
    explicit HashedPath(Path path)
        : path_(std::move(path))
        , hash_(compute_hash(path_))
    {}

    /// Create from path elements (variadic)
    template<PathElementType... Elements>
    explicit HashedPath(Elements&&... elements)
        : path_{}
        , hash_(0)
    {
        (path_.push_back(convert_element(std::forward<Elements>(elements))), ...);
        hash_ = compute_hash(path_);
    }

    /// Get the underlying path
    [[nodiscard]] const Path& path() const noexcept { return path_; }

    /// Get the pre-computed hash
    [[nodiscard]] std::size_t hash() const noexcept { return hash_; }

    /// Convert to lens (using cached lookup)
    [[nodiscard]] LagerValueLens to_lens() const {
        return lager_path_lens(path_);
    }

    /// Equality comparison with HashedPath
    bool operator==(const HashedPath& other) const {
        return hash_ == other.hash_ && path_ == other.path_;
    }

    bool operator!=(const HashedPath& other) const {
        return !(*this == other);
    }

    /// Equality comparison with raw Path
    bool operator==(const Path& other) const {
        return path_ == other;
    }

    bool operator!=(const Path& other) const {
        return path_ != other;
    }

    /// Equality comparison with PathBuilder
    bool operator==(const PathBuilder& other) const {
        return path_ == other.path();
    }

    bool operator!=(const PathBuilder& other) const {
        return path_ != other.path();
    }

    /// Append element (returns new HashedPath)
    [[nodiscard]] HashedPath operator/(const std::string& key) const {
        Path new_path = path_;
        new_path.push_back(key);
        return HashedPath{std::move(new_path)};
    }

    [[nodiscard]] HashedPath operator/(const char* key) const {
        return *this / std::string{key};
    }

    [[nodiscard]] HashedPath operator/(std::size_t index) const {
        Path new_path = path_;
        new_path.push_back(index);
        return HashedPath{std::move(new_path)};
    }

    [[nodiscard]] HashedPath operator/(int index) const {
        return *this / static_cast<std::size_t>(index);
    }

private:
    Path path_;
    std::size_t hash_;

    template<typename T>
    static PathElement convert_element(T&& elem) {
        using U = std::decay_t<T>;
        if constexpr (StringLike<U>) {
            return std::string{std::forward<T>(elem)};
        } else {
            return static_cast<std::size_t>(elem);
        }
    }

    static std::size_t compute_hash(const Path& path) {
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
            // FNV-1a style mixing
            hash ^= elem_hash + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

/// Hash functor for HashedPath (uses pre-computed hash)
struct HashedPathHash {
    std::size_t operator()(const HashedPath& hp) const noexcept {
        return hp.hash();  // O(1) - already computed!
    }
};

/// Create a hashed path from variadic arguments
template<PathElementType... Elements>
[[nodiscard]] HashedPath make_hashed_path(Elements&&... elements) {
    return HashedPath{std::forward<Elements>(elements)...};
}

// ============================================================
// Demo function
// ============================================================
void demo_lager_lens();

} // namespace lager_ext
