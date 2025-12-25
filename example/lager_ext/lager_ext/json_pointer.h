// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file json_pointer.h
/// @brief Scheme 4: JSON Pointer (RFC 6901) style API.
///
/// Provides a familiar JSON-like path syntax for accessing nested data:
///   "/users/0/name"  ->  data["users"][0]["name"]
///
/// RFC 6901: https://datatracker.ietf.org/doc/html/rfc6901
///
/// Key features:
/// - Paths start with "/" (root reference)
/// - Segments separated by "/"
/// - Numeric segments (e.g., "0", "123") are treated as array indices
/// - Escape sequences: "~0" -> "~", "~1" -> "/"
/// - Empty pointer "" refers to the whole document

#pragma once

#include <lager_ext/value.h>
#include <lager_ext/lager_lens.h>
#include <string_view>

namespace lager_ext {

// ============================================================
// JSON Pointer parsing and conversion
// ============================================================

// Parse JSON Pointer string into Path
// Examples:
//   "/users/0/name"  -> ["users", 0, "name"]
//   "/config/theme"  -> ["config", "theme"]
//   ""               -> []  (root)
//   "/"              -> [""]  (key is empty string)
[[nodiscard]] Path parse_json_pointer(std::string_view pointer);

// Convert Path back to JSON Pointer string
// Useful for debugging and error messages
[[nodiscard]] std::string path_to_json_pointer(const Path& path);

// ============================================================
// JSON Pointer lens functions
// ============================================================

// Build lens from JSON Pointer string
// Returns lager::lens<Value, Value> for use with lager::view/set/over
[[nodiscard]] LagerValueLens json_pointer_lens(std::string_view pointer);

// ============================================================
// Convenience functions
// ============================================================

// Get value by JSON Pointer
// Returns null Value if path not found
[[nodiscard]] Value get_by_pointer(const Value& data, std::string_view pointer);

// Set value by JSON Pointer
// Returns new immutable Value with the change applied
[[nodiscard]] Value set_by_pointer(const Value& data, std::string_view pointer, Value new_value);

// Update value by JSON Pointer with a function
template<typename Fn>
[[nodiscard]] Value over_by_pointer(const Value& data, std::string_view pointer, Fn&& fn)
{
    auto lens = json_pointer_lens(pointer);
    return lager::over(lens, data, std::forward<Fn>(fn));
}

// ============================================================
// Demo function
// ============================================================
void demo_json_pointer();

} // namespace lager_ext
