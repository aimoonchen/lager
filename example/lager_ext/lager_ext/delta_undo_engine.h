// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file delta_undo_engine.h
/// @brief Delta-based (Incremental) Undo/Redo Engine.
///
/// This module implements a delta-based undo/redo mechanism that solves the problem
/// of "system state persistence across undo/redo operations":
///
/// Problem Scenario:
///   T1: User Action A -> state1 (recorded)
///   T2: System Action S (e.g., lazy load) -> state2 (NOT recorded)
///   T3: User Action B -> state3 (recorded)
///
///   When user performs Undo (to undo B):
///   - With snapshot-based undo: restores state1, LOSING system changes from S
///   - With delta-based undo: applies inverse of B to current state, PRESERVING S
///
/// Key Concepts:
/// 1. Delta (Reversible Operation): Stores both the forward and inverse transformations
/// 2. System actions modify state but don't create deltas
/// 3. Undo/Redo applies transformations to the CURRENT state, not restoring snapshots
///
/// This allows system operations to be treated as "initialization" that persists
/// across all undo/redo operations.

#pragma once

#include <lager_ext/scene_types.h>  // Shared types: SceneObject, SceneState, UIMeta, etc.
#include <lager_ext/value.h>
#include <lager_ext/shared_state.h>
#include <lager_ext/lager_lens.h>

#include <lager/store.hpp>
#include <lager/event_loop/manual.hpp>

#include <immer/flex_vector.hpp>
#include <immer/map.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lager_ext {
namespace delta_undo {

// ============================================================
// Type Aliases from lager_ext namespace
// ============================================================

// Re-use shared types from scene_types.h (in lager_ext namespace)
// Aliased here for convenience within delta_undo namespace
using lager_ext::WidgetType;
using lager_ext::NumericRange;
using lager_ext::ComboOptions;
using lager_ext::PropertyMeta;
using lager_ext::UIMeta;
using lager_ext::SceneObject;
using lager_ext::SceneState;

// Forward declarations for types defined in this header
struct DeltaModel;

// ============================================================
// Delta (Reversible Operation) - Core concept
// ============================================================

/// A Delta represents a reversible state transformation.
/// It stores both:
/// - apply_fn: How to apply the change (for redo)
/// - unapply_fn: How to reverse the change (for undo)
///
/// Both functions operate on the CURRENT state, not restoring snapshots.
/// This is the key difference from snapshot-based undo.
struct Delta {
    std::string description;  // Human-readable description of the operation

    /// Apply the change to produce the new state (forward)
    std::function<SceneState(const SceneState&)> apply_fn;

    /// Reverse the change to restore the previous state (backward)
    std::function<SceneState(const SceneState&)> unapply_fn;

    /// Constructor for creating a delta
    Delta(std::string desc,
          std::function<SceneState(const SceneState&)> apply,
          std::function<SceneState(const SceneState&)> unapply)
        : description(std::move(desc))
        , apply_fn(std::move(apply))
        , unapply_fn(std::move(unapply))
    {}

    /// Default constructor for variant compatibility
    Delta() : description("empty"), apply_fn([](const SceneState& s) { return s; }),
              unapply_fn([](const SceneState& s) { return s; }) {}
};

// ============================================================
// Action Types - Separated by undo behavior
// ============================================================

namespace actions {

// ===== Control Actions =====

/// Undo the last user operation (applies unapply_fn to current state)
struct Undo {};

/// Redo the last undone operation (applies apply_fn to current state)
struct Redo {};

/// Clear all undo/redo history
struct ClearHistory {};

// ===== User Actions (create deltas, can be undone) =====

/// Set a single property - creates a delta with old/new value
struct SetProperty {
    std::string object_id;
    std::string property_path;
    Value new_value;
};

/// Set multiple properties atomically - creates a single delta
struct SetProperties {
    std::string object_id;
    std::map<std::string, Value> updates;  // path -> new_value
};

/// Add a new object - delta removes it on undo
struct AddObject {
    SceneObject object;
    std::string parent_id;
};

/// Remove an object - delta adds it back on undo
struct RemoveObject {
    std::string object_id;
};

/// Composite operation - groups multiple operations into one undo step
struct BeginTransaction {
    std::string description;
};

struct EndTransaction {};

// ===== System Actions (modify state but DON'T create deltas) =====

/// Select an object - no delta needed
struct SelectObject {
    std::string object_id;
};

/// Sync from external source - no delta, state changes persist through undo
struct SyncFromEngine {
    SceneState new_state;
};

/// Load objects incrementally - no delta, loaded data persists through undo
struct LoadObjects {
    std::vector<SceneObject> objects;
};

/// Update system/UI state - no delta
struct SetSystemState {
    bool is_loading = false;
    float progress = 0.0f;
    std::string status_message;
};

} // namespace actions

// Action variant
using DeltaAction = std::variant<
    // Control
    actions::Undo,
    actions::Redo,
    actions::ClearHistory,
    // User actions (create deltas)
    actions::SetProperty,
    actions::SetProperties,
    actions::AddObject,
    actions::RemoveObject,
    actions::BeginTransaction,
    actions::EndTransaction,
    // System actions (no deltas)
    actions::SelectObject,
    actions::SyncFromEngine,
    actions::LoadObjects,
    actions::SetSystemState
>;

// ============================================================
// Delta Model - State with delta-based history
// ============================================================

/// System state that persists across undo/redo
struct SystemState {
    bool is_loading = false;
    float progress = 0.0f;
    std::string status_message;
};

/// Main model for delta-based undo/redo
struct DeltaModel {
    SceneState scene;
    SystemState system;

    // Delta stacks for undo/redo
    immer::flex_vector<Delta> undo_stack;
    immer::flex_vector<Delta> redo_stack;

    // Transaction support - accumulates deltas into a single compound delta
    std::optional<std::string> transaction_description;
    std::vector<Delta> transaction_deltas;  // Temporary storage during transaction

    // Configuration
    static constexpr std::size_t max_history = 100;

    // Dirty flag
    bool dirty = false;
};

// ============================================================
// Delta Factory - Creates deltas for different operations
// ============================================================

/// Factory for creating reversible deltas from operations
class DeltaFactory {
public:
    /// Create delta for setting a single property
    static Delta create_set_property_delta(
        const std::string& object_id,
        const std::string& property_path,
        const Value& old_value,
        const Value& new_value);

    /// Create delta for setting multiple properties
    static Delta create_set_properties_delta(
        const std::string& object_id,
        const std::map<std::string, Value>& old_values,
        const std::map<std::string, Value>& new_values);

    /// Create delta for adding an object
    static Delta create_add_object_delta(
        const SceneObject& object,
        const std::string& parent_id);

    /// Create delta for removing an object
    static Delta create_remove_object_delta(
        const SceneObject& object,
        const std::string& parent_id);

    /// Compose multiple deltas into a single compound delta
    static Delta compose_deltas(
        const std::string& description,
        const std::vector<Delta>& deltas);
};

// ============================================================
// Reducer - Pure function for state updates
// ============================================================

/// Main reducer for delta-based undo engine
/// - User actions create deltas and modify state
/// - System actions only modify state (no deltas)
/// - Undo applies unapply_fn to current state
/// - Redo applies apply_fn to current state
DeltaModel delta_update(DeltaModel model, DeltaAction action);

// ============================================================
// Delta Controller - High-level interface
// ============================================================

/// Controller for delta-based undo/redo engine
class DeltaController {
public:
    DeltaController();
    ~DeltaController();

    // Initialize with scene state
    void initialize(const SceneState& initial_state);

    // Dispatch actions
    void dispatch(DeltaAction action);

    // Get current state
    [[nodiscard]] const DeltaModel& get_model() const;
    [[nodiscard]] const SceneState& get_scene() const;
    [[nodiscard]] const SceneObject* get_object(const std::string& id) const;
    [[nodiscard]] const SceneObject* get_selected_object() const;

    // Property access
    [[nodiscard]] Value get_property(const std::string& object_id, const std::string& path) const;
    void set_property(const std::string& object_id, const std::string& path, Value value);

    // Batch property updates (creates single undo entry)
    void set_properties(const std::string& object_id, const std::map<std::string, Value>& updates);

    // Transaction support - group multiple operations into single undo
    void begin_transaction(const std::string& description);
    void end_transaction();

    // Undo/Redo
    [[nodiscard]] bool can_undo() const;
    [[nodiscard]] bool can_redo() const;
    [[nodiscard]] std::string get_undo_description() const;
    [[nodiscard]] std::string get_redo_description() const;
    void undo();
    void redo();

    // History info
    [[nodiscard]] std::size_t undo_count() const;
    [[nodiscard]] std::size_t redo_count() const;
    void clear_history();

    // Process pending events
    void step();

    // Watch for changes
    using WatchCallback = std::function<void(const DeltaModel&)>;
    [[nodiscard]] std::function<void()> watch(WatchCallback callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================
// Demo Functions
// ============================================================

/// Demo: Basic delta-based undo/redo
void demo_delta_undo_basic();

/// Demo: System operations persisting through undo
/// Shows that system changes (like lazy loading) are preserved when undoing user actions
void demo_system_persistence();

/// Demo: Transaction support (grouping operations)
void demo_transactions();

/// Demo: Complex scenario with interleaved user and system operations
void demo_interleaved_operations();

} // namespace delta_undo
} // namespace lager_ext
