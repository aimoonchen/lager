// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file erased_lens.h
/// @brief Scheme 1: Custom type-erased lens using std::function.
///
/// This approach implements type erasure manually using std::function,
/// allowing dynamic lens composition at runtime for JSON-like data.

#pragma once

#include <lager_ext/value.h>
#include <functional>

namespace lager_ext {

// ============================================================
// InlineLens - Lightweight lens without std::function overhead
//
// For performance-critical code paths, InlineLens avoids the
// std::function indirection by storing the getter/setter directly
// as template parameters. This enables inlining and avoids heap
// allocations for small lambdas.
// ============================================================

template<typename Getter, typename Setter>
class InlineLens {
public:
    constexpr InlineLens(Getter g, Setter s)
        : getter_(std::move(g)), setter_(std::move(s)) {}

    [[nodiscard]] Value get(const Value& v) const { return getter_(v); }
    [[nodiscard]] Value set(Value whole, Value part) const {
        return setter_(std::move(whole), std::move(part));
    }

    template<typename Fn>
    [[nodiscard]] Value over(Value whole, Fn&& fn) const {
        return set(whole, std::forward<Fn>(fn)(get(whole)));
    }

    /// Compose with another InlineLens
    /// Uses public interface (get/set) for better encapsulation
    template<typename G2, typename S2>
    [[nodiscard]] auto compose(const InlineLens<G2, S2>& inner) const {
        // Capture copies of both lenses
        auto outer = *this;
        auto inner_copy = inner;

        return make_inline_lens(
            [outer, inner_copy](const Value& v) {
                return inner_copy.get(outer.get(v));
            },
            [outer, inner_copy](Value whole, Value new_val) {
                auto outer_part = outer.get(whole);
                auto new_outer = inner_copy.set(std::move(outer_part), std::move(new_val));
                return outer.set(std::move(whole), std::move(new_outer));
            }
        );
    }

private:
    template<typename G, typename S> friend auto make_inline_lens(G, S);

    Getter getter_;
    Setter setter_;
};

/// Factory function for InlineLens with type deduction
template<typename Getter, typename Setter>
[[nodiscard]] auto make_inline_lens(Getter g, Setter s) {
    return InlineLens<Getter, Setter>(std::move(g), std::move(s));
}

/// Identity InlineLens
inline auto inline_identity_lens() {
    return make_inline_lens(
        [](const Value& v) { return v; },
        [](Value, Value v) { return v; }
    );
}

// ============================================================
// ErasedLens class (Optimized)
//
// A type-erased lens that uses std::function for get/set operations.
// Supports dynamic path composition via the compose() method and | operator.
//
// Optimizations:
// 1. Path-based lens uses direct traversal instead of nested composition
// 2. Provides conversion from InlineLens for type erasure when needed
// 3. Caches getter_/setter_ to avoid repeated function object creation
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

    // Construct from InlineLens (type-erase an inline lens)
    template<typename G, typename S>
    ErasedLens(const InlineLens<G, S>& inline_lens)
        : getter_([inline_lens](const Value& v) { return inline_lens.get(v); })
        , setter_([inline_lens](Value w, Value p) { return inline_lens.set(std::move(w), std::move(p)); })
    {}

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

} // namespace lager_ext
