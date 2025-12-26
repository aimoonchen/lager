// main.cpp
// Path Lens Example - Demonstrating multiple approaches for dynamic data access
//
// This example showcases four different approaches for working with
// JSON-like dynamic data stored in immer containers:
//
// Scheme 1: Custom ErasedLens using std::function
// Scheme 2: lager::lens<Value, Value> using lager's built-in type erasure
// Scheme 3: lager::lenses::at with Value's container interface
// Scheme 4: String Path API for familiar path syntax
// Scheme 5: Static Path (compile-time paths for known schemas)
//
// All schemes work with the same Value type defined in value.h

#include <lager_ext/value.h>
#include <lager_ext/erased_lens.h>
#include <lager_ext/lager_lens.h>
#include <lager_ext/at_lens.h>
#include <lager_ext/string_path.h>
#include <lager_ext/static_path.h>
#include <lager_ext/diff_collector.h>
#include <lager_ext/shared_state.h>
#include <lager_ext/editor_engine.h>

#include <lager/store.hpp>
#include <lager/event_loop/manual.hpp>

#include <iostream>
#include <string>

using namespace lager_ext;

// ============================================================
// Application State and Actions
// ============================================================

struct AddItem
{
    std::string text;
};

struct UpdateItem
{
    Path path;
    std::string new_value;
};

struct Undo {};
struct Redo {};

using Action = std::variant<AddItem, UpdateItem, Undo, Redo>;

struct AppState
{
    Value data;
    immer::vector<Value> history;
    immer::vector<Value> future;
};

// ============================================================
// Initial State Factory
// ============================================================

AppState create_initial_state()
{
    auto item1 = Value{
        ValueMap{{"title", immer::box<Value>{"Task 1"}},
                 {"done", immer::box<Value>{false}}}};

    auto items = Value{ValueVector{immer::box<Value>{std::move(item1)}}};

    auto root = Value{ValueMap{{"items", immer::box<Value>{std::move(items)}}}};

    return AppState{
        .data    = std::move(root),
        .history = immer::vector<Value>{},
        .future  = immer::vector<Value>{}
    };
}

// ============================================================
// Reducer
// ============================================================

AppState reducer(AppState state, Action action)
{
    return std::visit(
        [&](auto&& act) -> AppState {
            using T = std::decay_t<decltype(act)>;

            if constexpr (std::is_same_v<T, Undo>) {
                if (state.history.empty())
                    return state;

                auto new_state = state;
                auto previous  = new_state.history.back();

                new_state.future = new_state.future.push_back(new_state.data);
                new_state.data   = previous;
                new_state.history = new_state.history.take(new_state.history.size() - 1);

                return new_state;

            } else if constexpr (std::is_same_v<T, Redo>) {
                if (state.future.empty())
                    return state;

                auto new_state = state;
                auto next      = new_state.future.back();

                new_state.history = new_state.history.push_back(new_state.data);
                new_state.data    = next;
                new_state.future = new_state.future.take(new_state.future.size() - 1);

                return new_state;

            } else if constexpr (std::is_same_v<T, AddItem>) {
                auto new_state    = state;
                new_state.history = new_state.history.push_back(state.data);
                new_state.future  = immer::vector<Value>{};

                // Use ErasedLens (Scheme 1) for this operation
                Path items_path = {std::string{"items"}};
                auto items_lens = path_lens(items_path);
                auto current_items = items_lens.get(new_state.data);

                if (auto* vec = current_items.get_if<ValueVector>()) {
                auto new_item = Value{
                    ValueMap{{"title", immer::box<Value>{act.text}},
                             {"done", immer::box<Value>{false}}}};

                    auto new_vec = vec->push_back(immer::box<Value>{std::move(new_item)});
                    new_state.data = items_lens.set(new_state.data, Value{std::move(new_vec)});
                }

                return new_state;

            } else if constexpr (std::is_same_v<T, UpdateItem>) {
                auto new_state    = state;
                new_state.history = new_state.history.push_back(state.data);
                new_state.future  = immer::vector<Value>{};

                // Use ErasedLens (Scheme 1) for this operation
                auto lens      = path_lens(act.path);
                auto new_value = Value{act.new_value};
                new_state.data = lens.set(new_state.data, std::move(new_value));

                return new_state;
            }

            return state;
        },
        action);
}

// ============================================================
// Main Application
// ============================================================

int main()
{
    auto loop  = lager::with_manual_event_loop{};
    auto store = lager::make_store<Action>(
        create_initial_state(),
        loop,
        lager::with_reducer(reducer)
    );

    std::cout << "=== Path Lens Example ===\n";
    std::cout << "Demonstrating 5 schemes for dynamic data access\n\n";

    while (true) {
        std::cout << "Current data:\n";
        print_value(store.get().data, "", 1);

        std::cout << "\n=== Operations ===\n";
        std::cout << "1. Add item\n";
        std::cout << "2. Update item\n";
        std::cout << "U. Undo\n";
        std::cout << "R. Redo\n";
        std::cout << "\n=== Scheme Demos ===\n";
        std::cout << "E. Scheme 1: Custom ErasedLens\n";
        std::cout << "L. Scheme 2: lager::lens<Value, Value>\n";
        std::cout << "A. Scheme 3: lager::lenses::at\n";
        std::cout << "J. Scheme 4: String Path API\n";
        std::cout << "S. Scheme 5: Static Path (compile-time)\n";
        std::cout << "\n=== Diff Demos ===\n";
        std::cout << "D. Demo immer::diff (basic)\n";
        std::cout << "C. Demo RecursiveDiffCollector\n";
        std::cout << "\n=== Cross-Process ===\n";
        std::cout << "P. Demo Shared State (Publisher/Subscriber)\n";
        std::cout << "\n=== Editor-Engine Demo ===\n";
        std::cout << "G. Demo Editor-Engine (Full Flow)\n";
        std::cout << "H. Demo Property Editing\n";
        std::cout << "I. Demo Undo/Redo\n";
        std::cout << "\nQ. Quit\n";
        std::cout << "\nChoice: ";

        char choice;
        std::cin >> choice;
        std::cin.ignore();

        switch (choice) {
        case '1': {
            std::cout << "Enter item title: ";
            std::string title;
            std::getline(std::cin, title);
            store.dispatch(AddItem{title});
            break;
        }
        case '2': {
            std::cout << "Enter item index: ";
            size_t index;
            std::cin >> index;
            std::cin.ignore();

            std::cout << "Enter new title: ";
            std::string new_title;
            std::getline(std::cin, new_title);

            Path path = {std::string{"items"}, index, std::string{"title"}};
            store.dispatch(UpdateItem{path, new_title});
            break;
        }
        case 'U':
        case 'u':
            store.dispatch(Undo{});
            break;
        case 'R':
        case 'r':
            store.dispatch(Redo{});
            break;
        case 'E':
        case 'e':
            demo_erased_lens();
            break;
        case 'L':
        case 'l':
            demo_lager_lens();
            break;
        case 'A':
        case 'a':
            demo_at_lens();
            break;
        case 'J':
        case 'j':
            demo_string_path();
            break;
        case 'S':
        case 's':
            demo_static_path();
            break;
        case 'D':
        case 'd':
            demo_immer_diff();
            break;
        case 'C':
        case 'c':
            demo_recursive_diff_collector();
            break;
        case 'P':
        case 'p':
            demo_shared_state();
            break;
        case 'G':
        case 'g':
            demo_editor_engine();
            break;
        case 'H':
        case 'h':
            demo_property_editing();
            break;
        case 'I':
        case 'i':
            demo_undo_redo();
            break;
        case 'Q':
        case 'q':
            std::cout << "Goodbye!\n";
            return 0;
        default:
            std::cout << "Invalid choice!\n";
        }

        std::cout << "\n";
    }
}
