// erased_lens.cpp
// Implementation of custom type-erased lens (Scheme 1)

#include <lager_ext/erased_lens.h>
#include <lager_ext/path_utils.h>
#include <iostream>

namespace lager_ext {

// ============================================================
// ErasedLens implementation
// ============================================================

ErasedLens::ErasedLens()
    : getter_([](const Value& v) { return v; })
    , setter_([](Value, Value v) { return v; })
{
}

ErasedLens::ErasedLens(Getter g, Setter s)
    : getter_(std::move(g))
    , setter_(std::move(s))
{
}

Value ErasedLens::get(const Value& v) const
{
    return getter_(v);
}

Value ErasedLens::set(Value whole, Value part) const
{
    return setter_(std::move(whole), std::move(part));
}

ErasedLens ErasedLens::compose(const ErasedLens& inner) const
{
    auto outer_get = getter_;
    auto outer_set = setter_;
    auto inner_get = inner.getter_;
    auto inner_set = inner.setter_;

    return ErasedLens{
        [=](const Value& v) { return inner_get(outer_get(v)); },
        [=](Value whole, Value new_val) {
            auto outer_part = outer_get(whole);
            auto new_outer = inner_set(std::move(outer_part), std::move(new_val));
            return outer_set(std::move(whole), std::move(new_outer));
        }};
}

ErasedLens operator|(const ErasedLens& lhs, const ErasedLens& rhs)
{
    return lhs.compose(rhs);
}

// ============================================================
// Lens factory functions
// ============================================================

ErasedLens make_key_lens(const std::string& key)
{
    return ErasedLens{
        // Getter
        [key](const Value& obj) -> Value {
            if (auto* map = obj.get_if<ValueMap>()) {
                if (auto found = map->find(key))
                    return **found;
            }
            return Value{};
        },
        // Setter (strict mode)
        // Note: For auto-vivification, use set_at_path_vivify() from path_utils.h
        [key](Value obj, Value value) -> Value {
            if (auto* m = obj.get_if<ValueMap>()) {
                return Value{m->set(key, immer::box<Value>{std::move(value)})};
            }
            // Strict mode: log error and return unchanged
            std::cerr << "[make_key_lens] Not a map, cannot set key: " << key << "\n";
            return obj;
        }};
}

ErasedLens make_index_lens(std::size_t index)
{
    return ErasedLens{
        // Getter
        [index](const Value& obj) -> Value {
            if (auto* vec = obj.get_if<ValueVector>()) {
                if (index < vec->size())
                    return *(*vec)[index];
            }
            return Value{};
        },
        // Setter (strict mode)
        // Note: For auto-vivification, use set_at_path_vivify() from path_utils.h
        [index](Value obj, Value value) -> Value {
            if (auto* v = obj.get_if<ValueVector>()) {
                if (index < v->size()) {
                    return Value{v->set(index, immer::box<Value>{std::move(value)})};
                }
            }
            // Strict mode: log error and return unchanged
            std::cerr << "[make_index_lens] Not a vector or index out of range: " << index << "\n";
            return obj;
        }};
}

// ============================================================
// Optimized Path Lens Implementation
//
// Uses shared path_utils.h for direct traversal functions,
// avoiding code duplication with lager_lens.cpp
// ============================================================

ErasedLens path_lens(const Path& path)
{
    if (path.empty()) {
        return ErasedLens{}; // Identity lens
    }

    // OPTIMIZED: Single lens with direct traversal instead of N nested compositions
    // Uses path_utils.h functions for efficient path access
    return ErasedLens{
        // Getter: Single-pass traversal using path_utils
        [path](const Value& root) -> Value {
            return get_at_path_direct(root, path);
        },
        // Setter: Recursive rebuild using path_utils
        [path](Value root, Value new_val) -> Value {
            return set_at_path_direct(root, path, std::move(new_val));
        }
    };
}

// ============================================================
// Demo function
// ============================================================

void demo_erased_lens()
{
    std::cout << "\n=== Scheme 1: Custom ErasedLens Demo ===\n\n";

    // Use common test data
    Value data = create_sample_data();

    std::cout << "Data structure:\n";
    print_value(data, "", 1);

    // Test path_lens
    std::cout << "\n--- Test 1: GET using path_lens ---\n";
    Path name_path = {std::string{"users"}, size_t{0}, std::string{"name"}};
    auto lens = path_lens(name_path);

    std::cout << "Path: " << path_to_string(name_path) << "\n";
    std::cout << "Value: " << value_to_string(lens.get(data)) << "\n";

    // Test SET
    std::cout << "\n--- Test 2: SET using path_lens ---\n";
    Value updated = lens.set(data, Value{std::string{"Alicia"}});
    std::cout << "After setting to \"Alicia\":\n";
    std::cout << "New value: " << value_to_string(lens.get(updated)) << "\n";

    // Test OVER
    std::cout << "\n--- Test 3: OVER using path_lens ---\n";
    Path age_path = {std::string{"users"}, size_t{1}, std::string{"age"}};
    auto age_lens = path_lens(age_path);

    std::cout << "Original age: " << value_to_string(age_lens.get(data)) << "\n";
    Value incremented = age_lens.over(data, [](Value v) {
        if (auto* n = v.get_if<int>()) {
            return Value{*n + 5};
        }
        return v;
    });
    std::cout << "After +5: " << value_to_string(age_lens.get(incremented)) << "\n";

    // Test composition with | operator
    std::cout << "\n--- Test 4: Composition with | operator ---\n";
    auto config_version = make_key_lens("config") | make_key_lens("version");
    std::cout << "config.version = " << value_to_string(config_version.get(data)) << "\n";

    std::cout << "\n=== Demo End ===\n\n";
}

} // namespace lager_ext
