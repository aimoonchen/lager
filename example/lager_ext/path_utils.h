// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file path_utils.h
/// @brief Common path traversal utilities shared by lens implementations.
///
/// This file provides low-level path traversal functions used by both
/// ErasedLens and LagerValueLens implementations, avoiding code duplication.

#pragma once

#include "value.h"

namespace lager_ext {

// ============================================================
// Path Element Access
//
// These inline functions provide direct access to Value elements
// by key or index, used by all lens implementations.
// ============================================================

/// Get value at a single path element (key or index)
/// @param current The current value (should be a container)
/// @param elem The path element (string key or size_t index)
/// @return The accessed value, or monostate if not found
[[nodiscard]] inline Value get_at_path_element(const Value& current, const PathElement& elem)
{
    return std::visit([&current](const auto& key_or_idx) -> Value {
        using T = std::decay_t<decltype(key_or_idx)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return current.at(key_or_idx);
        } else {
            return current.at(key_or_idx);
        }
    }, elem);
}

/// Set value at a single path element (key or index)
/// @param current The current container value
/// @param elem The path element
/// @param new_val The new value to set
/// @return New container with the element updated
[[nodiscard]] inline Value set_at_path_element(const Value& current, const PathElement& elem, Value new_val)
{
    return std::visit([&current, &new_val](const auto& key_or_idx) -> Value {
        using T = std::decay_t<decltype(key_or_idx)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return current.set(key_or_idx, std::move(new_val));
        } else {
            return current.set(key_or_idx, std::move(new_val));
        }
    }, elem);
}

// ============================================================
// Direct Path Traversal
//
// High-performance path traversal without lens composition overhead.
// These functions directly walk the data structure.
// ============================================================

/// Get value at a full path using direct traversal
/// @param root The root value
/// @param path The path to traverse
/// @return The value at the path, or monostate if any step fails
[[nodiscard]] inline Value get_at_path_direct(const Value& root, const Path& path)
{
    Value current = root;
    for (const auto& elem : path) {
        current = get_at_path_element(current, elem);
        if (current.is_null()) {
            break;  // Early exit on null
        }
    }
    return current;
}

/// Set value at a path using recursive traversal
/// @param root The root value
/// @param path The full path
/// @param path_index Current position in path (0 = start)
/// @param new_val The new value to set
/// @return New root with the update applied
[[nodiscard]] inline Value set_at_path_recursive(
    const Value& root,
    const Path& path,
    std::size_t path_index,
    Value new_val)
{
    if (path_index >= path.size()) {
        return new_val;  // Base case: replace current node
    }

    const auto& elem = path[path_index];
    Value current_child = get_at_path_element(root, elem);
    Value new_child = set_at_path_recursive(current_child, path, path_index + 1, std::move(new_val));
    return set_at_path_element(root, elem, std::move(new_child));
}

/// Set value at a full path using direct traversal
/// @param root The root value
/// @param path The path to the value
/// @param new_val The new value to set
/// @return New root with the update applied
[[nodiscard]] inline Value set_at_path_direct(const Value& root, const Path& path, Value new_val)
{
    if (path.empty()) {
        return new_val;
    }
    return set_at_path_recursive(root, path, 0, std::move(new_val));
}

// ============================================================
// Path Erasure Utilities
// ============================================================

/// Erase a key from a map value
/// @param val The value (should be a ValueMap)
/// @param key The key to erase
/// @return New map with the key removed, or original value if not a map
[[nodiscard]] inline Value erase_key_from_map(const Value& val, const std::string& key)
{
    if (auto* m = val.get_if<ValueMap>()) {
        return m->erase(key);
    }
    return val;
}

/// Erase value at a full path
/// For maps: actually erases the key
/// For vectors/arrays: sets to null (cannot shrink without reindexing)
/// @param root The root value
/// @param path The path to the value to erase
/// @return New root with the element erased
[[nodiscard]] inline Value erase_at_path_direct(const Value& root, const Path& path)
{
    if (path.empty()) {
        return Value{};  // Erase entire root
    }

    // If last element is a string key, we can erase from map
    if (path.size() == 1) {
        if (auto* key = std::get_if<std::string>(&path[0])) {
            return erase_key_from_map(root, *key);
        }
        // For index, set to null (cannot truly remove without reindexing)
        return set_at_path_direct(root, path, Value{});
    }

    // Navigate to parent and erase from there
    Path parent_path;
    parent_path.reserve(path.size() - 1);
    for (std::size_t i = 0; i < path.size() - 1; ++i) {
        parent_path.push_back(path[i]);
    }
    const auto& last = path.back();

    if (auto* key = std::get_if<std::string>(&last)) {
        // Get parent, erase key from it, set parent back
        Value parent = get_at_path_direct(root, parent_path);
        Value new_parent = erase_key_from_map(parent, *key);
        return set_at_path_direct(root, parent_path, new_parent);
    }

    // For index removal, set to null
    return set_at_path_direct(root, path, Value{});
}

// ============================================================
// Path Validation Utilities
// ============================================================

/// Check if a path element can be accessed in the given value
/// @param val The value to check
/// @param elem The path element to access
/// @return true if the element can be accessed
[[nodiscard]] inline bool can_access(const Value& val, const PathElement& elem)
{
    return std::visit([&val](const auto& key_or_idx) -> bool {
        using T = std::decay_t<decltype(key_or_idx)>;
        if constexpr (std::is_same_v<T, std::string>) {
            // For map access, check if it's a map and has the key
            if (const auto* map = val.get_if<ValueMap>()) {
                return map->count(key_or_idx) > 0;
            }
            return false;
        } else {
            // For index access, check if it's a vector/array with valid index
            if (const auto* vec = val.get_if<ValueVector>()) {
                return key_or_idx < vec->size();
            }
            if (const auto* arr = val.get_if<ValueArray>()) {
                return key_or_idx < arr->size();
            }
            return false;
        }
    }, elem);
}

/// Check if an entire path can be traversed
/// @param root The root value
/// @param path The path to check
/// @return true if all elements in the path can be accessed
[[nodiscard]] inline bool is_valid_path(const Value& root, const Path& path)
{
    Value current = root;
    for (const auto& elem : path) {
        if (!can_access(current, elem)) {
            return false;
        }
        current = get_at_path_element(current, elem);
    }
    return true;
}

/// Get the depth of valid traversal for a path
/// @param root The root value
/// @param path The path to check
/// @return Number of path elements that can be successfully traversed
[[nodiscard]] inline std::size_t valid_path_depth(const Value& root, const Path& path)
{
    Value current = root;
    std::size_t depth = 0;
    for (const auto& elem : path) {
        if (!can_access(current, elem)) {
            break;
        }
        current = get_at_path_element(current, elem);
        ++depth;
    }
    return depth;
}

} // namespace lager_ext
