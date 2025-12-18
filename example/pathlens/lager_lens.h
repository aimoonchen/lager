// lager_lens.h
// Scheme 2: Using lager::lens<Value, Value> for type erasure
//
// This approach uses lager's built-in type-erased lens instead of
// custom ErasedLens. Both approaches work for dynamic JSON-like data.
//
// Key insight: Since our data model is Value -> Value -> Value...,
// we can use lager::lens<Value, Value> as a uniform type for
// dynamically composed lenses.

#pragma once

#include "value.h"
#include <lager/lens.hpp>
#include <lager/lenses.hpp>
#include <zug/compose.hpp>

namespace immer_lens {

// Type alias for lager's type-erased lens with Value as both Whole and Part
using LagerValueLens = lager::lens<Value, Value>;

// ============================================================
// Lens factory functions
// ============================================================

// Create a getset lens that focuses on a map key
auto key_lens(const std::string& key);

// Create a getset lens that focuses on a vector index
auto index_lens(std::size_t index);

// Create a type-erased lens for a map key
LagerValueLens lager_key_lens(const std::string& key);

// Create a type-erased lens for a vector index
LagerValueLens lager_index_lens(std::size_t index);

// Build a type-erased lens from a path
LagerValueLens lager_path_lens(const Path& path);

// ============================================================
// Static path lens using fold expression
// Use when path elements are known at compile time
// ============================================================

namespace detail {

template <typename T>
auto element_to_lens(T&& elem)
{
    if constexpr (std::is_convertible_v<T, std::string> ||
                  std::is_same_v<std::decay_t<T>, const char*>) {
        return key_lens(std::string{elem});
    } else if constexpr (std::is_integral_v<std::decay_t<T>>) {
        return index_lens(static_cast<std::size_t>(elem));
    }
}

} // namespace detail

template <typename... Elements>
auto static_path_lens(Elements&&... elements)
{
    return (zug::identity | ... | detail::element_to_lens(std::forward<Elements>(elements)));
}

// ============================================================
// Demo function
// ============================================================
void demo_lager_lens();

} // namespace immer_lens
