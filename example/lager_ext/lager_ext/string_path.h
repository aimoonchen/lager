// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file string_path.h
/// @brief String-based path parsing and access API.
///
/// Provides a familiar slash-separated path syntax for accessing nested data:
///   "/users/0/name"  ->  data["users"][0]["name"]
///
/// Path syntax follows RFC 6901 (JSON Pointer) conventions:
/// - Paths start with "/" (root reference)
/// - Segments separated by "/"
/// - Numeric segments (e.g., "0", "123") are treated as array indices
/// - Escape sequences: "~0" -> "~", "~1" -> "/"
/// - Empty path "" refers to the whole document

#pragma once

#include <lager_ext/value.h>
#include <lager_ext/lager_lens.h>
#include <string_view>

namespace lager_ext {

// ============================================================
// String path parsing and conversion
// ============================================================

/// Parse slash-separated path string into Path
/// @param path_str The path string (e.g., "/users/0/name")
/// @return Parsed Path object
/// @example
///   "/users/0/name"  -> ["users", 0, "name"]
///   "/config/theme"  -> ["config", "theme"]
///   ""               -> []  (root)
///   "/"              -> [""]  (key is empty string)
[[nodiscard]] Path parse_string_path(std::string_view path_str);

/// Convert Path back to slash-separated string
/// Useful for debugging and error messages
/// @param path The Path to convert
/// @return String representation (e.g., "/users/0/name")
[[nodiscard]] std::string path_to_string_path(const Path& path);

// ============================================================
// String path lens functions
// ============================================================

/// Build lens from path string
/// @param path_str The path string (e.g., "/users/0/name")
/// @return lager::lens<Value, Value> for use with lager::view/set/over
[[nodiscard]] LagerValueLens string_path_lens(std::string_view path_str);

// ============================================================
// Convenience functions
// ============================================================

/// Get value by path string
/// @param data The root Value
/// @param path_str The path string
/// @return The value at path, or null Value if not found
[[nodiscard]] Value get_by_path(const Value& data, std::string_view path_str);

/// Set value by path string
/// @param data The root Value
/// @param path_str The path string
/// @param new_value The new value to set
/// @return New immutable Value with the change applied
[[nodiscard]] Value set_by_path(const Value& data, std::string_view path_str, Value new_value);

/// Update value by path string with a function
/// @param data The root Value
/// @param path_str The path string
/// @param fn Function to transform the value
/// @return New immutable Value with the update applied
template<typename Fn>
[[nodiscard]] Value over_by_path(const Value& data, std::string_view path_str, Fn&& fn)
{
    auto lens = string_path_lens(path_str);
    return lager::over(lens, data, std::forward<Fn>(fn));
}

// ============================================================
// Demo function
// ============================================================
void demo_string_path();

} // namespace lager_ext
