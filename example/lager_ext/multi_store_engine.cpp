// multi_store_engine.cpp
// Implementation of Multi-Store Architecture with External UndoManager

#include <lager_ext/multi_store_engine.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace lager_ext {
namespace multi_store {

// ============================================================
// Object Reducer Implementation
// ============================================================

ObjectState object_update(ObjectState state, ObjectAction action) {
    return std::visit([&state](auto&& act) -> ObjectState {
        using T = std::decay_t<decltype(act)>;

        if constexpr (std::is_same_v<T, object_actions::SetProperty>) {
            // Get current data as map
            auto* map_ptr = state.data.get_if<ValueMap>();
            if (!map_ptr) {
                // Initialize as empty map if not a map
                state.data = Value{ValueMap{}};
                map_ptr = state.data.get_if<ValueMap>();
            }

            // Set the property
            state.data = Value{map_ptr->set(act.property_name,
                                            immer::box<Value>(act.new_value))};
            state.version++;
            return state;
        }
        else if constexpr (std::is_same_v<T, object_actions::SetProperties>) {
            auto* map_ptr = state.data.get_if<ValueMap>();
            if (!map_ptr) {
                state.data = Value{ValueMap{}};
                map_ptr = state.data.get_if<ValueMap>();
            }

            // Use transient for efficient batch update
            auto trans = map_ptr->transient();
            for (const auto& [name, value] : act.properties) {
                trans.set(name, immer::box<Value>(value));
            }
            state.data = Value{trans.persistent()};
            state.version++;
            return state;
        }
        else if constexpr (std::is_same_v<T, object_actions::ReplaceData>) {
            state.data = act.new_data;
            state.version++;
            return state;
        }
        else if constexpr (std::is_same_v<T, object_actions::RestoreState>) {
            // Complete state restoration for undo/redo
            return act.state;
        }

        return state;
    }, action);
}

// ============================================================
// Scene Reducer Implementation
// ============================================================

SceneMetaState scene_update(SceneMetaState state, SceneAction action) {
    return std::visit([&state](auto&& act) -> SceneMetaState {
        using T = std::decay_t<decltype(act)>;

        if constexpr (std::is_same_v<T, scene_actions::SelectObject>) {
            state.selected_id = act.object_id;
            return state;
        }
        else if constexpr (std::is_same_v<T, scene_actions::RegisterObject>) {
            state.object_ids.insert(act.object_id);
            state.version++;
            return state;
        }
        else if constexpr (std::is_same_v<T, scene_actions::UnregisterObject>) {
            state.object_ids.erase(act.object_id);
            if (state.selected_id == act.object_id) {
                state.selected_id.clear();
            }
            state.version++;
            return state;
        }

        return state;
    }, action);
}

// ============================================================
// UndoManager Implementation
// ============================================================

void UndoManager::begin_transaction(const std::string& description) {
    if (transaction_active_) {
        std::cerr << "[UndoManager] Warning: begin_transaction called while "
                  << "already in transaction. Ending previous transaction.\n";
        end_transaction();
    }

    transaction_active_ = true;
    current_transaction_ = CompositeCommand{};
    current_transaction_.description = description;
}

void UndoManager::record(UndoCommand cmd) {
    if (transaction_active_) {
        // Add to current transaction
        current_transaction_.sub_commands.push_back(std::move(cmd));
    } else {
        // Create single-command transaction and push immediately
        CompositeCommand composite;
        composite.description = cmd.description;
        composite.sub_commands.push_back(std::move(cmd));

        undo_stack_.push_back(std::move(composite));
        redo_stack_.clear();  // Clear redo on new action

        trim_history();
    }
}

void UndoManager::end_transaction() {
    if (!transaction_active_) {
        std::cerr << "[UndoManager] Warning: end_transaction called "
                  << "without active transaction.\n";
        return;
    }

    transaction_active_ = false;

    if (!current_transaction_.empty()) {
        undo_stack_.push_back(std::move(current_transaction_));
        redo_stack_.clear();
        trim_history();
    }

    current_transaction_ = CompositeCommand{};
}

void UndoManager::cancel_transaction() {
    if (!transaction_active_) {
        return;
    }

    // Restore all recorded states in reverse order
    auto& cmds = current_transaction_.sub_commands;
    for (auto it = cmds.rbegin(); it != cmds.rend(); ++it) {
        if (it->restore_fn) {
            it->restore_fn(it->old_state);
        }
    }

    transaction_active_ = false;
    current_transaction_ = CompositeCommand{};
}

bool UndoManager::undo() {
    if (undo_stack_.empty()) {
        return false;
    }

    CompositeCommand cmd = std::move(undo_stack_.back());
    undo_stack_.pop_back();

    // Restore old states in reverse order
    for (auto it = cmd.sub_commands.rbegin(); it != cmd.sub_commands.rend(); ++it) {
        if (it->restore_fn) {
            it->restore_fn(it->old_state);
        }
    }

    // Move to redo stack
    redo_stack_.push_back(std::move(cmd));

    return true;
}

bool UndoManager::redo() {
    if (redo_stack_.empty()) {
        return false;
    }

    CompositeCommand cmd = std::move(redo_stack_.back());
    redo_stack_.pop_back();

    // Apply new states in forward order
    for (auto& sub_cmd : cmd.sub_commands) {
        if (sub_cmd.restore_fn) {
            sub_cmd.restore_fn(sub_cmd.new_state);
        }
    }

    // Move to undo stack
    undo_stack_.push_back(std::move(cmd));

    return true;
}

std::optional<std::string> UndoManager::next_undo_description() const {
    if (undo_stack_.empty()) {
        return std::nullopt;
    }
    return undo_stack_.back().description;
}

std::optional<std::string> UndoManager::next_redo_description() const {
    if (redo_stack_.empty()) {
        return std::nullopt;
    }
    return redo_stack_.back().description;
}

void UndoManager::clear() {
    undo_stack_.clear();
    redo_stack_.clear();
    transaction_active_ = false;
    current_transaction_ = CompositeCommand{};
}

void UndoManager::trim_history() {
    if (max_history_ > 0 && undo_stack_.size() > max_history_) {
        undo_stack_.erase(
            undo_stack_.begin(),
            undo_stack_.begin() + (undo_stack_.size() - max_history_)
        );
    }
}

// ============================================================
// StoreRegistry Implementation
// ============================================================

ObjectStoreType* StoreRegistry::get(const std::string& object_id) {
    auto it = stores_.find(object_id);
    if (it != stores_.end()) {
        return it->second.get();
    }
    return nullptr;
}

ObjectStoreType* StoreRegistry::create(const std::string& object_id,
                                        ObjectState initial_state) {
    if (stores_.count(object_id) > 0) {
        std::cerr << "[StoreRegistry] Warning: store already exists for: "
                  << object_id << "\n";
        return stores_[object_id].get();
    }

    auto store = std::make_unique<ObjectStoreType>(
        make_object_store_impl(std::move(initial_state))
    );

    auto* ptr = store.get();
    stores_[object_id] = std::move(store);
    return ptr;
}

bool StoreRegistry::remove(const std::string& object_id) {
    return stores_.erase(object_id) > 0;
}

bool StoreRegistry::exists(const std::string& object_id) const {
    return stores_.count(object_id) > 0;
}

std::vector<std::string> StoreRegistry::all_ids() const {
    std::vector<std::string> ids;
    ids.reserve(stores_.size());
    for (const auto& [id, _] : stores_) {
        ids.push_back(id);
    }
    return ids;
}

// ============================================================
// MultiStoreController Implementation
// ============================================================

MultiStoreController::MultiStoreController()
    : scene_store_(std::make_unique<SceneStoreType>(
          make_scene_store_impl(SceneMetaState{})))
{
}

const SceneMetaState& MultiStoreController::get_scene_state() const {
    return scene_store_->get();
}

void MultiStoreController::add_object(const std::string& id,
                                      const std::string& type,
                                      Value initial_data,
                                      bool undoable) {
    if (registry_.exists(id)) {
        std::cerr << "[MultiStoreController] Object already exists: " << id << "\n";
        return;
    }

    ObjectState state{id, type, std::move(initial_data), 0};

    if (undoable) {
        // Record the action for undo
        UndoCommand cmd;
        cmd.store_id = id;
        cmd.description = "Add " + type + ": " + id;
        cmd.old_state = std::any{};  // Object didn't exist before
        cmd.new_state = state;
        cmd.restore_fn = [this, id](const std::any& state_any) {
            if (state_any.has_value()) {
                // Redo: recreate the object
                auto obj_state = std::any_cast<ObjectState>(state_any);
                if (!registry_.exists(id)) {
                    registry_.create(id, obj_state);
                    scene_store_->dispatch(scene_actions::RegisterObject{id});
                }
            } else {
                // Undo: remove the object
                registry_.remove(id);
                scene_store_->dispatch(scene_actions::UnregisterObject{id});
            }
        };

        undo_manager_.record(std::move(cmd));
    }

    // Create the store
    registry_.create(id, state);
    scene_store_->dispatch(scene_actions::RegisterObject{id});
}

void MultiStoreController::remove_object(const std::string& id, bool undoable) {
    auto* store = registry_.get(id);
    if (!store) {
        std::cerr << "[MultiStoreController] Object not found: " << id << "\n";
        return;
    }

    if (undoable) {
        ObjectState old_state = store->get();

        UndoCommand cmd;
        cmd.store_id = id;
        cmd.description = "Remove " + old_state.type + ": " + id;
        cmd.old_state = old_state;
        cmd.new_state = std::any{};  // Object will not exist
        cmd.restore_fn = [this, id](const std::any& state_any) {
            if (state_any.has_value()) {
                // Undo: restore the object
                auto obj_state = std::any_cast<ObjectState>(state_any);
                if (!registry_.exists(id)) {
                    registry_.create(id, obj_state);
                    scene_store_->dispatch(scene_actions::RegisterObject{id});
                }
            } else {
                // Redo: remove again
                registry_.remove(id);
                scene_store_->dispatch(scene_actions::UnregisterObject{id});
            }
        };

        undo_manager_.record(std::move(cmd));
    }

    registry_.remove(id);
    scene_store_->dispatch(scene_actions::UnregisterObject{id});
}

const ObjectState* MultiStoreController::get_object(const std::string& id) const {
    // Note: We need mutable access to call get() on store
    auto* store = const_cast<StoreRegistry&>(registry_).get(id);
    if (store) {
        // Return pointer to current state
        // This is a bit tricky - we return address of temporary
        // In practice, you'd want a different approach
        static thread_local ObjectState cached_state;
        cached_state = store->get();
        return &cached_state;
    }
    return nullptr;
}

std::vector<std::string> MultiStoreController::get_all_object_ids() const {
    return const_cast<StoreRegistry&>(registry_).all_ids();
}

void MultiStoreController::set_property(const std::string& object_id,
                                        const std::string& property_name,
                                        Value new_value,
                                        bool undoable) {
    auto* store = registry_.get(object_id);
    if (!store) {
        std::cerr << "[MultiStoreController] Object not found: " << object_id << "\n";
        return;
    }

    if (undoable) {
        ObjectState old_state = store->get();

        // Dispatch the action first to get new state
        store->dispatch(object_actions::SetProperty{property_name, new_value});
        ObjectState new_state = store->get();

        UndoCommand cmd;
        cmd.store_id = object_id;
        cmd.description = "Set " + property_name + " on " + object_id;
        cmd.old_state = old_state;
        cmd.new_state = new_state;
        cmd.restore_fn = make_object_restore_fn(object_id);

        undo_manager_.record(std::move(cmd));
    } else {
        store->dispatch(object_actions::SetProperty{property_name, new_value});
    }
}

void MultiStoreController::set_properties(
    const std::string& object_id,
    const std::vector<std::pair<std::string, Value>>& properties,
    bool undoable) {

    auto* store = registry_.get(object_id);
    if (!store) {
        std::cerr << "[MultiStoreController] Object not found: " << object_id << "\n";
        return;
    }

    if (undoable) {
        ObjectState old_state = store->get();

        store->dispatch(object_actions::SetProperties{properties});
        ObjectState new_state = store->get();

        UndoCommand cmd;
        cmd.store_id = object_id;
        cmd.description = "Set " + std::to_string(properties.size()) +
                         " properties on " + object_id;
        cmd.old_state = old_state;
        cmd.new_state = new_state;
        cmd.restore_fn = make_object_restore_fn(object_id);

        undo_manager_.record(std::move(cmd));
    } else {
        store->dispatch(object_actions::SetProperties{properties});
    }
}

void MultiStoreController::batch_edit(
    const std::vector<std::tuple<std::string, std::string, Value>>& edits,
    bool undoable) {

    if (edits.empty()) return;

    if (undoable) {
        undo_manager_.begin_transaction("Batch edit " +
            std::to_string(edits.size()) + " properties");
    }

    // Group edits by object
    std::unordered_map<std::string, std::vector<std::pair<std::string, Value>>> grouped;
    for (const auto& [obj_id, prop, val] : edits) {
        grouped[obj_id].emplace_back(prop, val);
    }

    // Apply grouped edits
    for (const auto& [obj_id, props] : grouped) {
        auto* store = registry_.get(obj_id);
        if (!store) continue;

        if (undoable) {
            ObjectState old_state = store->get();
            store->dispatch(object_actions::SetProperties{props});
            ObjectState new_state = store->get();

            UndoCommand cmd;
            cmd.store_id = obj_id;
            cmd.description = "";  // Part of transaction
            cmd.old_state = old_state;
            cmd.new_state = new_state;
            cmd.restore_fn = make_object_restore_fn(obj_id);

            undo_manager_.record(std::move(cmd));
        } else {
            store->dispatch(object_actions::SetProperties{props});
        }
    }

    if (undoable) {
        undo_manager_.end_transaction();
    }
}

void MultiStoreController::select_object(const std::string& object_id) {
    // Selection is not undoable by design
    scene_store_->dispatch(scene_actions::SelectObject{object_id});
}

std::string MultiStoreController::get_selected_id() const {
    return scene_store_->get().selected_id;
}

void MultiStoreController::begin_transaction(const std::string& description) {
    undo_manager_.begin_transaction(description);
}

void MultiStoreController::end_transaction() {
    undo_manager_.end_transaction();
}

void MultiStoreController::cancel_transaction() {
    undo_manager_.cancel_transaction();
}

std::function<void(const std::any&)>
MultiStoreController::make_object_restore_fn(const std::string& object_id) {
    return [this, object_id](const std::any& state_any) {
        if (!state_any.has_value()) return;

        auto* store = registry_.get(object_id);
        if (!store) return;

        auto state = std::any_cast<ObjectState>(state_any);
        store->dispatch(object_actions::RestoreState{state});
    };
}

// ============================================================
// Demo Functions
// ============================================================

void demo_multi_store_basic() {
    std::cout << "\n========================================\n";
    std::cout << "Demo: Multi-Store Basic Operations\n";
    std::cout << "========================================\n\n";

    MultiStoreController controller;

    // Add some objects
    std::cout << "1. Adding objects...\n";

    ValueMap light_data;
    light_data = light_data.set("name", immer::box<Value>(Value{"Sun Light"}));
    light_data = light_data.set("intensity", immer::box<Value>(Value{1.0}));
    light_data = light_data.set("color", immer::box<Value>(Value{"#FFFFFF"}));
    controller.add_object("light_sun", "Light", Value{light_data});

    ValueMap camera_data;
    camera_data = camera_data.set("name", immer::box<Value>(Value{"Main Camera"}));
    camera_data = camera_data.set("fov", immer::box<Value>(Value{60.0}));
    controller.add_object("camera_main", "Camera", Value{camera_data});

    std::cout << "   Objects added: " << controller.object_count() << "\n";
    std::cout << "   Undo stack: " << controller.undo_count() << "\n\n";

    // Modify a property
    std::cout << "2. Modifying light intensity...\n";
    controller.set_property("light_sun", "intensity", Value{2.5});

    if (auto* obj = controller.get_object("light_sun")) {
        if (auto* map = obj->data.get_if<ValueMap>()) {
            if (auto it = map->find("intensity"); it) {
                std::cout << "   New intensity: " << value_to_string(**it) << "\n";
            }
        }
    }
    std::cout << "   Undo stack: " << controller.undo_count() << "\n\n";

    // Selection (not undoable)
    std::cout << "3. Selecting object (should NOT affect undo)...\n";
    std::size_t undo_before = controller.undo_count();
    controller.select_object("light_sun");
    std::cout << "   Selected: " << controller.get_selected_id() << "\n";
    std::cout << "   Undo stack before: " << undo_before
              << ", after: " << controller.undo_count() << "\n\n";

    // Undo
    std::cout << "4. Undo property change...\n";
    controller.undo();

    if (auto* obj = controller.get_object("light_sun")) {
        if (auto* map = obj->data.get_if<ValueMap>()) {
            if (auto it = map->find("intensity"); it) {
                std::cout << "   Intensity after undo: " << value_to_string(**it) << "\n";
            }
        }
    }
    std::cout << "   Undo stack: " << controller.undo_count()
              << ", Redo stack: " << controller.redo_count() << "\n";
}

void demo_multi_store_transactions() {
    std::cout << "\n========================================\n";
    std::cout << "Demo: Multi-Store Transactions\n";
    std::cout << "========================================\n\n";

    MultiStoreController controller;

    // Create multiple objects
    for (int i = 0; i < 5; ++i) {
        ValueMap data;
        data = data.set("name", immer::box<Value>(Value{"Cube_" + std::to_string(i)}));
        data = data.set("x", immer::box<Value>(Value{static_cast<double>(i * 10)}));
        data = data.set("y", immer::box<Value>(Value{0.0}));
        data = data.set("z", immer::box<Value>(Value{0.0}));
        controller.add_object("cube_" + std::to_string(i), "Mesh", Value{data}, false);
    }

    std::cout << "Created " << controller.object_count() << " cubes\n";
    std::cout << "Initial undo stack: " << controller.undo_count() << "\n\n";

    // Batch edit using transaction
    std::cout << "1. Moving all cubes with a transaction (single undo operation)...\n";

    std::vector<std::tuple<std::string, std::string, Value>> edits;
    for (int i = 0; i < 5; ++i) {
        std::string id = "cube_" + std::to_string(i);
        edits.emplace_back(id, "y", Value{100.0});  // Move all cubes up
    }

    controller.batch_edit(edits);

    std::cout << "   Edited 5 objects\n";
    std::cout << "   Undo stack: " << controller.undo_count() << " (should be 1)\n\n";

    // Verify changes
    std::cout << "2. Verifying changes...\n";
    for (int i = 0; i < 3; ++i) {
        if (auto* obj = controller.get_object("cube_" + std::to_string(i))) {
            if (auto* map = obj->data.get_if<ValueMap>()) {
                if (auto it = map->find("y"); it) {
                    std::cout << "   cube_" << i << " y = " << value_to_string(**it) << "\n";
                }
            }
        }
    }

    // Single undo reverts ALL changes
    std::cout << "\n3. Single undo (should revert ALL cubes)...\n";
    controller.undo();

    for (int i = 0; i < 3; ++i) {
        if (auto* obj = controller.get_object("cube_" + std::to_string(i))) {
            if (auto* map = obj->data.get_if<ValueMap>()) {
                if (auto it = map->find("y"); it) {
                    std::cout << "   cube_" << i << " y = " << value_to_string(**it) << "\n";
                }
            }
        }
    }
    std::cout << "   Undo stack: " << controller.undo_count() << "\n";
}

void demo_multi_store_undo_redo() {
    std::cout << "\n========================================\n";
    std::cout << "Demo: Cross-Store Undo/Redo\n";
    std::cout << "========================================\n\n";

    MultiStoreController controller;

    // Create two objects
    ValueMap light_data;
    light_data = light_data.set("intensity", immer::box<Value>(Value{1.0}));
    controller.add_object("light", "Light", Value{light_data});

    ValueMap mesh_data;
    mesh_data = mesh_data.set("scale", immer::box<Value>(Value{1.0}));
    controller.add_object("mesh", "Mesh", Value{mesh_data});

    std::cout << "Created 2 objects (2 undo operations)\n";
    std::cout << "Undo stack: " << controller.undo_count() << "\n\n";

    // Interleaved edits
    std::cout << "1. Interleaved edits on different objects...\n";
    controller.set_property("light", "intensity", Value{2.0});
    std::cout << "   Edit 1: light.intensity = 2.0\n";

    controller.set_property("mesh", "scale", Value{2.0});
    std::cout << "   Edit 2: mesh.scale = 2.0\n";

    controller.set_property("light", "intensity", Value{3.0});
    std::cout << "   Edit 3: light.intensity = 3.0\n";

    std::cout << "   Undo stack: " << controller.undo_count() << "\n\n";

    auto print_state = [&]() {
        std::cout << "   Current state:\n";
        if (auto* light = controller.get_object("light")) {
            if (auto* map = light->data.get_if<ValueMap>()) {
                if (auto it = map->find("intensity"); it) {
                    std::cout << "     light.intensity = " << value_to_string(**it) << "\n";
                }
            }
        }
        if (auto* mesh = controller.get_object("mesh")) {
            if (auto* map = mesh->data.get_if<ValueMap>()) {
                if (auto it = map->find("scale"); it) {
                    std::cout << "     mesh.scale = " << value_to_string(**it) << "\n";
                }
            }
        }
    };

    print_state();

    // Undo sequence
    std::cout << "\n2. Undo sequence...\n";

    std::cout << "\n   After undo 1:\n";
    controller.undo();
    print_state();

    std::cout << "\n   After undo 2:\n";
    controller.undo();
    print_state();

    std::cout << "\n   After undo 3:\n";
    controller.undo();
    print_state();

    // Redo
    std::cout << "\n3. Redo sequence...\n";
    std::cout << "   Redo stack: " << controller.redo_count() << "\n";

    std::cout << "\n   After redo 1:\n";
    controller.redo();
    print_state();

    std::cout << "\n   After redo 2:\n";
    controller.redo();
    print_state();
}

void demo_multi_store_performance() {
    std::cout << "\n========================================\n";
    std::cout << "Demo: Multi-Store Performance\n";
    std::cout << "========================================\n\n";

    const int NUM_OBJECTS = 1000;
    const int NUM_EDITS = 100;

    MultiStoreController controller;

    // Create many objects
    std::cout << "1. Creating " << NUM_OBJECTS << " objects...\n";
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_OBJECTS; ++i) {
        ValueMap data;
        data = data.set("name", immer::box<Value>(Value{"Object_" + std::to_string(i)}));
        data = data.set("x", immer::box<Value>(Value{static_cast<double>(i)}));
        data = data.set("y", immer::box<Value>(Value{0.0}));
        data = data.set("z", immer::box<Value>(Value{0.0}));
        data = data.set("visible", immer::box<Value>(Value{true}));
        // Create without recording undo (simulating initial load)
        controller.add_object("obj_" + std::to_string(i), "Mesh", Value{data}, false);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto create_time = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "   Time: " << create_time.count() << "ms\n";
    std::cout << "   Objects: " << controller.object_count() << "\n\n";

    // Random edits with undo recording
    std::cout << "2. Performing " << NUM_EDITS << " random edits with undo...\n";
    start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_EDITS; ++i) {
        int obj_idx = i % NUM_OBJECTS;
        std::string obj_id = "obj_" + std::to_string(obj_idx);
        controller.set_property(obj_id, "x", Value{static_cast<double>(i * 10)});
    }

    end = std::chrono::high_resolution_clock::now();
    auto edit_time = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "   Time: " << edit_time.count() << "ms\n";
    std::cout << "   Undo stack: " << controller.undo_count() << "\n\n";

    // Undo all edits
    std::cout << "3. Undoing all " << NUM_EDITS << " edits...\n";
    start = std::chrono::high_resolution_clock::now();

    while (controller.can_undo()) {
        controller.undo();
    }

    end = std::chrono::high_resolution_clock::now();
    auto undo_time = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "   Time: " << undo_time.count() << "ms\n";
    std::cout << "   Undo stack: " << controller.undo_count() << "\n";
    std::cout << "   Redo stack: " << controller.redo_count() << "\n\n";

    // Redo all edits
    std::cout << "4. Redoing all " << NUM_EDITS << " edits...\n";
    start = std::chrono::high_resolution_clock::now();

    while (controller.can_redo()) {
        controller.redo();
    }

    end = std::chrono::high_resolution_clock::now();
    auto redo_time = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "   Time: " << redo_time.count() << "ms\n\n";

    std::cout << "Summary:\n";
    std::cout << "   Create " << NUM_OBJECTS << " objects: " << create_time.count() << "ms\n";
    std::cout << "   " << NUM_EDITS << " edits with undo: " << edit_time.count() << "ms\n";
    std::cout << "   Undo all: " << undo_time.count() << "ms\n";
    std::cout << "   Redo all: " << redo_time.count() << "ms\n";
}

} // namespace multi_store
} // namespace lager_ext
