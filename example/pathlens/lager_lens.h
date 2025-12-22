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

#include "value.h"
#include <lager/lens.hpp>
#include <lager/lenses.hpp>
#include <zug/compose.hpp>

#include <concepts>
#include <type_traits>

namespace immer_lens {

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
// Demo function
// ============================================================
void demo_lager_lens();

} // namespace immer_lens
