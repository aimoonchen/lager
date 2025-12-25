// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file static_path.h
/// @brief Compile-time static path lens implementation (C++20).
///
/// This file provides a zero-overhead abstraction for path-based lens access
/// when the data structure is known at compile time (e.g., from C++ reflection).
///
/// Key features:
/// - Paths are constructed at compile time
/// - Lens composition is resolved at compile time
/// - Zero runtime overhead for path construction
/// - Type-safe path definitions
/// - JSON Pointer style path syntax support

#pragma once

#include <lager_ext/value.h>
#include <lager/lenses.hpp>
#include <zug/compose.hpp>

#include <algorithm>
#include <array>
#include <string_view>
#include <type_traits>
#include <utility>

namespace lager_ext {
namespace static_path {

// ============================================================
// Fixed-size string for non-type template parameters (C++20 NTTP)
// ============================================================

template<std::size_t N>
struct FixedString {
    char data[N]{};

    constexpr FixedString() = default;

    constexpr FixedString(const char (&str)[N]) {
        std::copy_n(str, N, data);
    }

    constexpr std::size_t size() const noexcept { return N - 1; }
    constexpr const char* c_str() const noexcept { return data; }
    constexpr std::string_view view() const noexcept { return {data, N - 1}; }
    constexpr operator std::string_view() const noexcept { return view(); }
    std::string to_string() const { return std::string{view()}; }

    // Comparison operators for compile-time path comparison
    template<std::size_t M>
    constexpr bool operator==(const FixedString<M>& other) const noexcept {
        return view() == other.view();
    }

    constexpr bool operator==(std::string_view sv) const noexcept {
        return view() == sv;
    }
};

// Deduction guide
template<std::size_t N>
FixedString(const char (&)[N]) -> FixedString<N>;

// ============================================================
// Compile-time path segment types
// ============================================================

// Key segment - for map access
template<FixedString Key>
struct KeySeg {
    static constexpr auto key = Key;
    static constexpr bool is_key = true;
    static constexpr bool is_index = false;

    static std::string key_string() { return Key.to_string(); }
};

// Index segment - for vector access
template<std::size_t Index>
struct IndexSeg {
    static constexpr std::size_t index = Index;
    static constexpr bool is_key = false;
    static constexpr bool is_index = true;
};

// ============================================================
// Static Lens - Compile-time lens for a single path segment
// ============================================================

// Key lens - accesses a map by key at compile time
template<FixedString Key>
struct StaticKeyLens {
    static constexpr auto key = Key;

    Value get(const Value& whole) const {
        return whole.at(key.to_string());
    }

    Value set(Value whole, const Value& part) const {
        return whole.set(key.to_string(), part);
    }
};

// Index lens - accesses a vector by index at compile time
template<std::size_t Index>
struct StaticIndexLens {
    static constexpr std::size_t index = Index;

    Value get(const Value& whole) const {
        return whole.at(index);
    }

    Value set(Value whole, const Value& part) const {
        return whole.set(index, part);
    }
};

// ============================================================
// Composed Static Lens - Zero-overhead lens composition
// ============================================================

// Forward declaration with variadic template
template<typename... Lenses>
struct ComposedLens;

// Base case: single lens
template<typename Lens>
struct ComposedLens<Lens> {
    Lens lens;

    constexpr ComposedLens() = default;
    constexpr explicit ComposedLens(Lens l) : lens(std::move(l)) {}

    Value get(const Value& v) const {
        return lens.get(v);
    }

    Value set(Value v, const Value& x) const {
        return lens.set(std::move(v), x);
    }
};

// Recursive case: lens composition
template<typename First, typename... Rest>
struct ComposedLens<First, Rest...> {
    First first;
    ComposedLens<Rest...> rest;

    constexpr ComposedLens() = default;

    constexpr ComposedLens(First f, Rest... r)
        : first(std::move(f)), rest(std::move(r)...) {}

    Value get(const Value& v) const {
        auto inner = first.get(v);
        return rest.get(inner);
    }

    Value set(Value v, const Value& x) const {
        auto inner = first.get(v);
        auto new_inner = rest.set(std::move(inner), x);
        return first.set(std::move(v), new_inner);
    }
};

// Empty case (identity lens)
template<>
struct ComposedLens<> {
    constexpr ComposedLens() = default;

    Value get(const Value& v) const {
        return v;
    }

    Value set(Value, const Value& x) const {
        return x;
    }
};

// ============================================================
// Static Path - Compile-time path definition
// ============================================================

template<typename... Segments>
struct StaticPath {
    static constexpr std::size_t depth = sizeof...(Segments);

    // Convert to composed lens at compile time
    static auto to_lens() {
        return make_lens_impl(std::index_sequence_for<Segments...>{});
    }

    // Get value using this path
    static Value get(const Value& v) {
        return to_lens().get(v);
    }

    // Set value using this path
    static Value set(Value v, const Value& x) {
        return to_lens().set(std::move(v), x);
    }

    // Convert to runtime Path for compatibility
    static Path to_runtime_path() {
        Path result;
        (add_segment<Segments>(result), ...);
        return result;
    }

private:
    template<typename Seg>
    static void add_segment(Path& path) {
        if constexpr (Seg::is_key) {
            path.push_back(Seg::key_string());
        } else {
            path.push_back(Seg::index);
        }
    }

    template<std::size_t... Is>
    static auto make_lens_impl(std::index_sequence<Is...>) {
        return ComposedLens<decltype(segment_to_lens<Segments>())...>{
            segment_to_lens<Segments>()...
        };
    }

    template<typename Seg>
    static auto segment_to_lens() {
        if constexpr (Seg::is_key) {
            return StaticKeyLens<Seg::key>{};
        } else {
            return StaticIndexLens<Seg::index>{};
        }
    }
};

// Empty path specialization (identity lens)
template<>
struct StaticPath<> {
    static constexpr std::size_t depth = 0;

    static Value get(const Value& v) { return v; }
    static Value set(Value, const Value& x) { return x; }
    static Path to_runtime_path() { return {}; }
};

// ============================================================
// Convenience type aliases and factory functions
// ============================================================

// Create a key segment (use K for brevity to avoid name collision)
template<FixedString S>
using K = KeySeg<S>;

// Create an index segment (use I for brevity)
template<std::size_t N>
using I = IndexSeg<N>;

// Create a static path from segments
template<typename... Segments>
using MakePath = StaticPath<Segments...>;

// ============================================================
// Path concatenation - combine two static paths
// ============================================================

template<typename Path1, typename Path2>
struct ConcatPath;

template<typename... Segs1, typename... Segs2>
struct ConcatPath<StaticPath<Segs1...>, StaticPath<Segs2...>> {
    using type = StaticPath<Segs1..., Segs2...>;
};

template<typename Path1, typename Path2>
using ConcatPathT = typename ConcatPath<Path1, Path2>::type;

// ============================================================
// Path extension - add a segment to an existing path
// ============================================================

template<typename BasePath, typename Segment>
struct ExtendPath;

template<typename... Segs, typename Segment>
struct ExtendPath<StaticPath<Segs...>, Segment> {
    using type = StaticPath<Segs..., Segment>;
};

template<typename BasePath, typename Segment>
using ExtendPathT = typename ExtendPath<BasePath, Segment>::type;

// ============================================================
// Macro helpers for defining paths
// ============================================================

// Define a key segment
#define STATIC_KEY(name) \
    lager_ext::static_path::KeySeg<lager_ext::static_path::FixedString{name}>

// Define an index segment
#define STATIC_IDX(n) \
    lager_ext::static_path::IndexSeg<n>

// Define a complete path
#define STATIC_PATH(...) \
    lager_ext::static_path::StaticPath<__VA_ARGS__>

// ============================================================
// PathRegistry - For organizing paths by schema
// ============================================================

template<typename Schema>
struct PathRegistry {
    // Override this in derived types to define paths
};

// ============================================================
// JSON Pointer Style Static Path
//
// Allows defining paths using JSON Pointer syntax:
//   JsonPointerPath<"/users/0/name">
//
// This is equivalent to:
//   StaticPath<K<"users">, I<0>, K<"name">>
// ============================================================

namespace detail {

// Helper: check if string_view contains only digits
constexpr bool is_number(std::string_view sv) noexcept {
    if (sv.empty()) return false;
    for (char c : sv) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// Helper: convert string_view to number
constexpr std::size_t to_number(std::string_view sv) noexcept {
    std::size_t result = 0;
    for (char c : sv) {
        result = result * 10 + (c - '0');
    }
    return result;
}

// Count segments in a JSON Pointer
template<FixedString Ptr>
constexpr std::size_t count_segments() {
    std::string_view sv = Ptr.view();
    if (sv.empty()) return 0;

    std::size_t start = (sv[0] == '/') ? 1 : 0;
    if (start >= sv.size()) return 0;

    std::size_t count = 0;
    while (start < sv.size()) {
        ++count;
        auto next = sv.find('/', start);
        if (next == std::string_view::npos) break;
        start = next + 1;
    }
    return count;
}

// Extract segment at index as string_view
template<FixedString Ptr, std::size_t Index>
constexpr std::string_view get_segment() {
    std::string_view sv = Ptr.view();
    std::size_t start = (sv[0] == '/') ? 1 : 0;

    std::size_t current = 0;
    while (current < Index) {
        auto next = sv.find('/', start);
        start = next + 1;
        ++current;
    }

    auto end = sv.find('/', start);
    if (end == std::string_view::npos) end = sv.size();

    return sv.substr(start, end - start);
}

// Check if segment is a number
template<FixedString Ptr, std::size_t Index>
constexpr bool is_index_segment() {
    return is_number(get_segment<Ptr, Index>());
}

// Get segment as number
template<FixedString Ptr, std::size_t Index>
constexpr std::size_t get_index_value() {
    return to_number(get_segment<Ptr, Index>());
}

// Get segment length at compile time
template<FixedString Ptr, std::size_t Index>
constexpr std::size_t get_segment_length() {
    return get_segment<Ptr, Index>().size();
}

// Build FixedString from segment
template<FixedString Ptr, std::size_t Index, std::size_t Len>
struct SegmentString {
    static constexpr auto make() {
        constexpr auto seg = get_segment<Ptr, Index>();
        FixedString<Len + 1> result{};
        for (std::size_t i = 0; i < Len; ++i) {
            result.data[i] = seg[i];
        }
        result.data[Len] = '\0';
        return result;
    }
    static constexpr auto value = make();
};

template<FixedString Ptr, std::size_t Index>
constexpr auto make_segment_string() {
    constexpr std::size_t len = get_segment_length<Ptr, Index>();
    return SegmentString<Ptr, Index, len>::value;
}

// Build segment type at index
template<FixedString Ptr, std::size_t Index>
struct SegmentTypeAt {
    static constexpr bool is_idx = is_index_segment<Ptr, Index>();

    using type = std::conditional_t<
        is_idx,
        IndexSeg<get_index_value<Ptr, Index>()>,
        KeySeg<make_segment_string<Ptr, Index>()>
    >;
};

// Build StaticPath from segments
template<FixedString Ptr, typename Indices>
struct BuildPath;

template<FixedString Ptr, std::size_t... Is>
struct BuildPath<Ptr, std::index_sequence<Is...>> {
    using type = StaticPath<typename SegmentTypeAt<Ptr, Is>::type...>;
};

} // namespace detail

// Main template: Convert JSON Pointer to StaticPath
template<FixedString Ptr>
using JsonPointerPath = typename detail::BuildPath<
    Ptr,
    std::make_index_sequence<detail::count_segments<Ptr>()>
>::type;

} // namespace static_path

// ============================================================
// Demo function declaration
// ============================================================

void demo_static_path();

} // namespace lager_ext
