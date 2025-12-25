// diff_collector.cpp
// Implementation of DiffCollector and diff demos

#include <lager_ext/diff_collector.h>
#include <immer/algorithm.hpp>
#include <iostream>

namespace lager_ext {

// ============================================================
// DiffCollector implementation
// ============================================================

void DiffCollector::diff(const Value& old_val, const Value& new_val, bool recursive)
{
    diffs_.clear();
    recursive_ = recursive;

    // Fast path: if both Values share the same variant storage, no changes
    // This handles the case where the same Value object is compared to itself
    if (&old_val.data == &new_val.data) {
        return;
    }

    // Pre-allocate to reduce reallocations during diff collection
    diffs_.reserve(32);

    Path root_path;
    root_path.reserve(16);  // Pre-allocate for typical nesting depth
    diff_value(old_val, new_val, root_path);

    // Shrink to fit if we over-allocated significantly
    if (diffs_.size() > 0 && diffs_.capacity() > diffs_.size() * 2) {
        diffs_.shrink_to_fit();
    }
}

const std::vector<DiffEntry>& DiffCollector::get_diffs() const
{
    return diffs_;
}

void DiffCollector::clear()
{
    diffs_.clear();
}

bool DiffCollector::has_changes() const
{
    return !diffs_.empty();
}

void DiffCollector::print_diffs() const
{
    if (diffs_.empty()) {
        std::cout << "  (no changes)\n";
        return;
    }
    for (const auto& d : diffs_) {
        std::string type_str;
        switch (d.type) {
            case DiffEntry::Type::Add:    type_str = "ADD   "; break;
            case DiffEntry::Type::Remove: type_str = "REMOVE"; break;
            case DiffEntry::Type::Change: type_str = "CHANGE"; break;
        }
        std::cout << "  " << type_str << " " << path_to_string(d.path);
        if (d.type == DiffEntry::Type::Change) {
            std::cout << ": " << value_to_string(d.old_value) << " -> " << value_to_string(d.new_value);
        } else if (d.type == DiffEntry::Type::Add) {
            std::cout << ": " << value_to_string(d.new_value);
        } else {
            std::cout << ": " << value_to_string(d.old_value);
        }
        std::cout << "\n";
    }
}

void DiffCollector::diff_value(const Value& old_val, const Value& new_val, Path& current_path)
{
    // NOTE: This function is recursive. For extremely deeply nested data structures,
    // consider implementing an iterative version with an explicit stack to avoid
    // stack overflow. Typical use cases with nesting depth < 100 should be safe.
    // For deeply nested data (e.g., > 1000 levels), an iterative approach is recommended.

    // Fast path: if types differ, record as Change
    const auto old_index = old_val.data.index();
    const auto new_index = new_val.data.index();
    if (old_index != new_index) [[unlikely]] {
        diffs_.emplace_back(DiffEntry::Type::Change, current_path, old_val, new_val);
        return;
    }

    std::visit([&](const auto& old_arg) {
        using T = std::decay_t<decltype(old_arg)>;

        if constexpr (std::is_same_v<T, ValueMap>) {
            // We already verified indices match, so this is safe
            const auto& new_map = std::get<ValueMap>(new_val.data);
            // OPTIMIZATION: immer container identity check - O(1)
            // If the underlying immer::map shares the same root, skip entirely
            if (old_arg.impl().root == new_map.impl().root &&
                old_arg.impl().size == new_map.impl().size) [[likely]] {
                return;
            }
            // Shallow mode: report container change without recursing
            if (!recursive_) {
                diffs_.emplace_back(DiffEntry::Type::Change, current_path, old_val, new_val);
                return;
            }
            diff_map(old_arg, new_map, current_path);
        }
        else if constexpr (std::is_same_v<T, ValueVector>) {
            const auto& new_vec = std::get<ValueVector>(new_val.data);
            // OPTIMIZATION: immer container identity check - O(1)
            // If the underlying immer::flex_vector shares the same root, skip entirely
            if (old_arg.impl().root == new_vec.impl().root &&
                old_arg.impl().tail == new_vec.impl().tail &&
                old_arg.impl().size == new_vec.impl().size) [[likely]] {
                return;
            }
            // Shallow mode: report container change without recursing
            if (!recursive_) {
                diffs_.emplace_back(DiffEntry::Type::Change, current_path, old_val, new_val);
                return;
            }
            diff_vector(old_arg, new_vec, current_path);
        }
        else if constexpr (std::is_same_v<T, std::monostate>) {
            // Both null, no change
        }
        else {
            // Primitive types: direct comparison (already verified same type)
            const auto& new_arg = std::get<T>(new_val.data);
            if (old_arg != new_arg) {
                diffs_.emplace_back(DiffEntry::Type::Change, current_path, old_val, new_val);
            }
        }
    }, old_val.data);
}

void DiffCollector::diff_map(const ValueMap& old_map, const ValueMap& new_map, Path& current_path)
{
    // OPTIMIZED: Use push_back/pop_back pattern to avoid Path copying
    auto map_differ = immer::make_differ(
        // added
        [&](const std::pair<const std::string, ValueBox>& added_kv) {
            current_path.push_back(added_kv.first);
            collect_added(*added_kv.second, current_path);
            current_path.pop_back();
        },
        // removed
        [&](const std::pair<const std::string, ValueBox>& removed_kv) {
            current_path.push_back(removed_kv.first);
            collect_removed(*removed_kv.second, current_path);
            current_path.pop_back();
        },
        // changed (retained key)
        [&](const std::pair<const std::string, ValueBox>& old_kv,
            const std::pair<const std::string, ValueBox>& new_kv) {
            // Optimization: pointer comparison - O(1)
            if (old_kv.second.get() == new_kv.second.get()) [[likely]] {
                return; // Same pointer, unchanged
            }
            current_path.push_back(old_kv.first);
            diff_value(*old_kv.second, *new_kv.second, current_path);
            current_path.pop_back();
        }
    );

    immer::diff(old_map, new_map, map_differ);
}

void DiffCollector::diff_vector(const ValueVector& old_vec, const ValueVector& new_vec, Path& current_path)
{
    const size_t old_size = old_vec.size();
    const size_t new_size = new_vec.size();
    const size_t common_size = std::min(old_size, new_size);

    // OPTIMIZED: Use push_back/pop_back pattern to avoid Path copying
    // Compare common indices
    for (size_t i = 0; i < common_size; ++i) {
        const auto& old_box = old_vec[i];
        const auto& new_box = new_vec[i];

        // Optimization: immer::box pointer comparison - O(1)
        if (old_box.get() == new_box.get()) [[likely]] {
            continue;
        }

        current_path.push_back(i);
        diff_value(*old_box, *new_box, current_path);
        current_path.pop_back();
    }

    // Removed tail elements
    for (size_t i = common_size; i < old_size; ++i) {
        current_path.push_back(i);
        collect_removed(*old_vec[i], current_path);
        current_path.pop_back();
    }

    // Added tail elements
    for (size_t i = common_size; i < new_size; ++i) {
        current_path.push_back(i);
        collect_added(*new_vec[i], current_path);
        current_path.pop_back();
    }
}

// Helper: Collect entries for add/remove operations
// In recursive mode: recursively collect all nested entries
// In shallow mode: only report the container itself
// is_add: true for added entries, false for removed entries
void DiffCollector::collect_entries(const Value& val, Path& current_path, bool is_add)
{
    // In shallow mode, just record the value at current path (no recursion)
    if (!recursive_) {
        if (is_add) {
            diffs_.emplace_back(DiffEntry::Type::Add, current_path, Value{}, val);
        } else {
            diffs_.emplace_back(DiffEntry::Type::Remove, current_path, val, Value{});
        }
        return;
    }

    // Recursive mode: descend into containers
    std::visit([&](const auto& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, ValueMap>) {
            for (const auto& [k, v] : arg) {
                current_path.push_back(k);
                collect_entries(*v, current_path, is_add);
                current_path.pop_back();
            }
        }
        else if constexpr (std::is_same_v<T, ValueVector>) {
            for (size_t i = 0; i < arg.size(); ++i) {
                current_path.push_back(i);
                collect_entries(*arg[i], current_path, is_add);
                current_path.pop_back();
            }
        }
        else if constexpr (!std::is_same_v<T, std::monostate>) {
            // Leaf value: record it
            if (is_add) {
                diffs_.emplace_back(DiffEntry::Type::Add, current_path, Value{}, val);
            } else {
                diffs_.emplace_back(DiffEntry::Type::Remove, current_path, val, Value{});
            }
        }
    }, val.data);
}

void DiffCollector::collect_removed(const Value& val, Path& current_path)
{
    collect_entries(val, current_path, false);
}

void DiffCollector::collect_added(const Value& val, Path& current_path)
{
    collect_entries(val, current_path, true);
}

// ============================================================
// Quick change detection (early exit optimization)
// ============================================================

bool has_any_difference(const Value& old_val, const Value& new_val, bool recursive)
{
    // Fast path: same object
    if (&old_val.data == &new_val.data) {
        return false;
    }
    return detail::values_differ(old_val, new_val, recursive);
}

namespace detail {

bool values_differ(const Value& old_val, const Value& new_val, bool recursive)
{
    // Fast path: different types
    const auto old_index = old_val.data.index();
    const auto new_index = new_val.data.index();
    if (old_index != new_index) [[unlikely]] {
        return true;
    }

    return std::visit([&](const auto& old_arg) -> bool {
        using T = std::decay_t<decltype(old_arg)>;

        if constexpr (std::is_same_v<T, ValueMap>) {
            const auto& new_map = std::get<ValueMap>(new_val.data);
            // O(1) identity check
            if (old_arg.impl().root == new_map.impl().root &&
                old_arg.impl().size == new_map.impl().size) [[likely]] {
                return false;
            }
            // Shallow mode: identity check failed, containers are different
            if (!recursive) {
                return true;
            }
            return maps_differ(old_arg, new_map, recursive);
        }
        else if constexpr (std::is_same_v<T, ValueVector>) {
            const auto& new_vec = std::get<ValueVector>(new_val.data);
            // O(1) identity check
            if (old_arg.impl().root == new_vec.impl().root &&
                old_arg.impl().tail == new_vec.impl().tail &&
                old_arg.impl().size == new_vec.impl().size) [[likely]] {
                return false;
            }
            // Shallow mode: identity check failed, containers are different
            if (!recursive) {
                return true;
            }
            return vectors_differ(old_arg, new_vec, recursive);
        }
        else if constexpr (std::is_same_v<T, std::monostate>) {
            return false;  // Both null
        }
        else {
            // Primitive types: direct comparison
            const auto& new_arg = std::get<T>(new_val.data);
            return old_arg != new_arg;
        }
    }, old_val.data);
}

bool maps_differ(const ValueMap& old_map, const ValueMap& new_map, bool recursive)
{
    // Quick size check
    if (old_map.size() != new_map.size()) {
        return true;
    }

    // Use immer::diff with early exit
    bool found_difference = false;

    auto differ = immer::make_differ(
        // added - any addition means difference
        [&](const std::pair<const std::string, ValueBox>&) {
            found_difference = true;
        },
        // removed - any removal means difference
        [&](const std::pair<const std::string, ValueBox>&) {
            found_difference = true;
        },
        // retained - check if values differ
        [&](const std::pair<const std::string, ValueBox>& old_kv,
            const std::pair<const std::string, ValueBox>& new_kv) {
            if (found_difference) return;  // Already found, skip

            // O(1) pointer check
            if (old_kv.second.get() == new_kv.second.get()) [[likely]] {
                return;
            }
            // Shallow mode: pointer differs, that's enough
            if (!recursive) {
                found_difference = true;
                return;
            }
            // Recurse
            if (values_differ(*old_kv.second, *new_kv.second, recursive)) {
                found_difference = true;
            }
        }
    );

    immer::diff(old_map, new_map, differ);
    return found_difference;
}

bool vectors_differ(const ValueVector& old_vec, const ValueVector& new_vec, bool recursive)
{
    const size_t old_size = old_vec.size();
    const size_t new_size = new_vec.size();

    // Size difference means there's a change
    if (old_size != new_size) {
        return true;
    }

    // Check each element (early exit on first difference)
    for (size_t i = 0; i < old_size; ++i) {
        const auto& old_box = old_vec[i];
        const auto& new_box = new_vec[i];

        // O(1) pointer check
        if (old_box.get() == new_box.get()) [[likely]] {
            continue;
        }

        // Shallow mode: pointer differs, that's enough
        if (!recursive) {
            return true;
        }

        // Recurse - early exit if different
        if (values_differ(*old_box, *new_box, recursive)) {
            return true;
        }
    }

    return false;
}

} // namespace detail

// ============================================================
// Demo: immer::diff basic usage
// ============================================================

void demo_immer_diff()
{
    std::cout << "\n=== immer::diff Demo ===\n\n";

    // --- immer::vector comparison (manual) ---
    std::cout << "--- immer::vector comparison (manual) ---\n";
    std::cout << "Note: immer::diff does NOT support vector, must compare manually\n\n";

    ValueVector old_vec;
    old_vec = old_vec.push_back(ValueBox{Value{std::string{"Alice"}}});
    old_vec = old_vec.push_back(ValueBox{Value{std::string{"Bob"}}});
    old_vec = old_vec.push_back(ValueBox{Value{std::string{"Charlie"}}});

    ValueVector new_vec;
    new_vec = new_vec.push_back(ValueBox{Value{std::string{"Alice"}}});
    new_vec = new_vec.push_back(ValueBox{Value{std::string{"Bobby"}}});
    new_vec = new_vec.push_back(ValueBox{Value{std::string{"Charlie"}}});
    new_vec = new_vec.push_back(ValueBox{Value{std::string{"David"}}});

    std::cout << "Old: [Alice, Bob, Charlie]\n";
    std::cout << "New: [Alice, Bobby, Charlie, David]\n\n";

    std::cout << "Manual comparison:\n";

    size_t old_size = old_vec.size();
    size_t new_size = new_vec.size();
    size_t common_size = std::min(old_size, new_size);

    for (size_t i = 0; i < common_size; ++i) {
        const auto& old_box = old_vec[i];
        const auto& new_box = new_vec[i];

        auto* old_str = old_box->get_if<std::string>();
        auto* new_str = new_box->get_if<std::string>();

        if (old_str && new_str) {
            if (old_box == new_box) {
                std::cout << "  [" << i << "] retained: " << *old_str << " (same pointer)\n";
            } else if (*old_str == *new_str) {
                std::cout << "  [" << i << "] retained: " << *old_str << " (same value)\n";
            } else {
                std::cout << "  [" << i << "] modified: " << *old_str << " -> " << *new_str << "\n";
            }
        }
    }

    for (size_t i = common_size; i < old_size; ++i) {
        if (auto* str = old_vec[i]->get_if<std::string>()) {
            std::cout << "  [" << i << "] removed: " << *str << "\n";
        }
    }

    for (size_t i = common_size; i < new_size; ++i) {
        if (auto* str = new_vec[i]->get_if<std::string>()) {
            std::cout << "  [" << i << "] added: " << *str << "\n";
        }
    }

    // --- immer::map diff ---
    std::cout << "\n--- immer::map diff (using immer::diff) ---\n";

    ValueMap old_map;
    old_map = old_map.set("name", ValueBox{Value{std::string{"Tom"}}});
    old_map = old_map.set("age", ValueBox{Value{25}});
    old_map = old_map.set("city", ValueBox{Value{std::string{"Beijing"}}});

    ValueMap new_map;
    new_map = new_map.set("name", ValueBox{Value{std::string{"Tom"}}});
    new_map = new_map.set("age", ValueBox{Value{26}});
    new_map = new_map.set("email", ValueBox{Value{std::string{"tom@x.com"}}});

    std::cout << "Old: {name: Tom, age: 25, city: Beijing}\n";
    std::cout << "New: {name: Tom, age: 26, email: tom@x.com}\n\n";

    std::cout << "immer::diff results:\n";

    immer::diff(
        old_map,
        new_map,
        [](const auto& removed) {
            std::cout << "  [removed] key=" << removed.first << "\n";
        },
        [](const auto& added) {
            std::cout << "  [added] key=" << added.first << "\n";
        },
        [](const auto& old_kv, const auto& new_kv) {
            if (old_kv.second.get() == new_kv.second.get()) {
                std::cout << "  [retained] key=" << old_kv.first << " (same pointer)\n";
            } else {
                std::cout << "  [modified] key=" << old_kv.first << "\n";
            }
        }
    );

    std::cout << "\n=== Demo End ===\n\n";
}

// ============================================================
// Demo: DiffCollector usage
// ============================================================

void demo_recursive_diff_collector()
{
    std::cout << "\n=== DiffCollector Demo ===\n\n";

    // Create old state
    ValueMap user1;
    user1 = user1.set("name", ValueBox{Value{std::string{"Alice"}}});
    user1 = user1.set("age", ValueBox{Value{25}});

    ValueMap user2;
    user2 = user2.set("name", ValueBox{Value{std::string{"Bob"}}});
    user2 = user2.set("age", ValueBox{Value{30}});

    ValueVector users_old;
    users_old = users_old.push_back(ValueBox{Value{user1}});
    users_old = users_old.push_back(ValueBox{Value{user2}});

    ValueMap old_root;
    old_root = old_root.set("users", ValueBox{Value{users_old}});
    old_root = old_root.set("version", ValueBox{Value{1}});

    Value old_state{old_root};

    // Create new state (with modifications)
    ValueMap user1_new;
    user1_new = user1_new.set("name", ValueBox{Value{std::string{"Alice"}}});
    user1_new = user1_new.set("age", ValueBox{Value{26}});  // modified
    user1_new = user1_new.set("email", ValueBox{Value{std::string{"alice@x.com"}}}); // added

    ValueMap user3;
    user3 = user3.set("name", ValueBox{Value{std::string{"Charlie"}}});
    user3 = user3.set("age", ValueBox{Value{35}});

    ValueVector users_new;
    users_new = users_new.push_back(ValueBox{Value{user1_new}});
    users_new = users_new.push_back(ValueBox{Value{user2}});  // unchanged
    users_new = users_new.push_back(ValueBox{Value{user3}});  // added

    ValueMap new_root;
    new_root = new_root.set("users", ValueBox{Value{users_new}});
    new_root = new_root.set("version", ValueBox{Value{2}});  // modified

    Value new_state{new_root};

    // Print states
    std::cout << "--- Old State ---\n";
    print_value(old_state, "", 1);

    std::cout << "\n--- New State ---\n";
    print_value(new_state, "", 1);

    // Collect diffs (recursive mode - default)
    std::cout << "\n--- Recursive Diff Results ---\n";
    DiffCollector collector;
    collector.diff(old_state, new_state);  // recursive = true (default)
    collector.print_diffs();
    std::cout << "\nDetected " << collector.get_diffs().size() << " change(s)\n";

    // Collect diffs (shallow mode)
    std::cout << "\n--- Shallow Diff Results ---\n";
    collector.diff(old_state, new_state, false);  // recursive = false
    collector.print_diffs();
    std::cout << "\nDetected " << collector.get_diffs().size() << " change(s)\n";

    // Quick check using has_any_difference
    std::cout << "\n--- Quick Difference Check ---\n";
    std::cout << "has_any_difference (recursive): " 
              << (has_any_difference(old_state, new_state) ? "true" : "false") << "\n";
    std::cout << "has_any_difference (shallow):   " 
              << (has_any_difference(old_state, new_state, false) ? "true" : "false") << "\n";

    std::cout << "\n=== Demo End ===\n\n";
}

} // namespace lager_ext
