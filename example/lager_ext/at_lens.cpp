// at_lens.cpp
// Implementation of lager::lenses::at demo (Scheme 3)

#include <lager_ext/at_lens.h>
#include <lager/lenses/at.hpp>
#include <iostream>

namespace lager_ext {

void demo_at_lens()
{
    using namespace lager::lenses;

    std::cout << "\n=== Scheme 3: lager::lenses::at with Value Demo ===\n\n";

    // Use common test data
    Value data = create_sample_data();

    std::cout << "Data structure:\n";
    print_value(data, "", 1);

    // -------------------------------------------------------
    // Test 1: Simple single-level access
    // -------------------------------------------------------
    std::cout << "\n--- Test 1: Single-level at() ---\n";

    auto config_lens = at(std::string{"config"});
    auto config_opt = lager::view(config_lens, data);  // returns optional<Value>

    if (config_opt.has_value()) {
        std::cout << "data.at(\"config\") = " << value_to_string(*config_opt) << "\n";
    } else {
        std::cout << "data.at(\"config\") = (not found)\n";
    }

    // -------------------------------------------------------
    // Test 2: Nested access (config.theme)
    // -------------------------------------------------------
    std::cout << "\n--- Test 2: Nested access ---\n";

    auto config_result = lager::view(at(std::string{"config"}), data);
    if (config_result.has_value()) {
        auto theme_result = lager::view(at(std::string{"theme"}), *config_result);
        if (theme_result.has_value()) {
            std::cout << "config.theme = " << value_to_string(*theme_result) << "\n";
        }
    }

    // -------------------------------------------------------
    // Test 3: Array access (users[0])
    // -------------------------------------------------------
    std::cout << "\n--- Test 3: Array access ---\n";

    auto users_result = lager::view(at(std::string{"users"}), data);
    if (users_result.has_value()) {
        auto first_user_result = lager::view(at(size_t{0}), *users_result);
        if (first_user_result.has_value()) {
            std::cout << "users[0] = " << value_to_string(*first_user_result) << "\n";

            auto name_result = lager::view(at(std::string{"name"}), *first_user_result);
            if (name_result.has_value()) {
                std::cout << "users[0].name = " << value_to_string(*name_result) << "\n";
            }
        }
    }

    // -------------------------------------------------------
    // Test 4: Set operation
    // -------------------------------------------------------
    std::cout << "\n--- Test 4: Set operation ---\n";

    auto config_val = lager::view(at(std::string{"config"}), data);
    if (config_val.has_value()) {
        // Update version inside config
        Value new_config = lager::set(at(std::string{"version"}), *config_val,
                                       std::make_optional(Value{3}));

        // Update config in root
        Value new_data = lager::set(at(std::string{"config"}), data,
                                     std::make_optional(new_config));

        // Verify
        auto verify = lager::view(at(std::string{"config"}), new_data);
        if (verify.has_value()) {
            auto ver = lager::view(at(std::string{"version"}), *verify);
            if (ver.has_value()) {
                std::cout << "After set: config.version = " << value_to_string(*ver) << "\n";
            }
        }
    }

    // -------------------------------------------------------
    // Test 5: Non-existent key access
    // -------------------------------------------------------
    std::cout << "\n--- Test 5: Non-existent key access ---\n";

    auto nonexistent = lager::view(at(std::string{"nonexistent"}), data);
    if (nonexistent.has_value()) {
        std::cout << "data.nonexistent = " << value_to_string(*nonexistent) << "\n";
    } else {
        std::cout << "data.nonexistent = (not found, optional is empty)\n";
    }

    // -------------------------------------------------------
    // Summary
    // -------------------------------------------------------
    std::cout << "\n--- Summary ---\n";
    std::cout << "By implementing at(), set(), count(), size() on Value:\n";
    std::cout << "  1. Can use lager::lenses::at directly\n";
    std::cout << "  2. No need for custom Path, key_lens(), index_lens()\n";
    std::cout << "  3. Returns optional<Value> for safe access\n";
    std::cout << "  4. Works with both string keys and numeric indices\n";
    std::cout << "\nTrade-offs:\n";
    std::cout << "  - Nested access requires chaining optionals\n";
    std::cout << "  - Custom path_lens provides more ergonomic API for deep paths\n";
    std::cout << "\n=== Demo End ===\n\n";
}

} // namespace lager_ext
