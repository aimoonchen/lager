// diff_collector.cpp
// Implementation of RecursiveDiffCollector and diff demos

#include "diff_collector.h"
#include <immer/algorithm.hpp>
#include <iostream>

namespace immer_lens {

// ============================================================
// RecursiveDiffCollector implementation
// ============================================================

void RecursiveDiffCollector::diff(const Value& old_val, const Value& new_val)
{
    diffs_.clear();
    Path root_path;
    diff_value(old_val, new_val, root_path);
}

const std::vector<DiffEntry>& RecursiveDiffCollector::get_diffs() const
{
    return diffs_;
}

void RecursiveDiffCollector::clear()
{
    diffs_.clear();
}

bool RecursiveDiffCollector::has_changes() const
{
    return !diffs_.empty();
}

void RecursiveDiffCollector::print_diffs() const
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
            std::cout << ": " << d.old_value << " -> " << d.new_value;
        } else if (d.type == DiffEntry::Type::Add) {
            std::cout << ": " << d.new_value;
        } else {
            std::cout << ": " << d.old_value;
        }
        std::cout << "\n";
    }
}

void RecursiveDiffCollector::diff_value(const Value& old_val, const Value& new_val, Path& current_path)
{
    // Fast path: if types differ, record as Change
    if (old_val.data.index() != new_val.data.index()) [[unlikely]] {
        diffs_.push_back({DiffEntry::Type::Change, current_path, 
                          value_to_string(old_val), value_to_string(new_val)});
        return;
    }

    std::visit([&](const auto& old_arg) {
        using T = std::decay_t<decltype(old_arg)>;
        
        if constexpr (std::is_same_v<T, ValueMap>) {
            const auto& new_map = *new_val.get_if<ValueMap>();
            diff_map(old_arg, new_map, current_path);
        }
        else if constexpr (std::is_same_v<T, ValueVector>) {
            const auto& new_vec = *new_val.get_if<ValueVector>();
            diff_vector(old_arg, new_vec, current_path);
        }
        else if constexpr (std::is_same_v<T, std::monostate>) {
            // Both null, no change
        }
        else {
            // Primitive types: direct comparison
            const auto& new_arg = std::get<T>(new_val.data);
            if (old_arg != new_arg) {
                diffs_.push_back({DiffEntry::Type::Change, current_path,
                                  value_to_string(old_val), value_to_string(new_val)});
            }
        }
    }, old_val.data);
}

void RecursiveDiffCollector::diff_map(const ValueMap& old_map, const ValueMap& new_map, Path& current_path)
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

void RecursiveDiffCollector::diff_vector(const ValueVector& old_vec, const ValueVector& new_vec, Path& current_path)
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

// Helper: Recursively collect entries for add/remove operations
// OPTIMIZED: Use push_back/pop_back pattern to avoid Path copying
// is_add: true for added entries, false for removed entries
void RecursiveDiffCollector::collect_entries(const Value& val, Path& current_path, bool is_add)
{
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
            if (is_add) {
                diffs_.push_back({DiffEntry::Type::Add, current_path,
                                  "", value_to_string(val)});
            } else {
                diffs_.push_back({DiffEntry::Type::Remove, current_path,
                                  value_to_string(val), ""});
            }
        }
    }, val.data);
}

void RecursiveDiffCollector::collect_removed(const Value& val, Path& current_path)
{
    collect_entries(val, current_path, false);
}

void RecursiveDiffCollector::collect_added(const Value& val, Path& current_path)
{
    collect_entries(val, current_path, true);
}

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
// Demo: Recursive diff collection
// ============================================================

void demo_recursive_diff_collector()
{
    std::cout << "\n=== RecursiveDiffCollector Demo ===\n\n";

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

    // Collect diffs
    std::cout << "\n--- Diff Results ---\n";
    RecursiveDiffCollector collector;
    collector.diff(old_state, new_state);
    collector.print_diffs();

    std::cout << "\nDetected " << collector.get_diffs().size() << " change(s)\n";
    std::cout << "\n=== Demo End ===\n\n";
}

} // namespace immer_lens
