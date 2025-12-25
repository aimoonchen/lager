// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file editor_engine.h
/// @brief Editor-Engine Cross-Process State Management.
///
/// This module implements a technical preview for a game engine editor architecture:
/// - Process A (Editor): Uses lager store for state management with redo/undo
/// - Process B (Engine): Maintains runtime scene objects, receives state updates
///
/// Key features:
/// 1. Scene objects are serialized to Value with UI metadata for Qt binding
/// 2. Editor uses lager cursors/lenses for property editing
/// 3. State changes are published as diffs to the engine process
/// 4. Supports redo/undo via lager's built-in mechanisms

#pragma once

#include <lager_ext/scene_types.h>  // Shared types: SceneObject, SceneState, UIMeta, etc.
#include <lager_ext/value.h>
#include <lager_ext/shared_state.h>
#include <lager_ext/lager_lens.h>

#include <lager/store.hpp>
#include <lager/event_loop/manual.hpp>
#include <lager/extra/struct.hpp>
#include <lager/cursor.hpp>
#include <lager/lenses.hpp>
#include <lager/lenses/at.hpp>

#include <immer/flex_vector.hpp>
#include <immer/map.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lager_ext {

// Note: WidgetType, NumericRange, ComboOptions, PropertyMeta, UIMeta,
// SceneObject, and SceneState are now defined in scene_types.h

// ============================================================
// Action Category - Distinguish user operations from system operations
// ============================================================

/// Action category for undo/redo filtering
/// - User: User-initiated actions that should be recorded in undo history
/// - System: System/internal actions that should NOT be recorded in undo history
enum class ActionCategory {
    User,    // User operations - recorded to undo history
    System   // System operations - NOT recorded (e.g., loading, selection)
};

/// Tagged action wrapper - carries category metadata
/// This allows the reducer to determine whether to record the action in undo history
template<ActionCategory Category, typename Payload>
struct TaggedAction {
    Payload payload;
    static constexpr ActionCategory category = Category;

    // Convenience constructor
    explicit TaggedAction(Payload p) : payload(std::move(p)) {}

    // Default constructor for variant compatibility
    TaggedAction() : payload{} {}
};

/// Convenience aliases for action tagging
template<typename T>
using UserAction = TaggedAction<ActionCategory::User, T>;

template<typename T>
using SystemAction = TaggedAction<ActionCategory::System, T>;

// ============================================================
// Action Payloads - The actual data for each action type
// ============================================================

namespace payloads {

// Select an object for editing (system action - no undo needed)
struct SelectObject {
    std::string object_id;
};

// Modify a property of the selected object (user action - needs undo)
struct SetProperty {
    std::string property_path;  // e.g., "position.x" or just "name"
    Value new_value;
};

// Batch property update (user action - needs undo)
struct SetProperties {
    std::map<std::string, Value> updates;  // path -> value
};

// Sync from engine - replaces entire state (system action - clears history)
struct SyncFromEngine {
    SceneState new_state;
};

// Load objects in batch (system action - for incremental loading, no undo)
struct LoadObjects {
    std::vector<SceneObject> objects;
};

// Add a new object (user action - needs undo)
struct AddObject {
    SceneObject object;
    std::string parent_id;
};

// Remove an object (user action - needs undo)
struct RemoveObject {
    std::string object_id;
};

// Set loading state (system action - UI state, no undo)
struct SetLoadingState {
    bool is_loading;
    float progress;
};

} // namespace payloads

// ============================================================
// Editor Actions - Tagged with User/System category
// ============================================================

namespace actions {

// ===== Undo/Redo control actions =====
struct Undo {};
struct Redo {};
struct ClearHistory {};  // Clear undo/redo history (e.g., after scene load)

// ===== User Actions (recorded to undo history) =====
using SetProperty = UserAction<payloads::SetProperty>;
using SetProperties = UserAction<payloads::SetProperties>;
using AddObject = UserAction<payloads::AddObject>;
using RemoveObject = UserAction<payloads::RemoveObject>;

// ===== System Actions (NOT recorded to undo history) =====
using SelectObject = SystemAction<payloads::SelectObject>;
using SyncFromEngine = SystemAction<payloads::SyncFromEngine>;
using LoadObjects = SystemAction<payloads::LoadObjects>;
using SetLoadingState = SystemAction<payloads::SetLoadingState>;

} // namespace actions

// Action variant for lager - includes all tagged actions
using EditorAction = std::variant<
    // Control actions
    actions::Undo,
    actions::Redo,
    actions::ClearHistory,
    // User actions (recorded to undo)
    actions::SetProperty,
    actions::SetProperties,
    actions::AddObject,
    actions::RemoveObject,
    // System actions (NOT recorded to undo)
    actions::SelectObject,
    actions::SyncFromEngine,
    actions::LoadObjects,
    actions::SetLoadingState
>;

// ============================================================
// Helper: Check if an action should be recorded to undo history
// ============================================================

/// Determines whether an action should be recorded in the undo history
/// Returns true for UserAction, false for SystemAction and control actions
template<typename ActionVariant>
bool should_record_undo(const ActionVariant& action) {
    return std::visit([](const auto& act) -> bool {
        using T = std::decay_t<decltype(act)>;

        // Undo/Redo/ClearHistory themselves are never recorded
        if constexpr (std::is_same_v<T, actions::Undo> ||
                      std::is_same_v<T, actions::Redo> ||
                      std::is_same_v<T, actions::ClearHistory>) {
            return false;
        }
        // Check TaggedAction category
        else if constexpr (requires { T::category; }) {
            return T::category == ActionCategory::User;
        }
        // Untagged actions default to recording (safety)
        else {
            return true;
        }
    }, action);
}

// ============================================================
// Editor State Model (for lager store)
// ============================================================

struct EditorModel {
    SceneState scene;

    // History for undo/redo - using flex_vector for O(1) operations at both ends
    immer::flex_vector<SceneState> undo_stack;
    immer::flex_vector<SceneState> redo_stack;
    static constexpr std::size_t max_history = 100;

    // Dirty flag for change notification
    bool dirty = false;
};

// Reducer function for lager store
EditorModel editor_update(EditorModel model, EditorAction action);

// ============================================================
// Engine Simulator (Process B)
// ============================================================

class EngineSimulator {
public:
    EngineSimulator();
    ~EngineSimulator();

    // Initialize with sample scene data
    void initialize_sample_scene();

    // Get initial scene state (called by editor at startup)
    SceneState get_initial_state() const;

    // Apply changes from editor (diff or full state)
    void apply_diff(const DiffResult& diff);
    void apply_full_state(const Value& state);

    // Get current engine state as Value
    Value get_state_as_value() const;

    // Register callback for engine events
    using EngineCallback = std::function<void(const std::string& event, const Value& data)>;
    void on_event(EngineCallback callback);

    // Print current state (for debugging)
    void print_state() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================
// Editor Controller (Process A)
// ============================================================

// Effect handler for state changes
struct EditorEffects {
    std::function<void(const DiffResult& diff)> on_state_changed;
    std::function<void(const std::string& object_id)> on_selection_changed;
};

class EditorController {
public:
    EditorController();
    ~EditorController();

    // Initialize with engine state
    void initialize(const SceneState& initial_state);

    // Dispatch actions
    void dispatch(EditorAction action);

    // Get current state
    [[nodiscard]] const EditorModel& get_model() const;

    // Get currently selected object
    [[nodiscard]] const SceneObject* get_selected_object() const;

    // Create a cursor for a property of the selected object
    // Returns null Value if no object is selected or property doesn't exist
    [[nodiscard]] Value get_property(const std::string& path) const;

    // Set property value (shorthand for dispatch(SetProperty))
    void set_property(const std::string& path, Value value);

    // Undo/Redo
    [[nodiscard]] bool can_undo() const;
    [[nodiscard]] bool can_redo() const;
    void undo();
    void redo();

    // Set effect handlers
    void set_effects(EditorEffects effects);

    // Process pending events (for manual event loop)
    void step();

    // Watch for changes (returns unsubscribe function)
    using WatchCallback = std::function<void(const EditorModel&)>;
    [[nodiscard]] std::function<void()> watch(WatchCallback callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================
// Qt UI Binding Helpers (Mock implementation for demo)
// ============================================================

// Represents a Qt widget binding for a property
struct PropertyBinding {
    std::string property_path;
    PropertyMeta meta;
    std::function<Value()> getter;
    std::function<void(Value)> setter;
};

// Generate property bindings for the currently selected object
std::vector<PropertyBinding> generate_property_bindings(
    EditorController& controller,
    const SceneObject& object);

// ============================================================
// Demo Functions
// ============================================================

// Run the complete editor-engine demo
void demo_editor_engine();

// Demo: Property editing with Qt binding simulation
void demo_property_editing();

// Demo: Undo/Redo functionality
void demo_undo_redo();

// Demo: User vs System actions and undo filtering
void demo_action_categories();

} // namespace lager_ext
