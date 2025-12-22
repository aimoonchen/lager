// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file erased_lens.h
/// @brief Scheme 1: Custom type-erased lens using std::function.
///
/// This approach implements type erasure manually using std::function,
/// allowing dynamic lens composition at runtime for JSON-like data.

#pragma once

#include "value.h"
#include <functional>

namespace immer_lens {

// ============================================================
// ErasedLens class
// 
// A type-erased lens that uses std::function for get/set operations.
// Supports dynamic path composition via the compose() method and | operator.
// 
// Key features:
// 1. Uses std::function for type erasure (may benefit from SBO)
// 2. Supports | operator for composition (similar to zug::comp)
// 3. Provides over() operation for functional updates
// ============================================================
class ErasedLens
{
public:
    using Getter = std::function<Value(const Value&)>;
    using Setter = std::function<Value(Value, Value)>;

private:
    Getter getter_;
    Setter setter_;

public:
    // Identity lens (default constructor)
    ErasedLens();

    // Custom lens with provided get/set functions
    ErasedLens(Getter g, Setter s);

    // Get the focused value
    [[nodiscard]] Value get(const Value& v) const;
    
    // Set the focused value, returns updated whole
    [[nodiscard]] Value set(Value whole, Value part) const;
    
    // Update focused value using a function (over operation)
    template<typename Fn>
    [[nodiscard]] Value over(Value whole, Fn&& fn) const
    {
        auto current = getter_(whole);
        auto updated = std::forward<Fn>(fn)(std::move(current));
        return setter_(std::move(whole), std::move(updated));
    }

    // Compose with inner lens (this -> inner)
    [[nodiscard]] ErasedLens compose(const ErasedLens& inner) const;
    
    // Composition operator: lhs | rhs composes lenses left-to-right
    // 
    // Example:
    //   auto lens = make_key_lens("users") | make_index_lens(0) | make_key_lens("name");
    //   // Equivalent to: path_lens({"users", 0, "name"})
    //   // Access: data["users"][0]["name"]
    //
    // Note: This follows the same convention as zug::comp / operator|
    friend ErasedLens operator|(const ErasedLens& lhs, const ErasedLens& rhs);
};

// ============================================================
// Lens factory functions
// ============================================================

// Create a lens that focuses on a map key
[[nodiscard]] ErasedLens make_key_lens(const std::string& key);

// Create a lens that focuses on a vector index
[[nodiscard]] ErasedLens make_index_lens(std::size_t index);

// Build a composed lens from a path
[[nodiscard]] ErasedLens path_lens(const Path& path);

// ============================================================
// Demo function
// ============================================================
void demo_erased_lens();

} // namespace immer_lens
