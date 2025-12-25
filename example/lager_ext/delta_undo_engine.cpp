// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

#include <lager_ext/delta_undo_engine.h>

#include <lager/store.hpp>
#include <lager/event_loop/manual.hpp>

#include <immer/map_transient.hpp>
#include <immer/flex_vector_transient.hpp>

#include <iostream>
#include <map>
#include <sstream>
#include <format>

namespace lager_ext {
namespace delta_undo {

// ============================================================
// Helper Functions
// ============================================================

namespace {

// Get a property value from an object using dot-separated path
// Uses Value's at() method which works for map-like containers
Value get_value_at_path(const Value& data, const std::string& path) {
    if (path.empty()) return data;

    Value current = data;
    std::istringstream stream(path);
    std::string segment;

    while (std::getline(stream, segment, '.')) {
        // Use Value's at(key) method - returns null Value if not found
        Value next = current.at(segment);
        if (next.is_null()) {
            return Value{};  // Path not found
        }
        current = next;
    }
    return current;
}

// Set a property value in an object using dot-separated path
// Uses Value's set() method which handles map-like containers
Value set_value_at_path(const Value& data, const std::string& path, const Value& new_value) {
    if (path.empty()) return new_value;

    std::vector<std::string> segments;
    std::istringstream stream(path);
    std::string segment;
    while (std::getline(stream, segment, '.')) {
        segments.push_back(segment);
    }

    // Recursive helper to build the new value tree
    std::function<Value(const Value&, size_t)> set_recursive;
    set_recursive = [&](const Value& current, size_t depth) -> Value {
        if (depth >= segments.size()) return new_value;

        const std::string& key = segments[depth];

        // Get child value (may be null if not exists)
        Value child_value = current.at(key);

        // Recursively set the value
        Value new_child = set_recursive(child_value, depth + 1);

        // Use Value's set() method to create updated map
        return current.set(key, new_child);
    };

    return set_recursive(data, 0);
}

} // anonymous namespace

// ============================================================
// DeltaFactory Implementation
// ============================================================

Delta DeltaFactory::create_set_property_delta(
    const std::string& object_id,
    const std::string& property_path,
    const Value& old_value,
    const Value& new_value)
{
    std::string desc = std::format("Set {}.{}", object_id, property_path);

    // Capture values by copy for the lambdas
    auto apply = [object_id, property_path, new_value](const SceneState& state) -> SceneState {
        auto obj_ptr = state.objects.find(object_id);
        if (!obj_ptr) return state;

        SceneObject updated_obj = *obj_ptr;
        updated_obj.data = set_value_at_path(updated_obj.data, property_path, new_value);

        return SceneState{
            state.objects.set(object_id, updated_obj),
            state.root_id,
            state.selected_id,
            state.version + 1
        };
    };

    auto unapply = [object_id, property_path, old_value](const SceneState& state) -> SceneState {
        auto obj_ptr = state.objects.find(object_id);
        if (!obj_ptr) return state;

        SceneObject updated_obj = *obj_ptr;
        updated_obj.data = set_value_at_path(updated_obj.data, property_path, old_value);

        return SceneState{
            state.objects.set(object_id, updated_obj),
            state.root_id,
            state.selected_id,
            state.version + 1
        };
    };

    return Delta(std::move(desc), std::move(apply), std::move(unapply));
}

Delta DeltaFactory::create_set_properties_delta(
    const std::string& object_id,
    const std::map<std::string, Value>& old_values,
    const std::map<std::string, Value>& new_values)
{
    std::string desc = std::format("Set {} properties on {}", new_values.size(), object_id);

    auto apply = [object_id, new_values](const SceneState& state) -> SceneState {
        auto obj_ptr = state.objects.find(object_id);
        if (!obj_ptr) return state;

        SceneObject updated_obj = *obj_ptr;
        for (const auto& [path, value] : new_values) {
            updated_obj.data = set_value_at_path(updated_obj.data, path, value);
        }

        return SceneState{
            state.objects.set(object_id, updated_obj),
            state.root_id,
            state.selected_id,
            state.version + 1
        };
    };

    auto unapply = [object_id, old_values](const SceneState& state) -> SceneState {
        auto obj_ptr = state.objects.find(object_id);
        if (!obj_ptr) return state;

        SceneObject updated_obj = *obj_ptr;
        for (const auto& [path, value] : old_values) {
            updated_obj.data = set_value_at_path(updated_obj.data, path, value);
        }

        return SceneState{
            state.objects.set(object_id, updated_obj),
            state.root_id,
            state.selected_id,
            state.version + 1
        };
    };

    return Delta(std::move(desc), std::move(apply), std::move(unapply));
}

Delta DeltaFactory::create_add_object_delta(
    const SceneObject& object,
    const std::string& parent_id)
{
    std::string desc = std::format("Add object '{}'", object.id);
    std::string obj_id = object.id;

    auto apply = [object, parent_id](const SceneState& state) -> SceneState {
        auto new_objects = state.objects.set(object.id, object);

        // Add to parent's children if parent exists
        if (!parent_id.empty()) {
            auto parent_ptr = state.objects.find(parent_id);
            if (parent_ptr) {
                SceneObject updated_parent = *parent_ptr;
                updated_parent.children.push_back(object.id);
                new_objects = new_objects.set(parent_id, updated_parent);
            }
        }

        return SceneState{new_objects, state.root_id, state.selected_id, state.version + 1};
    };

    auto unapply = [obj_id, parent_id](const SceneState& state) -> SceneState {
        auto new_objects = state.objects.erase(obj_id);

        // Remove from parent's children
        if (!parent_id.empty()) {
            auto parent_ptr = state.objects.find(parent_id);
            if (parent_ptr) {
                SceneObject updated_parent = *parent_ptr;
                auto& children = updated_parent.children;
                children.erase(
                    std::remove(children.begin(), children.end(), obj_id),
                    children.end());
                new_objects = new_objects.set(parent_id, updated_parent);
            }
        }

        return SceneState{new_objects, state.root_id, state.selected_id, state.version + 1};
    };

    return Delta(std::move(desc), std::move(apply), std::move(unapply));
}

Delta DeltaFactory::create_remove_object_delta(
    const SceneObject& object,
    const std::string& parent_id)
{
    std::string desc = std::format("Remove object '{}'", object.id);
    std::string obj_id = object.id;

    // Remove is the inverse of add
    auto apply = [obj_id, parent_id](const SceneState& state) -> SceneState {
        auto new_objects = state.objects.erase(obj_id);

        if (!parent_id.empty()) {
            auto parent_ptr = state.objects.find(parent_id);
            if (parent_ptr) {
                SceneObject updated_parent = *parent_ptr;
                auto& children = updated_parent.children;
                children.erase(
                    std::remove(children.begin(), children.end(), obj_id),
                    children.end());
                new_objects = new_objects.set(parent_id, updated_parent);
            }
        }

        return SceneState{new_objects, state.root_id, state.selected_id, state.version + 1};
    };

    auto unapply = [object, parent_id](const SceneState& state) -> SceneState {
        auto new_objects = state.objects.set(object.id, object);

        if (!parent_id.empty()) {
            auto parent_ptr = state.objects.find(parent_id);
            if (parent_ptr) {
                SceneObject updated_parent = *parent_ptr;
                updated_parent.children.push_back(object.id);
                new_objects = new_objects.set(parent_id, updated_parent);
            }
        }

        return SceneState{new_objects, state.root_id, state.selected_id, state.version + 1};
    };

    return Delta(std::move(desc), std::move(apply), std::move(unapply));
}

Delta DeltaFactory::compose_deltas(
    const std::string& description,
    const std::vector<Delta>& deltas)
{
    if (deltas.empty()) {
        return Delta();
    }

    if (deltas.size() == 1) {
        return Delta(description, deltas[0].apply_fn, deltas[0].unapply_fn);
    }

    // Apply all deltas in order
    auto apply = [deltas](const SceneState& state) -> SceneState {
        SceneState current = state;
        for (const auto& delta : deltas) {
            current = delta.apply_fn(current);
        }
        return current;
    };

    // Unapply in reverse order
    auto unapply = [deltas](const SceneState& state) -> SceneState {
        SceneState current = state;
        for (auto it = deltas.rbegin(); it != deltas.rend(); ++it) {
            current = it->unapply_fn(current);
        }
        return current;
    };

    return Delta(description, std::move(apply), std::move(unapply));
}

// ============================================================
// Reducer Implementation
// ============================================================

DeltaModel delta_update(DeltaModel model, DeltaAction action) {
    return std::visit([&model](auto&& act) -> DeltaModel {
        using T = std::decay_t<decltype(act)>;

        // ===== Control Actions =====

        if constexpr (std::is_same_v<T, actions::Undo>) {
            if (model.undo_stack.empty()) return model;

            // Get the last delta
            Delta delta = model.undo_stack.back();
            auto new_undo = model.undo_stack.take(model.undo_stack.size() - 1);

            // Apply unapply_fn to CURRENT state (key difference from snapshot!)
            SceneState new_scene = delta.unapply_fn(model.scene);

            // Push to redo stack
            auto new_redo = model.redo_stack.push_back(delta);

            return DeltaModel{
                new_scene,
                model.system,
                new_undo,
                new_redo,
                model.transaction_description,
                model.transaction_deltas,
                true
            };
        }

        else if constexpr (std::is_same_v<T, actions::Redo>) {
            if (model.redo_stack.empty()) return model;

            Delta delta = model.redo_stack.back();
            auto new_redo = model.redo_stack.take(model.redo_stack.size() - 1);

            // Apply apply_fn to CURRENT state
            SceneState new_scene = delta.apply_fn(model.scene);

            auto new_undo = model.undo_stack.push_back(delta);

            return DeltaModel{
                new_scene,
                model.system,
                new_undo,
                new_redo,
                model.transaction_description,
                model.transaction_deltas,
                true
            };
        }

        else if constexpr (std::is_same_v<T, actions::ClearHistory>) {
            return DeltaModel{
                model.scene,
                model.system,
                {},  // Clear undo
                {},  // Clear redo
                std::nullopt,
                {},
                false
            };
        }

        // ===== User Actions (create deltas) =====

        else if constexpr (std::is_same_v<T, actions::SetProperty>) {
            auto obj_ptr = model.scene.objects.find(act.object_id);
            if (!obj_ptr) return model;

            // Get old value for creating the delta
            Value old_value = get_value_at_path(obj_ptr->data, act.property_path);

            // Create delta
            Delta delta = DeltaFactory::create_set_property_delta(
                act.object_id, act.property_path, old_value, act.new_value);

            // Apply the change
            SceneState new_scene = delta.apply_fn(model.scene);

            // Handle transaction or direct push
            if (model.transaction_description.has_value()) {
                auto new_deltas = model.transaction_deltas;
                new_deltas.push_back(delta);
                return DeltaModel{
                    new_scene, model.system, model.undo_stack, {},
                    model.transaction_description, new_deltas, true
                };
            } else {
                auto new_undo = model.undo_stack.push_back(delta);
                if (new_undo.size() > DeltaModel::max_history) {
                    new_undo = new_undo.drop(1);
                }
                return DeltaModel{
                    new_scene, model.system, new_undo, {},
                    std::nullopt, {}, true
                };
            }
        }

        else if constexpr (std::is_same_v<T, actions::SetProperties>) {
            auto obj_ptr = model.scene.objects.find(act.object_id);
            if (!obj_ptr) return model;

            // Get old values
            std::map<std::string, Value> old_values;
            for (const auto& [path, _] : act.updates) {
                old_values[path] = get_value_at_path(obj_ptr->data, path);
            }

            Delta delta = DeltaFactory::create_set_properties_delta(
                act.object_id, old_values, act.updates);

            SceneState new_scene = delta.apply_fn(model.scene);

            if (model.transaction_description.has_value()) {
                auto new_deltas = model.transaction_deltas;
                new_deltas.push_back(delta);
                return DeltaModel{
                    new_scene, model.system, model.undo_stack, {},
                    model.transaction_description, new_deltas, true
                };
            } else {
                auto new_undo = model.undo_stack.push_back(delta);
                if (new_undo.size() > DeltaModel::max_history) {
                    new_undo = new_undo.drop(1);
                }
                return DeltaModel{
                    new_scene, model.system, new_undo, {},
                    std::nullopt, {}, true
                };
            }
        }

        else if constexpr (std::is_same_v<T, actions::AddObject>) {
            Delta delta = DeltaFactory::create_add_object_delta(act.object, act.parent_id);
            SceneState new_scene = delta.apply_fn(model.scene);

            if (model.transaction_description.has_value()) {
                auto new_deltas = model.transaction_deltas;
                new_deltas.push_back(delta);
                return DeltaModel{
                    new_scene, model.system, model.undo_stack, {},
                    model.transaction_description, new_deltas, true
                };
            } else {
                auto new_undo = model.undo_stack.push_back(delta);
                if (new_undo.size() > DeltaModel::max_history) {
                    new_undo = new_undo.drop(1);
                }
                return DeltaModel{
                    new_scene, model.system, new_undo, {},
                    std::nullopt, {}, true
                };
            }
        }

        else if constexpr (std::is_same_v<T, actions::RemoveObject>) {
            auto obj_ptr = model.scene.objects.find(act.object_id);
            if (!obj_ptr) return model;

            // Find parent (simplified - assume root-level objects or search)
            std::string parent_id;
            for (const auto& [id, obj] : model.scene.objects) {
                for (const auto& child : obj.children) {
                    if (child == act.object_id) {
                        parent_id = id;
                        break;
                    }
                }
                if (!parent_id.empty()) break;
            }

            Delta delta = DeltaFactory::create_remove_object_delta(*obj_ptr, parent_id);
            SceneState new_scene = delta.apply_fn(model.scene);

            if (model.transaction_description.has_value()) {
                auto new_deltas = model.transaction_deltas;
                new_deltas.push_back(delta);
                return DeltaModel{
                    new_scene, model.system, model.undo_stack, {},
                    model.transaction_description, new_deltas, true
                };
            } else {
                auto new_undo = model.undo_stack.push_back(delta);
                if (new_undo.size() > DeltaModel::max_history) {
                    new_undo = new_undo.drop(1);
                }
                return DeltaModel{
                    new_scene, model.system, new_undo, {},
                    std::nullopt, {}, true
                };
            }
        }

        else if constexpr (std::is_same_v<T, actions::BeginTransaction>) {
            return DeltaModel{
                model.scene, model.system, model.undo_stack, model.redo_stack,
                act.description, {}, model.dirty
            };
        }

        else if constexpr (std::is_same_v<T, actions::EndTransaction>) {
            if (!model.transaction_description.has_value()) return model;

            if (model.transaction_deltas.empty()) {
                return DeltaModel{
                    model.scene, model.system, model.undo_stack, model.redo_stack,
                    std::nullopt, {}, model.dirty
                };
            }

            // Compose all transaction deltas into one
            Delta compound = DeltaFactory::compose_deltas(
                *model.transaction_description,
                model.transaction_deltas);

            auto new_undo = model.undo_stack.push_back(compound);
            if (new_undo.size() > DeltaModel::max_history) {
                new_undo = new_undo.drop(1);
            }

            return DeltaModel{
                model.scene, model.system, new_undo, {},
                std::nullopt, {}, true
            };
        }

        // ===== System Actions (NO deltas created) =====

        else if constexpr (std::is_same_v<T, actions::SelectObject>) {
            SceneState new_scene = model.scene;
            new_scene.selected_id = act.object_id;
            // No delta - selection persists through undo/redo
            return DeltaModel{
                new_scene, model.system, model.undo_stack, model.redo_stack,
                model.transaction_description, model.transaction_deltas, model.dirty
            };
        }

        else if constexpr (std::is_same_v<T, actions::SyncFromEngine>) {
            // Replace scene state entirely - no delta
            return DeltaModel{
                act.new_state, model.system, model.undo_stack, model.redo_stack,
                model.transaction_description, model.transaction_deltas, true
            };
        }

        else if constexpr (std::is_same_v<T, actions::LoadObjects>) {
            // Add objects without creating delta - they persist through undo
            auto trans = model.scene.objects.transient();
            for (const auto& obj : act.objects) {
                trans.set(obj.id, obj);
            }

            SceneState new_scene = model.scene;
            new_scene.objects = trans.persistent();
            new_scene.version++;

            // No delta - loaded objects persist through undo/redo!
            return DeltaModel{
                new_scene, model.system, model.undo_stack, model.redo_stack,
                model.transaction_description, model.transaction_deltas, true
            };
        }

        else if constexpr (std::is_same_v<T, actions::SetSystemState>) {
            SystemState new_system{
                act.is_loading,
                act.progress,
                act.status_message
            };
            // System state never creates deltas
            return DeltaModel{
                model.scene, new_system, model.undo_stack, model.redo_stack,
                model.transaction_description, model.transaction_deltas, model.dirty
            };
        }

        return model;

    }, action);
}

// Store type deduction helper
inline auto make_delta_store_impl(DeltaModel initial) {
    return lager::make_store<DeltaAction>(
        std::move(initial),
        lager::with_manual_event_loop{},
        lager::with_reducer(delta_update)
    );
}

using DeltaStoreType = decltype(make_delta_store_impl(std::declval<DeltaModel>()));

// ============================================================
// DeltaController Implementation
// ============================================================

struct DeltaController::Impl {
    std::unique_ptr<DeltaStoreType> store;
    std::vector<std::function<void()>> unsubscribers;

    Impl() = default;
};

DeltaController::DeltaController() : impl_(std::make_unique<Impl>()) {}
DeltaController::~DeltaController() {
    for (auto& unsub : impl_->unsubscribers) {
        unsub();
    }
}

void DeltaController::initialize(const SceneState& initial_state) {
    DeltaModel initial{initial_state, {}, {}, {}, std::nullopt, {}, false};
    impl_->store = std::make_unique<DeltaStoreType>(make_delta_store_impl(std::move(initial)));
}

void DeltaController::dispatch(DeltaAction action) {
    if (impl_->store) {
        impl_->store->dispatch(std::move(action));
    }
}

const DeltaModel& DeltaController::get_model() const {
    return impl_->store->get();
}

const SceneState& DeltaController::get_scene() const {
    return get_model().scene;
}

const SceneObject* DeltaController::get_object(const std::string& id) const {
    return get_scene().objects.find(id);
}

const SceneObject* DeltaController::get_selected_object() const {
    const auto& scene = get_scene();
    if (scene.selected_id.empty()) return nullptr;
    return scene.objects.find(scene.selected_id);
}

Value DeltaController::get_property(const std::string& object_id, const std::string& path) const {
    auto obj = get_object(object_id);
    if (!obj) return Value{};
    return get_value_at_path(obj->data, path);
}

void DeltaController::set_property(const std::string& object_id, const std::string& path, Value value) {
    dispatch(actions::SetProperty{object_id, path, std::move(value)});
}

void DeltaController::set_properties(const std::string& object_id, const std::map<std::string, Value>& updates) {
    dispatch(actions::SetProperties{object_id, updates});
}

void DeltaController::begin_transaction(const std::string& description) {
    dispatch(actions::BeginTransaction{description});
}

void DeltaController::end_transaction() {
    dispatch(actions::EndTransaction{});
}

bool DeltaController::can_undo() const {
    return !get_model().undo_stack.empty();
}

bool DeltaController::can_redo() const {
    return !get_model().redo_stack.empty();
}

std::string DeltaController::get_undo_description() const {
    if (!can_undo()) return "";
    return get_model().undo_stack.back().description;
}

std::string DeltaController::get_redo_description() const {
    if (!can_redo()) return "";
    return get_model().redo_stack.back().description;
}

void DeltaController::undo() {
    dispatch(actions::Undo{});
}

void DeltaController::redo() {
    dispatch(actions::Redo{});
}

std::size_t DeltaController::undo_count() const {
    return get_model().undo_stack.size();
}

std::size_t DeltaController::redo_count() const {
    return get_model().redo_stack.size();
}

void DeltaController::clear_history() {
    dispatch(actions::ClearHistory{});
}

void DeltaController::step() {
    if (impl_->store) {
        // Manual event loop doesn't need explicit step
    }
}

std::function<void()> DeltaController::watch(WatchCallback callback) {
    if (!impl_->store) return [](){};

    // Simplified implementation - just call callback with current state
    // In production, would use lager's watch mechanism properly
    callback(get_model());

    // Return a no-op unsubscriber for now
    // Full implementation would track and remove the watcher
    return [](){};
}

// ============================================================
// Demo Functions
// ============================================================

// Helper to get double value from Value
double get_double_value(const Value& v) {
    if (auto* d = v.get_if<double>()) return *d;
    if (auto* i = v.get_if<int>()) return static_cast<double>(*i);
    return 0.0;
}

void demo_delta_undo_basic() {
    std::cout << "\n========== Delta Undo Basic Demo ==========\n";

    // Create initial scene using Value::object factory
    SceneState initial;
    initial.root_id = "root";
    initial.objects = initial.objects.set("obj1", SceneObject{
        "obj1", "Transform",
        Value::object({{"x", 0.0}, {"y", 0.0}}),
        {}, {}
    });

    DeltaController controller;
    controller.initialize(initial);

    auto print_state = [&]() {
        auto obj = controller.get_object("obj1");
        if (obj) {
            double x = get_double_value(obj->data.at("x"));
            double y = get_double_value(obj->data.at("y"));
            std::cout << "  obj1.x = " << x << ", y = " << y << "\n";
        }
        std::cout << "  Undo count: " << controller.undo_count()
                  << ", Redo count: " << controller.redo_count() << "\n";
    };

    std::cout << "Initial state:\n";
    print_state();

    // User action 1: Set x = 10
    std::cout << "\n[User] Set x = 10\n";
    controller.set_property("obj1", "x", Value(10.0));
    print_state();

    // User action 2: Set y = 20
    std::cout << "\n[User] Set y = 20\n";
    controller.set_property("obj1", "y", Value(20.0));
    print_state();

    // Undo
    std::cout << "\n[Undo] '" << controller.get_undo_description() << "'\n";
    controller.undo();
    print_state();

    // Undo again
    std::cout << "\n[Undo] '" << controller.get_undo_description() << "'\n";
    controller.undo();
    print_state();

    // Redo
    std::cout << "\n[Redo] '" << controller.get_redo_description() << "'\n";
    controller.redo();
    print_state();

    std::cout << "\n========== Demo Complete ==========\n";
}

void demo_system_persistence() {
    std::cout << "\n========== System Persistence Demo ==========\n";
    std::cout << "This demo shows that system operations persist through undo/redo.\n\n";

    // Create initial scene with one object
    SceneState initial;
    initial.root_id = "root";
    initial.objects = initial.objects.set("obj1", SceneObject{
        "obj1", "Transform",
        Value::object({{"x", 0.0}}),
        {}, {}
    });

    DeltaController controller;
    controller.initialize(initial);

    auto print_state = [&]() {
        std::cout << "  Objects in scene: ";
        for (const auto& [id, obj] : controller.get_scene().objects) {
            auto x_val = get_value_at_path(obj.data, "x");
            std::cout << id << "(x=" << get_double_value(x_val) << ") ";
        }
        std::cout << "\n  Undo stack: " << controller.undo_count() << "\n";
    };

    std::cout << "Initial state:\n";
    print_state();

    // T1: User modifies obj1.x = 10 (recorded)
    std::cout << "\n[T1] User sets obj1.x = 10\n";
    controller.set_property("obj1", "x", Value(10.0));
    print_state();

    // T2: System loads new object obj2 (NOT recorded - simulates lazy load)
    std::cout << "\n[T2] System loads obj2 (lazy load - NOT recorded)\n";
    controller.dispatch(actions::LoadObjects{{
        SceneObject{"obj2", "Light",
            Value::object({{"x", 100.0}}),
            {}, {}}
    }});
    print_state();

    // T3: User modifies obj1.x = 20 (recorded)
    std::cout << "\n[T3] User sets obj1.x = 20\n";
    controller.set_property("obj1", "x", Value(20.0));
    print_state();

    // Now undo T3
    std::cout << "\n[Undo T3] - Should restore obj1.x=10, but KEEP obj2!\n";
    controller.undo();
    print_state();

    // Undo T1
    std::cout << "\n[Undo T1] - Should restore obj1.x=0, but STILL KEEP obj2!\n";
    controller.undo();
    print_state();

    std::cout << "\n*** SUCCESS: obj2 persisted through all undo operations! ***\n";
    std::cout << "This is because system operations (LoadObjects) modify state\n";
    std::cout << "without creating deltas. When we undo, we apply the inverse\n";
    std::cout << "transformation to the CURRENT state, not restore a snapshot.\n";

    std::cout << "\n========== Demo Complete ==========\n";
}

void demo_transactions() {
    std::cout << "\n========== Transaction Demo ==========\n";
    std::cout << "Transactions group multiple operations into a single undo step.\n\n";

    SceneState initial;
    initial.objects = initial.objects.set("obj1", SceneObject{
        "obj1", "Transform",
        Value::object({{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}),
        {}, {}
    });

    DeltaController controller;
    controller.initialize(initial);

    auto print_state = [&]() {
        auto obj = controller.get_object("obj1");
        if (obj) {
            double x = get_double_value(obj->data.at("x"));
            double y = get_double_value(obj->data.at("y"));
            double z = get_double_value(obj->data.at("z"));
            std::cout << "  obj1: x=" << x << ", y=" << y << ", z=" << z << "\n";
        }
        std::cout << "  Undo stack size: " << controller.undo_count() << "\n";
    };

    std::cout << "Initial:\n";
    print_state();

    // Transaction: Move object (changes x, y, z together)
    std::cout << "\n[Begin Transaction: 'Move object']\n";
    controller.begin_transaction("Move object");
    controller.set_property("obj1", "x", Value(10.0));
    controller.set_property("obj1", "y", Value(20.0));
    controller.set_property("obj1", "z", Value(30.0));
    controller.end_transaction();
    std::cout << "[End Transaction]\n";
    print_state();

    std::cout << "\nNote: 3 property changes = 1 undo entry!\n";

    // Single undo reverts all three changes
    std::cout << "\n[Undo] '" << controller.get_undo_description() << "'\n";
    controller.undo();
    print_state();

    std::cout << "\n========== Demo Complete ==========\n";
}

void demo_interleaved_operations() {
    std::cout << "\n========== Interleaved Operations Demo ==========\n";
    std::cout << "Complex scenario mixing user and system operations.\n\n";

    SceneState initial;
    initial.objects = initial.objects.set("player", SceneObject{
        "player", "Character",
        Value::object({{"health", 100.0}, {"score", 0.0}}),
        {}, {}
    });

    DeltaController controller;
    controller.initialize(initial);

    auto print_state = [&]() {
        std::cout << "  State: ";
        for (const auto& [id, obj] : controller.get_scene().objects) {
            std::cout << id << "{";
            // Print health if exists
            Value health = obj.data.at("health");
            if (!health.is_null()) {
                std::cout << "health=" << get_double_value(health);
            }
            // Print score if exists
            Value score = obj.data.at("score");
            if (!score.is_null()) {
                if (!health.is_null()) std::cout << ", ";
                std::cout << "score=" << get_double_value(score);
            }
            std::cout << "} ";
        }
        std::cout << "\n";
    };

    std::cout << "Initial:\n";
    print_state();

    std::cout << "\n[User] Player takes 20 damage (health = 80)\n";
    controller.set_property("player", "health", Value(80.0));
    print_state();

    std::cout << "\n[System] Enemy spawns (lazy loaded)\n";
    controller.dispatch(actions::LoadObjects{{
        SceneObject{"enemy1", "Enemy",
            Value::object({{"health", 50.0}}),
            {}, {}}
    }});
    print_state();

    std::cout << "\n[User] Player scores 100 points\n";
    controller.set_property("player", "score", Value(100.0));
    print_state();

    std::cout << "\n[System] Another enemy spawns\n";
    controller.dispatch(actions::LoadObjects{{
        SceneObject{"enemy2", "Enemy",
            Value::object({{"health", 75.0}}),
            {}, {}}
    }});
    print_state();

    std::cout << "\n[User] Player takes more damage (health = 50)\n";
    controller.set_property("player", "health", Value(50.0));
    print_state();

    std::cout << "\n--- Now undoing all user actions ---\n";
    std::cout << "Enemies should remain (system state persists)!\n\n";

    while (controller.can_undo()) {
        std::cout << "[Undo] '" << controller.get_undo_description() << "'\n";
        controller.undo();
        print_state();
    }

    std::cout << "\n*** Both enemies still exist after undoing all user actions! ***\n";
    std::cout << "\n========== Demo Complete ==========\n";
}

} // namespace delta_undo
} // namespace lager_ext
