// json_pointer.cpp
// Implementation of JSON Pointer (RFC 6901) API (Scheme 4)

#include <lager_ext/json_pointer.h>
#include <cctype>
#include <iostream>
#include <ranges>

namespace lager_ext {

// ============================================================
// JSON Pointer parsing
// ============================================================

namespace {

/// Unescape a JSON Pointer segment according to RFC 6901
/// ~1 -> /, ~0 -> ~
std::string unescape_segment(std::string_view segment)
{
    std::string result;
    result.reserve(segment.size());

    for (size_t i = 0; i < segment.size(); ++i) {
        if (segment[i] == '~' && i + 1 < segment.size()) {
            if (segment[i + 1] == '1') {
                result += '/';
                ++i;
                continue;
            } else if (segment[i + 1] == '0') {
                result += '~';
                ++i;
                continue;
            }
        }
        result += segment[i];
    }

    return result;
}

/// Check if a string represents a valid array index
bool is_array_index(const std::string& s)
{
    if (s.empty() || s == "-") {  // "-" is special case for "end of array"
        return false;
    }
    return std::ranges::all_of(s, [](unsigned char c) { return std::isdigit(c); });
}

} // anonymous namespace

Path parse_json_pointer(std::string_view pointer)
{
    // Empty pointer refers to root
    if (pointer.empty()) {
        return Path{};
    }

    // JSON Pointer must start with '/'
    if (pointer[0] != '/') {
        std::cerr << "[parse_json_pointer] Invalid pointer, must start with '/': "
                  << pointer << "\n";
        return Path{};
    }

    // Use PathBuilder for fluent path construction
    PathBuilder builder;

    // Skip leading '/'
    pointer = pointer.substr(1);

    // Split by '/'
    while (!pointer.empty()) {
        // Find next '/'
        auto pos = pointer.find('/');
        std::string_view segment = (pos == std::string_view::npos)
                                    ? pointer
                                    : pointer.substr(0, pos);

        // Unescape and add to path
        std::string unescaped = unescape_segment(segment);

        if (is_array_index(unescaped)) {
            builder = std::move(builder).index(static_cast<size_t>(std::stoull(unescaped)));
        } else {
            builder = std::move(builder).key(std::move(unescaped));
        }

        // Move to next segment
        if (pos == std::string_view::npos) {
            break;
        }
        pointer = pointer.substr(pos + 1);
    }

    return builder.path();
}

std::string path_to_json_pointer(const Path& path)
{
    if (path.empty()) {
        return "";  // Root reference
    }

    std::string result;
    for (const auto& elem : path) {
        result += '/';
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                // Escape: ~ -> ~0, / -> ~1
                for (char c : v) {
                    if (c == '~') {
                        result += "~0";
                    } else if (c == '/') {
                        result += "~1";
                    } else {
                        result += c;
                    }
                }
            } else {
                result += std::to_string(v);
            }
        }, elem);
    }
    return result;
}

// ============================================================
// JSON Pointer lens functions
// ============================================================

LagerValueLens json_pointer_lens(std::string_view pointer)
{
    return lager_path_lens(parse_json_pointer(pointer));
}

Value get_by_pointer(const Value& data, std::string_view pointer)
{
    auto lens = json_pointer_lens(pointer);
    return lager::view(lens, data);
}

Value set_by_pointer(const Value& data, std::string_view pointer, Value new_value)
{
    auto lens = json_pointer_lens(pointer);
    return lager::set(lens, data, std::move(new_value));
}

// ============================================================
// Demo function
// ============================================================

void demo_json_pointer()
{
    std::cout << "\n=== Scheme 4: JSON Pointer (RFC 6901) API Demo ===\n\n";

    // Create test data:
    // {
    //   "users": [
    //     { "name": "Alice", "profile": { "city": "Beijing", "tags/skills": ["c++", "rust"] } },
    //     { "name": "Bob", "profile": { "city": "Shanghai" } }
    //   ],
    //   "config": { "version": 1, "theme~mode": "dark" }
    // }

    // Build inner structures first
    ValueVector alice_tags;
    alice_tags = alice_tags.push_back(immer::box<Value>{"c++"});
    alice_tags = alice_tags.push_back(immer::box<Value>{"rust"});

    ValueMap alice_profile;
    alice_profile = alice_profile.set("city", immer::box<Value>{"Beijing"});
    alice_profile = alice_profile.set("tags/skills", immer::box<Value>{alice_tags});  // key with '/'

    ValueMap alice;
    alice = alice.set("name", immer::box<Value>{"Alice"});
    alice = alice.set("profile", immer::box<Value>{alice_profile});

    ValueMap bob_profile;
    bob_profile = bob_profile.set("city", immer::box<Value>{"Shanghai"});

    ValueMap bob;
    bob = bob.set("name", immer::box<Value>{"Bob"});
    bob = bob.set("profile", immer::box<Value>{bob_profile});

    ValueVector users;
    users = users.push_back(immer::box<Value>{alice});
    users = users.push_back(immer::box<Value>{bob});

    ValueMap config;
    config = config.set("version", immer::box<Value>{1});
    config = config.set("theme~mode", immer::box<Value>{"dark"});  // key with '~'

    ValueMap root;
    root = root.set("users", immer::box<Value>{users});
    root = root.set("config", immer::box<Value>{config});

    Value data{root};

    std::cout << "Data structure:\n";
    print_value(data, "", 1);

    // --- Test 1: Basic path parsing ---
    std::cout << "\n--- Test 1: JSON Pointer Parsing ---\n";

    std::vector<std::string> test_pointers = {
        "",                           // root
        "/users",                     // simple key
        "/users/0",                   // array index
        "/users/0/name",              // nested path
        "/users/0/profile/city",      // deep nesting
        "/config/theme~0mode",        // ~ escape: ~0 -> ~
        "/users/0/profile/tags~1skills",  // / escape: ~1 -> /
        "/users/0/profile/tags~1skills/0" // array in escaped key
    };

    for (const auto& ptr : test_pointers) {
        auto path = parse_json_pointer(ptr);
        auto round_trip = path_to_json_pointer(path);
        std::cout << "  \"" << ptr << "\" -> Path{";
        for (size_t i = 0; i < path.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::visit([](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    std::cout << "\"" << v << "\"";
                } else {
                    std::cout << v;
                }
            }, path[i]);
        }
        std::cout << "} -> \"" << round_trip << "\"\n";
    }

    // --- Test 2: GET operations ---
    std::cout << "\n--- Test 2: GET by JSON Pointer ---\n";

    std::cout << "  get_by_pointer(\"/users/0/name\") = "
              << value_to_string(get_by_pointer(data, "/users/0/name")) << "\n";

    std::cout << "  get_by_pointer(\"/users/1/profile/city\") = "
              << value_to_string(get_by_pointer(data, "/users/1/profile/city")) << "\n";

    std::cout << "  get_by_pointer(\"/config/version\") = "
              << value_to_string(get_by_pointer(data, "/config/version")) << "\n";

    // Access key with special characters (escaped)
    std::cout << "  get_by_pointer(\"/config/theme~0mode\") = "  // ~0 -> ~
              << value_to_string(get_by_pointer(data, "/config/theme~0mode")) << "\n";

    std::cout << "  get_by_pointer(\"/users/0/profile/tags~1skills\") = "  // ~1 -> /
              << value_to_string(get_by_pointer(data, "/users/0/profile/tags~1skills")) << "\n";

    std::cout << "  get_by_pointer(\"/users/0/profile/tags~1skills/0\") = "
              << value_to_string(get_by_pointer(data, "/users/0/profile/tags~1skills/0")) << "\n";

    // Non-existent path
    std::cout << "  get_by_pointer(\"/nonexistent\") = "
              << value_to_string(get_by_pointer(data, "/nonexistent")) << "\n";

    // --- Test 3: SET operations ---
    std::cout << "\n--- Test 3: SET by JSON Pointer ---\n";

    // Change Alice's name
    Value updated1 = set_by_pointer(data, "/users/0/name", Value{std::string{"Alicia"}});
    std::cout << "  After set_by_pointer(\"/users/0/name\", \"Alicia\"):\n";
    std::cout << "    users[0].name = " << value_to_string(get_by_pointer(updated1, "/users/0/name")) << "\n";

    // Update config version
    Value updated2 = set_by_pointer(data, "/config/version", Value{2});
    std::cout << "  After set_by_pointer(\"/config/version\", 2):\n";
    std::cout << "    config.version = " << value_to_string(get_by_pointer(updated2, "/config/version")) << "\n";

    // --- Test 4: OVER operations ---
    std::cout << "\n--- Test 4: OVER by JSON Pointer ---\n";

    // Increment version
    Value updated3 = over_by_pointer(data, "/config/version", [](Value v) {
        if (auto* n = v.get_if<int>()) {
            return Value{*n + 10};
        }
        return v;
    });
    std::cout << "  After over_by_pointer(\"/config/version\", n + 10):\n";
    std::cout << "    config.version = " << value_to_string(get_by_pointer(updated3, "/config/version")) << "\n";

    // --- Test 5: Using with lager ecosystem ---
    std::cout << "\n--- Test 5: Direct lens usage with lager::view/set/over ---\n";

    // Get lens once, reuse multiple times
    auto name_lens = json_pointer_lens("/users/0/name");

    std::cout << "  lens = json_pointer_lens(\"/users/0/name\")\n";
    std::cout << "  lager::view(lens, data) = " << value_to_string(lager::view(name_lens, data)) << "\n";

    auto after_set = lager::set(name_lens, data, Value{std::string{"Alice2"}});
    std::cout << "  lager::set(lens, data, \"Alice2\") -> " << value_to_string(lager::view(name_lens, after_set)) << "\n";

    auto after_over = lager::over(name_lens, data, [](Value v) {
        if (auto* s = v.get_if<std::string>()) {
            return Value{*s + " (modified)"};
        }
        return v;
    });
    std::cout << "  lager::over(lens, data, fn) -> " << value_to_string(lager::view(name_lens, after_over)) << "\n";

    // --- Summary ---
    std::cout << "\n--- Summary ---\n";
    std::cout << "JSON Pointer API provides:\n";
    std::cout << "  1. Familiar path syntax: \"/users/0/name\"\n";
    std::cout << "  2. RFC 6901 compliant escape sequences (~0 for ~, ~1 for /)\n";
    std::cout << "  3. Convenience functions: get_by_pointer(), set_by_pointer(), over_by_pointer()\n";
    std::cout << "  4. Full lager integration: json_pointer_lens() returns LagerValueLens\n";
    std::cout << "  5. Immutable operations: all set/over return new Value\n";
    std::cout << "\n=== Demo End ===\n\n";
}

} // namespace lager_ext
