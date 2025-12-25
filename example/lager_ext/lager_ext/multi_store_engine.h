// multi_store_engine.h
// Multi-Store Architecture with External UndoManager
//
// This architecture separates each object into its own lager store,
// while maintaining a unified undo/redo history across all stores.
//
// Key components:
// - ObjectStore: Individual lager store for each scene object
// - StoreRegistry: Manages collection of ObjectStores
// - UndoManager: External undo/redo system that works across stores
// - MultiStoreController: Coordinator that ties everything together

#pragma once

#include <lager_ext/value.h>

#include <lager/store.hpp>
#include <lager/event_loop/manual.hpp>

#include <any>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <iostream>

namespace lager_ext {
namespace multi_store {

// ============================================================
// Object-Level State and Actions
// ============================================================

// State for a single object's store
struct ObjectState {
    std::string id;
    std::string type;
    Value data;           // Object properties as Value map
    std::size_t version = 0;

    bool operator==(const ObjectState& other) const {
        return id == other.id &&
               type == other.type &&
               version == other.version;
    }
};

// Actions for individual object stores
namespace object_actions {

struct SetProperty {
    std::string property_name;
    Value new_value;
};

struct SetProperties {
    std::vector<std::pair<std::string, Value>> properties;
};

struct ReplaceData {
    Value new_data;
};

// Internal action for undo/redo - restores entire state
struct RestoreState {
    ObjectState state;
};

} // namespace object_actions

using ObjectAction = std::variant<
    object_actions::SetProperty,
    object_actions::SetProperties,
    object_actions::ReplaceData,
    object_actions::RestoreState
>;

// Reducer for individual object stores
ObjectState object_update(ObjectState state, ObjectAction action);

// ============================================================
// Scene-Level State (Lightweight - No Object Data)
// ============================================================

struct SceneMetaState {
    std::string selected_id;
    std::set<std::string> object_ids;      // Just IDs, not full objects
    std::size_t version = 0;
};

namespace scene_actions {

struct SelectObject {
    std::string object_id;
};

struct RegisterObject {
    std::string object_id;
};

struct UnregisterObject {
    std::string object_id;
};

} // namespace scene_actions

using SceneAction = std::variant<
    scene_actions::SelectObject,
    scene_actions::RegisterObject,
    scene_actions::UnregisterObject
>;

SceneMetaState scene_update(SceneMetaState state, SceneAction action);

// ============================================================
// UndoManager - External Undo/Redo System
// ============================================================

// Represents a single undoable operation.
//
// Design Note on std::any:
//   Using std::any for type erasure here has the following trade-offs:
//   - Pros: Flexibility to store any state type without template complexity
//   - Cons: Dynamic allocation overhead, RTTI usage
//
//   For performance-critical scenarios with known state types, consider:
//   - Using std::variant<ObjectState, SceneMetaState> instead
//   - Using a type-erased wrapper with small buffer optimization (SBO)
//
//   Current design prioritizes flexibility over absolute performance,
//   which is acceptable for typical undo/redo use cases (user-driven, not hot path).
struct UndoCommand {
    std::string store_id;           // Which store this affects ("__scene__" for scene store)
    std::string description;        // Human-readable description
    std::any old_state;             // State before the operation
    std::any new_state;             // State after the operation

    // Function to restore state - called during undo/redo
    std::function<void(const std::any&)> restore_fn;
};

// Composite command - groups multiple operations into one undoable unit
struct CompositeCommand {
    std::vector<UndoCommand> sub_commands;
    std::string description;

    bool empty() const { return sub_commands.empty(); }
};

class UndoManager {
public:
    // Transaction API - group multiple operations
    void begin_transaction(const std::string& description = "");
    void record(UndoCommand cmd);
    void end_transaction();
    void cancel_transaction();

    // Check if currently in a transaction
    bool in_transaction() const { return transaction_active_; }

    // Undo/Redo operations
    bool undo();
    bool redo();

    // State queries
    bool can_undo() const { return !undo_stack_.empty(); }
    bool can_redo() const { return !redo_stack_.empty(); }
    std::size_t undo_count() const { return undo_stack_.size(); }
    std::size_t redo_count() const { return redo_stack_.size(); }

    // Get descriptions for UI
    std::optional<std::string> next_undo_description() const;
    std::optional<std::string> next_redo_description() const;

    // Clear all history
    void clear();

    // Set maximum history size (0 = unlimited)
    void set_max_history(std::size_t max) { max_history_ = max; }

private:
    std::vector<CompositeCommand> undo_stack_;
    std::vector<CompositeCommand> redo_stack_;

    bool transaction_active_ = false;
    CompositeCommand current_transaction_;

    std::size_t max_history_ = 100;  // Default max history

    void trim_history();
};

// ============================================================
// Store Type Definitions
// ============================================================

// Use decltype to get the actual store type from make_store
inline auto make_object_store_impl(ObjectState initial_state) {
    return lager::make_store<ObjectAction>(
        std::move(initial_state),
        lager::with_manual_event_loop{},
        lager::with_reducer(object_update)
    );
}

inline auto make_scene_store_impl(SceneMetaState initial_state) {
    return lager::make_store<SceneAction>(
        std::move(initial_state),
        lager::with_manual_event_loop{},
        lager::with_reducer(scene_update)
    );
}

using ObjectStoreType = decltype(make_object_store_impl(std::declval<ObjectState>()));
using SceneStoreType = decltype(make_scene_store_impl(std::declval<SceneMetaState>()));

// ============================================================
// StoreRegistry - Manages Multiple Object Stores
// ============================================================

class StoreRegistry {
public:
    // Get or create a store for an object
    ObjectStoreType* get(const std::string& object_id);
    ObjectStoreType* create(const std::string& object_id, ObjectState initial_state);

    // Remove a store
    bool remove(const std::string& object_id);

    // Check existence
    bool exists(const std::string& object_id) const;

    // Get all store IDs
    std::vector<std::string> all_ids() const;

    // Get count
    std::size_t size() const { return stores_.size(); }

    // Iterate over all stores
    template<typename Fn>
    void for_each(Fn&& fn) {
        for (auto& [id, store] : stores_) {
            fn(id, *store);
        }
    }

    // Clear all stores
    void clear() { stores_.clear(); }

private:
    std::unordered_map<std::string, std::unique_ptr<ObjectStoreType>> stores_;
};

// ============================================================
// MultiStoreController - Main Coordinator
// ============================================================

class MultiStoreController {
public:
    MultiStoreController();
    ~MultiStoreController() = default;

    // ===== Object Management =====

    // Add a new object to the scene
    void add_object(const std::string& id,
                   const std::string& type,
                   Value initial_data,
                   bool undoable = true);

    // Remove an object from the scene
    void remove_object(const std::string& id, bool undoable = true);

    // Get object state (returns nullptr if not found)
    const ObjectState* get_object(const std::string& id) const;

    // Get all object IDs
    std::vector<std::string> get_all_object_ids() const;

    // ===== Property Editing =====

    // Set a single property on an object
    void set_property(const std::string& object_id,
                     const std::string& property_name,
                     Value new_value,
                     bool undoable = true);

    // Set multiple properties at once (single undo operation)
    void set_properties(const std::string& object_id,
                       const std::vector<std::pair<std::string, Value>>& properties,
                       bool undoable = true);

    // Batch edit across multiple objects (single undo operation)
    void batch_edit(const std::vector<std::tuple<std::string, std::string, Value>>& edits,
                   bool undoable = true);

    // ===== Selection =====

    void select_object(const std::string& object_id);
    std::string get_selected_id() const;

    // ===== Undo/Redo =====

    bool undo() { return undo_manager_.undo(); }
    bool redo() { return undo_manager_.redo(); }
    bool can_undo() const { return undo_manager_.can_undo(); }
    bool can_redo() const { return undo_manager_.can_redo(); }
    std::size_t undo_count() const { return undo_manager_.undo_count(); }
    std::size_t redo_count() const { return undo_manager_.redo_count(); }

    // Transaction API for complex operations
    void begin_transaction(const std::string& description = "");
    void end_transaction();
    void cancel_transaction();

    // ===== Statistics =====

    std::size_t object_count() const { return registry_.size(); }
    const SceneMetaState& get_scene_state() const;

    // Access to undo manager for advanced usage
    UndoManager& undo_manager() { return undo_manager_; }
    const UndoManager& undo_manager() const { return undo_manager_; }

private:
    StoreRegistry registry_;
    std::unique_ptr<SceneStoreType> scene_store_;
    UndoManager undo_manager_;

    // Helper to create restore function for object state
    std::function<void(const std::any&)> make_object_restore_fn(const std::string& object_id);
};

// ============================================================
// Demo Functions
// ============================================================

// Demonstrates basic multi-store operations
void demo_multi_store_basic();

// Demonstrates transaction (composite) operations
void demo_multi_store_transactions();

// Demonstrates undo/redo across multiple stores
void demo_multi_store_undo_redo();

// Performance comparison: single store vs multi-store
void demo_multi_store_performance();

} // namespace multi_store
} // namespace lager_ext
