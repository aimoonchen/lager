// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file diff_collector.h
/// @brief Recursive diff collector for detecting changes between Value states.
///
/// Uses immer's structural sharing for efficient O(1) unchanged subtree skipping.

#pragma once

#include <lager_ext/value.h>
#include <vector>

namespace lager_ext {

// ============================================================
// DiffEntry structure - records a single change
// ============================================================
struct DiffEntry {
    enum class Type { Add, Remove, Change };

    Type type;
    Path path;              // Path to the changed value
    Value old_value;        // Old value (null for Add)
    Value new_value;        // New value (null for Remove)

    // Constructor for emplace_back optimization
    DiffEntry(Type t, const Path& p, const Value& old_v, const Value& new_v)
        : type(t), path(p), old_value(old_v), new_value(new_v) {}

    // Default constructor
    DiffEntry() = default;
};

// ============================================================
// DiffCollector
//
// Collects differences between two Value states.
//
// Supports two modes:
// - Recursive mode (default): Collects all nested differences down to leaf values
// - Shallow mode: Only collects top-level differences (changed containers reported as single entry)
//
// Optimizations:
// 1. Uses immer::box pointer comparison for O(1) unchanged subtree skipping
// 2. Uses immer::diff for map comparisons (efficient for structural sharing)
// 3. Uses index iteration for vectors (immer::diff only supports map/set)
// 4. Pre-allocates memory to reduce reallocations
// 5. Uses emplace_back for in-place construction
// ============================================================
class DiffCollector {
private:
    std::vector<DiffEntry> diffs_;
    bool recursive_ = true;  // Default: recursive mode

    // OPTIMIZATION: Pass Path by reference to avoid copying.
    // We use push_back/pop_back pattern instead of creating new Path objects.
    void diff_value(const Value& old_val, const Value& new_val, Path& current_path);
    void diff_map(const ValueMap& old_map, const ValueMap& new_map, Path& current_path);
    void diff_vector(const ValueVector& old_vec, const ValueVector& new_vec, Path& current_path);
    void collect_entries(const Value& val, Path& current_path, bool is_add);
    void collect_removed(const Value& val, Path& current_path);
    void collect_added(const Value& val, Path& current_path);

public:
    // Main entry point: compare two Values
    // @param recursive: true = collect all nested diffs, false = only top-level diffs
    void diff(const Value& old_val, const Value& new_val, bool recursive = true);

    // Access results
    [[nodiscard]] const std::vector<DiffEntry>& get_diffs() const;

    // Clear collected diffs
    void clear();

    // Check if there are any changes (after calling diff())
    [[nodiscard]] bool has_changes() const;

    // Check if last diff was recursive
    [[nodiscard]] bool is_recursive() const { return recursive_; }

    // Print diffs to stdout
    void print_diffs() const;
};

// Backward compatibility alias
using RecursiveDiffCollector = DiffCollector;

// ============================================================
// Quick change detection (early exit optimization)
//
// For scenarios where you only need to know IF there are changes,
// not WHAT the changes are. Returns immediately upon finding
// the first difference - O(1) best case, O(changed nodes) worst case.
//
// Supports two modes:
// - Recursive (default): Deep comparison, checks all nested levels
// - Shallow: Only checks top-level, containers compared by identity
// ============================================================

// Check if two Values have any differences (early exit on first difference)
// @param recursive: true = deep comparison (default), false = shallow comparison
[[nodiscard]] bool has_any_difference(const Value& old_val, const Value& new_val, bool recursive = true);

// Internal helper functions for early-exit detection
namespace detail {
    [[nodiscard]] bool values_differ(const Value& old_val, const Value& new_val, bool recursive);
    [[nodiscard]] bool maps_differ(const ValueMap& old_map, const ValueMap& new_map, bool recursive);
    [[nodiscard]] bool vectors_differ(const ValueVector& old_vec, const ValueVector& new_vec, bool recursive);
}

// ============================================================
// Demo functions
// ============================================================

// Demo: Basic immer::diff usage
void demo_immer_diff();

// Demo: Full recursive diff collection
void demo_recursive_diff_collector();

} // namespace lager_ext
