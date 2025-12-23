// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file diff_collector.h
/// @brief Recursive diff collector for detecting changes between Value states.
///
/// Uses immer's structural sharing for efficient O(1) unchanged subtree skipping.

#pragma once

#include "value.h"
#include <vector>

namespace immer_lens {

// ============================================================
// DiffEntry structure - records a single change
// ============================================================
struct DiffEntry {
    enum class Type { Add, Remove, Change };
    
    Type type;
    Path path;              // Path to the changed value
    std::string old_value;  // String representation of old value
    std::string new_value;  // String representation of new value
};

// ============================================================
// RecursiveDiffCollector
// 
// Recursively collects all differences between two Value states.
// 
// Optimizations:
// 1. Uses immer::box pointer comparison for O(1) unchanged subtree skipping
// 2. Uses immer::diff for map comparisons (efficient for structural sharing)
// 3. Uses index iteration for vectors (immer::diff only supports map/set)
// ============================================================
class RecursiveDiffCollector {
private:
    std::vector<DiffEntry> diffs_;

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
    void diff(const Value& old_val, const Value& new_val);

    // Access results
    [[nodiscard]] const std::vector<DiffEntry>& get_diffs() const;
    
    // Clear collected diffs
    void clear();
    
    // Check if there are any changes
    [[nodiscard]] bool has_changes() const;

    // Print diffs to stdout
    void print_diffs() const;
};

// ============================================================
// Demo functions
// ============================================================

// Demo: Basic immer::diff usage
void demo_immer_diff();

// Demo: Full recursive diff collection
void demo_recursive_diff_collector();

} // namespace immer_lens
