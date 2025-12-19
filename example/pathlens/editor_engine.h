// editor_engine.h
// Editor-Engine Cross-Process State Management
//
// This module implements a technical preview for a game engine editor architecture:
// - Process A (Editor): Uses lager store for state management with redo/undo
// - Process B (Engine): Maintains runtime scene objects, receives state updates
//
// Key features:
// 1. Scene objects are serialized to Value with UI metadata for Qt binding
// 2. Editor uses lager cursors/lenses for property editing
// 3. State changes are published as diffs to the engine process
// 4. Supports redo/undo via lager's built-in mechanisms

#pragma once

#include "value.h"
#include "shared_state.h"
#include "lager_lens.h"

#include <lager/store.hpp>
#include <lager/event_loop/manual.hpp>
#include <lager/extra/struct.hpp>
#include <lager/cursor.hpp>
#include <lager/lenses.hpp>
#include <lager/lenses/at.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <map>

namespace immer_lens {

// ============================================================
// UI Metadata - Information for generating Qt UI widgets
// ============================================================

// Widget type hints for Qt UI generation
enum class WidgetType {
    LineEdit,       // QString, single line text
    SpinBox,        // int
    DoubleSpinBox,  // float/double
    CheckBox,       // bool
    ColorPicker,    // color (stored as int or string)
    Slider,         // int/float with range
    ComboBox,       // enum or string selection
    Vector3Edit,    // 3D vector (x, y, z)
    FileSelector,   // file path
    ReadOnly,       // display only, not editable
};

// Range constraint for numeric values
struct NumericRange {
    double min_value = 0.0;
    double max_value = 100.0;
    double step = 1.0;
};

// Combo box options
struct ComboOptions {
    std::vector<std::string> options;
    int default_index = 0;
};

// Property UI metadata
struct PropertyMeta {
    std::string name;           // Property name (key in Value map)
    std::string display_name;   // Human-readable name for UI
    std::string tooltip;        // Tooltip text
    std::string category;       // Category for grouping in property editor
    WidgetType widget_type = WidgetType::LineEdit;
    
    // Optional constraints
    std::optional<NumericRange> range;
    std::optional<ComboOptions> combo_options;
    
    bool read_only = false;
    bool visible = true;
    int sort_order = 0;         // For ordering in UI
};

// Object UI metadata (collection of property metadata)
struct ObjectMeta {
    std::string type_name;      // Object type (e.g., "Transform", "Light")
    std::string icon_name;      // Icon for tree view
    std::vector<PropertyMeta> properties;
    
    // Find property meta by name
    const PropertyMeta* find_property(const std::string& name) const {
        for (const auto& prop : properties) {
            if (prop.name == name) return &prop;
        }
        return nullptr;
    }
};

// ============================================================
// Scene Object Structure
// ============================================================

// Scene object with value data and metadata
struct SceneObject {
    std::string id;             // Unique object ID
    std::string type;           // Object type name
    Value data;                 // Object properties as Value
    ObjectMeta meta;            // UI metadata for Qt binding
    
    std::vector<std::string> children;  // Child object IDs
};

// Complete scene state
struct SceneState {
    std::map<std::string, SceneObject> objects;  // All objects by ID
    std::string root_id;                          // Root object ID
    std::string selected_id;                      // Currently selected object
    uint64_t version = 0;                         // State version
};

// ============================================================
// Editor Actions (for lager store)
// ============================================================

namespace actions {

// Select an object for editing
struct SelectObject {
    std::string object_id;
};

// Modify a property of the selected object
struct SetProperty {
    std::string property_path;  // e.g., "position.x" or just "name"
    Value new_value;
};

// Batch property update
struct SetProperties {
    std::map<std::string, Value> updates;  // path -> value
};

// Undo/Redo markers
struct Undo {};
struct Redo {};

// Sync from engine (receive full state)
struct SyncFromEngine {
    SceneState new_state;
};

// Add a new object
struct AddObject {
    SceneObject object;
    std::string parent_id;
};

// Remove an object
struct RemoveObject {
    std::string object_id;
};

} // namespace actions

// Action variant for lager
using EditorAction = std::variant<
    actions::SelectObject,
    actions::SetProperty,
    actions::SetProperties,
    actions::Undo,
    actions::Redo,
    actions::SyncFromEngine,
    actions::AddObject,
    actions::RemoveObject
>;

// ============================================================
// Editor State Model (for lager store)
// ============================================================

struct EditorModel {
    SceneState scene;
    
    // History for undo/redo
    std::vector<SceneState> undo_stack;
    std::vector<SceneState> redo_stack;
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
    const EditorModel& get_model() const;
    
    // Get currently selected object
    const SceneObject* get_selected_object() const;
    
    // Create a cursor for a property of the selected object
    // Returns nullopt if no object is selected or property doesn't exist
    std::optional<Value> get_property(const std::string& path) const;
    
    // Set property value (shorthand for dispatch(SetProperty))
    void set_property(const std::string& path, Value value);
    
    // Undo/Redo
    bool can_undo() const;
    bool can_redo() const;
    void undo();
    void redo();
    
    // Set effect handlers
    void set_effects(EditorEffects effects);
    
    // Process pending events (for manual event loop)
    void step();
    
    // Watch for changes (returns unsubscribe function)
    using WatchCallback = std::function<void(const EditorModel&)>;
    std::function<void()> watch(WatchCallback callback);

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

} // namespace immer_lens
