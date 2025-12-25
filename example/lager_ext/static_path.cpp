// static_path.cpp
// Compile-time static path lens implementation
//
// This file demonstrates zero-overhead path-based lens access
// for data structures known at compile time.

#include <lager_ext/static_path.h>
#include <iostream>
#include <iomanip>

namespace lager_ext {

// ============================================================
// Example: Define a schema with static paths
//
// Imagine this is generated from C++ class reflection:
//
// struct User {
//     std::string name;
//     int age;
//     std::string email;
// };
//
// struct AppConfig {
//     std::string title;
//     std::vector<User> users;
//     struct {
//         int width;
//         int height;
//     } window;
// };
// ============================================================

namespace schema {

using namespace static_path;

// ============================================================
// Path definitions - These are compile-time constants
// ============================================================

// Root-level paths
using TitlePath = StaticPath<K<"title">>;
using UsersPath = StaticPath<K<"users">>;
using WindowPath = StaticPath<K<"window">>;

// Window sub-paths
using WindowWidthPath = StaticPath<K<"window">, K<"width">>;
using WindowHeightPath = StaticPath<K<"window">, K<"height">>;

// User paths (parameterized by index)
template<std::size_t Idx>
using UserPath = StaticPath<K<"users">, I<Idx>>;

template<std::size_t Idx>
using UserNamePath = StaticPath<K<"users">, I<Idx>, K<"name">>;

template<std::size_t Idx>
using UserAgePath = StaticPath<K<"users">, I<Idx>, K<"age">>;

template<std::size_t Idx>
using UserEmailPath = StaticPath<K<"users">, I<Idx>, K<"email">>;

// ============================================================
// Type-safe accessors
// ============================================================

struct AppConfigPaths {
    // Singleton paths
    static constexpr auto title() { return TitlePath{}; }
    static constexpr auto users() { return UsersPath{}; }
    static constexpr auto window() { return WindowPath{}; }
    static constexpr auto window_width() { return WindowWidthPath{}; }
    static constexpr auto window_height() { return WindowHeightPath{}; }

    // Indexed user access
    template<std::size_t I>
    struct user {
        static constexpr auto path() { return UserPath<I>{}; }
        static constexpr auto name() { return UserNamePath<I>{}; }
        static constexpr auto age() { return UserAgePath<I>{}; }
        static constexpr auto email() { return UserEmailPath<I>{}; }
    };
};

} // namespace schema

// ============================================================
// Helper: Create sample data
// ============================================================

static Value create_sample_state() {
    // Create users using Builder API for O(n) construction
    Value user0 = MapBuilder()
        .set("name", Value{"Alice"})
        .set("age", Value{30})
        .set("email", Value{"alice@example.com"})
        .finish();

    Value user1 = MapBuilder()
        .set("name", Value{"Bob"})
        .set("age", Value{25})
        .set("email", Value{"bob@example.com"})
        .finish();

    Value user2 = MapBuilder()
        .set("name", Value{"Charlie"})
        .set("age", Value{35})
        .set("email", Value{"charlie@example.com"})
        .finish();

    // Create users array using Builder API
    Value users = VectorBuilder()
        .push_back(user0)
        .push_back(user1)
        .push_back(user2)
        .finish();

    // Create window config using Builder API
    Value window = MapBuilder()
        .set("width", Value{1920})
        .set("height", Value{1080})
        .finish();

    // Create root state using Builder API
    return MapBuilder()
        .set("title", Value{"My Application"})
        .set("users", users)
        .set("window", window)
        .finish();
}

// ============================================================
// Demo function implementation
// ============================================================

void demo_static_path() {
    std::cout << "\n";
    std::cout << "============================================================\n";
    std::cout << " Static Path Lens Demo (Compile-time Paths)\n";
    std::cout << "============================================================\n\n";

    using namespace schema;

    // Create sample state
    Value state = create_sample_state();

    std::cout << "Initial state:\n";
    print_value(state);
    std::cout << "\n";

    // --------------------------------------------------------
    // Demo 1: Basic compile-time path access
    // --------------------------------------------------------
    std::cout << "--- Demo 1: Compile-time Path Access ---\n\n";

    // Get title using static path
    auto title = TitlePath::get(state);
    std::cout << "TitlePath::get(state) = " << value_to_string(title) << "\n";

    // Get window dimensions
    auto width = WindowWidthPath::get(state);
    auto height = WindowHeightPath::get(state);
    std::cout << "WindowWidthPath::get(state) = " << value_to_string(width) << "\n";
    std::cout << "WindowHeightPath::get(state) = " << value_to_string(height) << "\n";

    // Get user data using indexed paths
    auto user0_name = UserNamePath<0>::get(state);
    auto user1_age = UserAgePath<1>::get(state);
    auto user2_email = UserEmailPath<2>::get(state);

    std::cout << "UserNamePath<0>::get(state) = " << value_to_string(user0_name) << "\n";
    std::cout << "UserAgePath<1>::get(state) = " << value_to_string(user1_age) << "\n";
    std::cout << "UserEmailPath<2>::get(state) = " << value_to_string(user2_email) << "\n\n";

    // --------------------------------------------------------
    // Demo 2: Compile-time immutable updates
    // --------------------------------------------------------
    std::cout << "--- Demo 2: Compile-time Immutable Updates ---\n\n";

    // Update title
    Value state2 = TitlePath::set(state, Value{"Updated App Title"});
    std::cout << "After TitlePath::set(state, \"Updated App Title\"):\n";
    std::cout << "  New title = " << value_to_string(TitlePath::get(state2)) << "\n";
    std::cout << "  Original title = " << value_to_string(TitlePath::get(state)) << "\n\n";

    // Update nested value
    Value state3 = UserAgePath<0>::set(state, Value{31});
    std::cout << "After UserAgePath<0>::set(state, 31):\n";
    std::cout << "  New age = " << value_to_string(UserAgePath<0>::get(state3)) << "\n";
    std::cout << "  Original age = " << value_to_string(UserAgePath<0>::get(state)) << "\n\n";

    // --------------------------------------------------------
    // Demo 3: Using the type-safe accessor pattern
    // --------------------------------------------------------
    std::cout << "--- Demo 3: Type-safe Accessor Pattern ---\n\n";

    // Using AppConfigPaths for cleaner access
    auto title_v2 = AppConfigPaths::title().get(state);
    auto user1_name = AppConfigPaths::user<1>::name().get(state);

    std::cout << "AppConfigPaths::title().get(state) = " << value_to_string(title_v2) << "\n";
    std::cout << "AppConfigPaths::user<1>::name().get(state) = " << value_to_string(user1_name) << "\n\n";

    // --------------------------------------------------------
    // Demo 4: Using the lens directly
    // --------------------------------------------------------
    std::cout << "--- Demo 4: Using Lens Directly ---\n\n";

    // Get the lens object
    auto user0_name_lens = UserNamePath<0>::to_lens();

    // Use it like a regular lens
    auto name1 = user0_name_lens.get(state);
    auto state4 = user0_name_lens.set(state, Value{"Alicia"});
    auto name2 = user0_name_lens.get(state4);

    std::cout << "user0_name_lens.get(state) = " << value_to_string(name1) << "\n";
    std::cout << "After set to \"Alicia\": " << value_to_string(name2) << "\n\n";

    // --------------------------------------------------------
    // Demo 5: Path metadata
    // --------------------------------------------------------
    std::cout << "--- Demo 5: Path Metadata ---\n\n";

    std::cout << "Path depths (compile-time constants):\n";
    std::cout << "  TitlePath::depth = " << TitlePath::depth << "\n";
    std::cout << "  WindowWidthPath::depth = " << WindowWidthPath::depth << "\n";
    std::cout << "  UserNamePath<0>::depth = " << UserNamePath<0>::depth << "\n\n";

    std::cout << "Convert to runtime path:\n";
    auto runtime_path = UserEmailPath<2>::to_runtime_path();
    std::cout << "  UserEmailPath<2>::to_runtime_path() = "
              << path_to_string(runtime_path) << "\n\n";

    // --------------------------------------------------------
    // Demo 6: Path composition
    // --------------------------------------------------------
    std::cout << "--- Demo 6: Path Composition ---\n\n";

    using namespace static_path;

    // Compose paths using ConcatPathT
    using BasePath = StaticPath<K<"users">, I<0>>;
    using FieldPath = StaticPath<K<"name">>;
    using FullPath = ConcatPathT<BasePath, FieldPath>;

    auto composed_result = FullPath::get(state);
    std::cout << "ConcatPathT<users[0], name>::get(state) = "
              << value_to_string(composed_result) << "\n";

    // Extend path using ExtendPathT
    using ExtendedPath = ExtendPathT<BasePath, K<"age">>;
    auto extended_result = ExtendedPath::get(state);
    std::cout << "ExtendPathT<users[0], age>::get(state) = "
              << value_to_string(extended_result) << "\n\n";

    // --------------------------------------------------------
    // Demo 7: Using macros for path definition
    // --------------------------------------------------------
    std::cout << "--- Demo 7: Using Macros ---\n\n";

    using MacroPath = STATIC_PATH(STATIC_KEY("window"), STATIC_KEY("width"));
    auto macro_result = MacroPath::get(state);
    std::cout << "STATIC_PATH(STATIC_KEY(\"window\"), STATIC_KEY(\"width\"))::get(state) = "
              << value_to_string(macro_result) << "\n\n";

#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
    // --------------------------------------------------------
    // Demo 8: JSON Pointer Syntax (C++20 only)
    // --------------------------------------------------------
    std::cout << "--- Demo 8: JSON Pointer Syntax (C++20) ---\n\n";

    // Define paths using JSON Pointer syntax
    using TitlePathJP = static_path::JsonPointerPath<"/title">;
    using UserNamePathJP = static_path::JsonPointerPath<"/users/0/name">;
    using WindowWidthPathJP = static_path::JsonPointerPath<"/window/width">;

    // Use them just like regular StaticPath
    auto title_jp = TitlePathJP::get(state);
    auto user0_name_jp = UserNamePathJP::get(state);
    auto width_jp = WindowWidthPathJP::get(state);

    std::cout << "JsonPointerPath<\"/title\">::get(state) = "
              << value_to_string(title_jp) << "\n";
    std::cout << "JsonPointerPath<\"/users/0/name\">::get(state) = "
              << value_to_string(user0_name_jp) << "\n";
    std::cout << "JsonPointerPath<\"/window/width\">::get(state) = "
              << value_to_string(width_jp) << "\n\n";

    // Verify they work the same as manually defined paths
    std::cout << "Verification (should match Demo 1):\n";
    std::cout << "  TitlePathJP::depth = " << TitlePathJP::depth << "\n";
    std::cout << "  UserNamePathJP::depth = " << UserNamePathJP::depth << "\n";

    // Set using JSON Pointer path
    auto state5 = UserNamePathJP::set(state, Value{"Alice (via JSON Pointer)"});
    std::cout << "  After set via JSON Pointer: "
              << value_to_string(UserNamePathJP::get(state5)) << "\n\n";
#else
    std::cout << "--- Demo 8: JSON Pointer Syntax ---\n\n";
    std::cout << "(Requires C++20 - not available in current build)\n\n";
#endif

    // --------------------------------------------------------
    // Summary
    // --------------------------------------------------------
    std::cout << "============================================================\n";
    std::cout << " Static Path Summary\n";
    std::cout << "============================================================\n\n";
    std::cout << "Advantages:\n";
    std::cout << "  1. Zero runtime overhead for path construction\n";
    std::cout << "  2. Compile-time type checking of path structure\n";
    std::cout << "  3. IDE autocomplete for path definitions\n";
    std::cout << "  4. Paths can be reused as type aliases\n";
    std::cout << "  5. Compatible with runtime Path for debugging\n";
#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
    std::cout << "  6. JSON Pointer syntax for familiar path definitions (C++20)\n";
#endif
    std::cout << "\n";

    std::cout << "Use cases:\n";
    std::cout << "  - C++ class reflection with known schema\n";
    std::cout << "  - Configuration files with fixed structure\n";
    std::cout << "  - Database ORM-like access patterns\n";
    std::cout << "  - Any scenario where paths are known at compile time\n\n";

    std::cout << "Syntax comparison:\n";
    std::cout << "  Manual:       StaticPath<K<\"users\">, I<0>, K<\"name\">>\n";
#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
    std::cout << "  JSON Pointer: JsonPointerPath<\"/users/0/name\"> (C++20)\n";
#endif
    std::cout << "\n";
}

} // namespace lager_ext
