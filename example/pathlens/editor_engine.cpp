// editor_engine.cpp
// Implementation of Editor-Engine Cross-Process State Management

#include "editor_engine.h"
#include <iostream>
#include <sstream>

namespace immer_lens {

// ============================================================
// Helper: Parse property path (e.g., "position.x" -> ["position", "x"])
// ============================================================
static Path parse_property_path(const std::string& path_str) {
    Path result;
    std::istringstream ss(path_str);
    std::string segment;
    while (std::getline(ss, segment, '.')) {
        if (!segment.empty()) {
            result.push_back(segment);
        }
    }
    return result;
}

// ============================================================
// Helper: Get value at path from a Value
// ============================================================
static Value get_value_at_path(const Value& root, const Path& path) {
    Value current = root;
    for (const auto& elem : path) {
        if (auto* key = std::get_if<std::string>(&elem)) {
            current = current.at(*key);
        } else if (auto* idx = std::get_if<std::size_t>(&elem)) {
            current = current.at(*idx);
        }
    }
    return current;
}

// ============================================================
// Helper: Set value at path in a Value (immutable)
// ============================================================
static Value set_value_at_path(const Value& root, const Path& path, const Value& value) {
    if (path.empty()) {
        return value;
    }
    
    const auto& first = path[0];
    Path rest(path.begin() + 1, path.end());
    
    if (auto* key = std::get_if<std::string>(&first)) {
        Value child = root.at(*key);
        Value new_child = set_value_at_path(child, rest, value);
        return root.set(*key, new_child);
    } else if (auto* idx = std::get_if<std::size_t>(&first)) {
        Value child = root.at(*idx);
        Value new_child = set_value_at_path(child, rest, value);
        return root.set(*idx, new_child);
    }
    
    return root;
}

// ============================================================
// Editor Reducer Implementation
// ============================================================

EditorModel editor_update(EditorModel model, EditorAction action) {
    return std::visit([&model](auto&& act) -> EditorModel {
        using T = std::decay_t<decltype(act)>;
        
        if constexpr (std::is_same_v<T, actions::SelectObject>) {
            // Select object for editing
            if (model.scene.objects.find(act.object_id) != model.scene.objects.end()) {
                model.scene.selected_id = act.object_id;
            }
            return model;
        }
        else if constexpr (std::is_same_v<T, actions::SetProperty>) {
            // Modify property of selected object
            if (model.scene.selected_id.empty()) {
                return model;
            }
            
            auto it = model.scene.objects.find(model.scene.selected_id);
            if (it == model.scene.objects.end()) {
                return model;
            }
            
            // Save current state for undo
            model.undo_stack.push_back(model.scene);
            if (model.undo_stack.size() > EditorModel::max_history) {
                model.undo_stack.erase(model.undo_stack.begin());
            }
            model.redo_stack.clear();
            
            // Update property
            Path path = parse_property_path(act.property_path);
            SceneObject updated_obj = it->second;
            updated_obj.data = set_value_at_path(updated_obj.data, path, act.new_value);
            model.scene.objects[model.scene.selected_id] = updated_obj;
            model.scene.version++;
            model.dirty = true;
            
            return model;
        }
        else if constexpr (std::is_same_v<T, actions::SetProperties>) {
            // Batch update multiple properties
            if (model.scene.selected_id.empty()) {
                return model;
            }
            
            auto it = model.scene.objects.find(model.scene.selected_id);
            if (it == model.scene.objects.end()) {
                return model;
            }
            
            // Save current state for undo
            model.undo_stack.push_back(model.scene);
            if (model.undo_stack.size() > EditorModel::max_history) {
                model.undo_stack.erase(model.undo_stack.begin());
            }
            model.redo_stack.clear();
            
            // Update all properties
            SceneObject updated_obj = it->second;
            for (const auto& [path_str, value] : act.updates) {
                Path path = parse_property_path(path_str);
                updated_obj.data = set_value_at_path(updated_obj.data, path, value);
            }
            model.scene.objects[model.scene.selected_id] = updated_obj;
            model.scene.version++;
            model.dirty = true;
            
            return model;
        }
        else if constexpr (std::is_same_v<T, actions::Undo>) {
            if (model.undo_stack.empty()) {
                return model;
            }
            
            // Save current state for redo
            model.redo_stack.push_back(model.scene);
            
            // Restore previous state
            model.scene = model.undo_stack.back();
            model.undo_stack.pop_back();
            model.dirty = true;
            
            return model;
        }
        else if constexpr (std::is_same_v<T, actions::Redo>) {
            if (model.redo_stack.empty()) {
                return model;
            }
            
            // Save current state for undo
            model.undo_stack.push_back(model.scene);
            
            // Restore next state
            model.scene = model.redo_stack.back();
            model.redo_stack.pop_back();
            model.dirty = true;
            
            return model;
        }
        else if constexpr (std::is_same_v<T, actions::SyncFromEngine>) {
            // Full sync from engine (replaces current state)
            model.scene = act.new_state;
            model.undo_stack.clear();
            model.redo_stack.clear();
            model.dirty = false;
            
            return model;
        }
        else if constexpr (std::is_same_v<T, actions::AddObject>) {
            // Save for undo
            model.undo_stack.push_back(model.scene);
            model.redo_stack.clear();
            
            // Add the object
            model.scene.objects[act.object.id] = act.object;
            
            // Add to parent's children list
            if (!act.parent_id.empty()) {
                auto parent_it = model.scene.objects.find(act.parent_id);
                if (parent_it != model.scene.objects.end()) {
                    parent_it->second.children.push_back(act.object.id);
                }
            }
            
            model.scene.version++;
            model.dirty = true;
            
            return model;
        }
        else if constexpr (std::is_same_v<T, actions::RemoveObject>) {
            auto it = model.scene.objects.find(act.object_id);
            if (it == model.scene.objects.end()) {
                return model;
            }
            
            // Save for undo
            model.undo_stack.push_back(model.scene);
            model.redo_stack.clear();
            
            // Remove from parent's children list
            for (auto& [id, obj] : model.scene.objects) {
                auto child_it = std::find(obj.children.begin(), obj.children.end(), act.object_id);
                if (child_it != obj.children.end()) {
                    obj.children.erase(child_it);
                    break;
                }
            }
            
            // Remove the object
            model.scene.objects.erase(it);
            
            // Clear selection if removed object was selected
            if (model.scene.selected_id == act.object_id) {
                model.scene.selected_id.clear();
            }
            
            model.scene.version++;
            model.dirty = true;
            
            return model;
        }
        
        return model;
    }, action);
}

// ============================================================
// EngineSimulator Implementation
// ============================================================

struct EngineSimulator::Impl {
    SceneState scene;
    std::vector<EngineCallback> callbacks;
    
    void fire_event(const std::string& event, const Value& data) {
        for (auto& cb : callbacks) {
            if (cb) {
                cb(event, data);
            }
        }
    }
};

EngineSimulator::EngineSimulator() : impl_(std::make_unique<Impl>()) {}
EngineSimulator::~EngineSimulator() = default;

void EngineSimulator::initialize_sample_scene() {
    // Create a sample scene with a few objects
    
    // ===== Transform component metadata =====
    ObjectMeta transform_meta;
    transform_meta.type_name = "Transform";
    transform_meta.icon_name = "transform_icon";
    transform_meta.properties = {
        {"position.x", "Position X", "X coordinate", "Transform", WidgetType::DoubleSpinBox,
         NumericRange{-1000.0, 1000.0, 0.1}, std::nullopt, false, true, 0},
        {"position.y", "Position Y", "Y coordinate", "Transform", WidgetType::DoubleSpinBox,
         NumericRange{-1000.0, 1000.0, 0.1}, std::nullopt, false, true, 1},
        {"position.z", "Position Z", "Z coordinate", "Transform", WidgetType::DoubleSpinBox,
         NumericRange{-1000.0, 1000.0, 0.1}, std::nullopt, false, true, 2},
        {"rotation.x", "Rotation X", "X rotation in degrees", "Transform", WidgetType::Slider,
         NumericRange{-180.0, 180.0, 1.0}, std::nullopt, false, true, 3},
        {"rotation.y", "Rotation Y", "Y rotation in degrees", "Transform", WidgetType::Slider,
         NumericRange{-180.0, 180.0, 1.0}, std::nullopt, false, true, 4},
        {"rotation.z", "Rotation Z", "Z rotation in degrees", "Transform", WidgetType::Slider,
         NumericRange{-180.0, 180.0, 1.0}, std::nullopt, false, true, 5},
        {"scale.x", "Scale X", "X scale factor", "Transform", WidgetType::DoubleSpinBox,
         NumericRange{0.01, 100.0, 0.1}, std::nullopt, false, true, 6},
        {"scale.y", "Scale Y", "Y scale factor", "Transform", WidgetType::DoubleSpinBox,
         NumericRange{0.01, 100.0, 0.1}, std::nullopt, false, true, 7},
        {"scale.z", "Scale Z", "Z scale factor", "Transform", WidgetType::DoubleSpinBox,
         NumericRange{0.01, 100.0, 0.1}, std::nullopt, false, true, 8},
    };
    
    // ===== Light component metadata =====
    ObjectMeta light_meta;
    light_meta.type_name = "Light";
    light_meta.icon_name = "light_icon";
    light_meta.properties = {
        {"name", "Name", "Object name", "General", WidgetType::LineEdit,
         std::nullopt, std::nullopt, false, true, 0},
        {"type", "Light Type", "Type of light source", "Light", WidgetType::ComboBox,
         std::nullopt, ComboOptions{{"Point", "Directional", "Spot"}, 0}, false, true, 1},
        {"color", "Color", "Light color", "Light", WidgetType::ColorPicker,
         std::nullopt, std::nullopt, false, true, 2},
        {"intensity", "Intensity", "Light intensity", "Light", WidgetType::Slider,
         NumericRange{0.0, 10.0, 0.1}, std::nullopt, false, true, 3},
        {"enabled", "Enabled", "Is light enabled", "Light", WidgetType::CheckBox,
         std::nullopt, std::nullopt, false, true, 4},
    };
    
    // ===== Mesh component metadata =====
    ObjectMeta mesh_meta;
    mesh_meta.type_name = "MeshRenderer";
    mesh_meta.icon_name = "mesh_icon";
    mesh_meta.properties = {
        {"name", "Name", "Object name", "General", WidgetType::LineEdit,
         std::nullopt, std::nullopt, false, true, 0},
        {"mesh_path", "Mesh", "Path to mesh file", "Mesh", WidgetType::FileSelector,
         std::nullopt, std::nullopt, false, true, 1},
        {"material", "Material", "Material name", "Mesh", WidgetType::LineEdit,
         std::nullopt, std::nullopt, false, true, 2},
        {"visible", "Visible", "Is mesh visible", "Mesh", WidgetType::CheckBox,
         std::nullopt, std::nullopt, false, true, 3},
        {"cast_shadows", "Cast Shadows", "Does mesh cast shadows", "Mesh", WidgetType::CheckBox,
         std::nullopt, std::nullopt, false, true, 4},
    };
    
    // ===== Create Root object =====
    SceneObject root;
    root.id = "root";
    root.type = "Transform";
    root.meta = transform_meta;
    root.children = {"camera_main", "light_sun", "cube_1"};
    
    // Create root data
    ValueMap position, rotation, scale;
    position = position.set("x", ValueBox{Value{0.0}});
    position = position.set("y", ValueBox{Value{0.0}});
    position = position.set("z", ValueBox{Value{0.0}});
    
    rotation = rotation.set("x", ValueBox{Value{0.0}});
    rotation = rotation.set("y", ValueBox{Value{0.0}});
    rotation = rotation.set("z", ValueBox{Value{0.0}});
    
    scale = scale.set("x", ValueBox{Value{1.0}});
    scale = scale.set("y", ValueBox{Value{1.0}});
    scale = scale.set("z", ValueBox{Value{1.0}});
    
    ValueMap root_data;
    root_data = root_data.set("position", ValueBox{Value{position}});
    root_data = root_data.set("rotation", ValueBox{Value{rotation}});
    root_data = root_data.set("scale", ValueBox{Value{scale}});
    root.data = Value{root_data};
    
    // ===== Create Light object =====
    SceneObject light;
    light.id = "light_sun";
    light.type = "Light";
    light.meta = light_meta;
    
    ValueMap light_data;
    light_data = light_data.set("name", ValueBox{Value{std::string{"Sun Light"}}});
    light_data = light_data.set("type", ValueBox{Value{std::string{"Directional"}}});
    light_data = light_data.set("color", ValueBox{Value{std::string{"#FFFFCC"}}});
    light_data = light_data.set("intensity", ValueBox{Value{1.5}});
    light_data = light_data.set("enabled", ValueBox{Value{true}});
    light.data = Value{light_data};
    
    // ===== Create Mesh object =====
    SceneObject cube;
    cube.id = "cube_1";
    cube.type = "MeshRenderer";
    cube.meta = mesh_meta;
    
    ValueMap cube_data;
    cube_data = cube_data.set("name", ValueBox{Value{std::string{"Main Cube"}}});
    cube_data = cube_data.set("mesh_path", ValueBox{Value{std::string{"/meshes/cube.fbx"}}});
    cube_data = cube_data.set("material", ValueBox{Value{std::string{"default_material"}}});
    cube_data = cube_data.set("visible", ValueBox{Value{true}});
    cube_data = cube_data.set("cast_shadows", ValueBox{Value{true}});
    cube.data = Value{cube_data};
    
    // ===== Create Camera object (using transform meta) =====
    SceneObject camera;
    camera.id = "camera_main";
    camera.type = "Transform";
    camera.meta = transform_meta;
    
    ValueMap cam_position, cam_rotation, cam_scale;
    cam_position = cam_position.set("x", ValueBox{Value{0.0}});
    cam_position = cam_position.set("y", ValueBox{Value{5.0}});
    cam_position = cam_position.set("z", ValueBox{Value{-10.0}});
    
    cam_rotation = cam_rotation.set("x", ValueBox{Value{15.0}});
    cam_rotation = cam_rotation.set("y", ValueBox{Value{0.0}});
    cam_rotation = cam_rotation.set("z", ValueBox{Value{0.0}});
    
    cam_scale = cam_scale.set("x", ValueBox{Value{1.0}});
    cam_scale = cam_scale.set("y", ValueBox{Value{1.0}});
    cam_scale = cam_scale.set("z", ValueBox{Value{1.0}});
    
    ValueMap camera_data;
    camera_data = camera_data.set("position", ValueBox{Value{cam_position}});
    camera_data = camera_data.set("rotation", ValueBox{Value{cam_rotation}});
    camera_data = camera_data.set("scale", ValueBox{Value{cam_scale}});
    camera.data = Value{camera_data};
    
    // ===== Build scene =====
    impl_->scene.objects["root"] = root;
    impl_->scene.objects["light_sun"] = light;
    impl_->scene.objects["cube_1"] = cube;
    impl_->scene.objects["camera_main"] = camera;
    impl_->scene.root_id = "root";
    impl_->scene.version = 1;
}

SceneState EngineSimulator::get_initial_state() const {
    return impl_->scene;
}

void EngineSimulator::apply_diff(const DiffResult& diff) {
    std::cout << "[Engine] Applying diff with " 
              << diff.added.size() << " additions, "
              << diff.removed.size() << " removals, "
              << diff.modified.size() << " modifications\n";
    
    // In a real implementation, we would:
    // 1. Parse the paths to find which objects are affected
    // 2. Update the corresponding runtime objects
    // 3. Trigger necessary updates (e.g., re-render)
    
    for (const auto& mod : diff.modified) {
        std::cout << "  Modified: " << path_to_string(mod.path) 
                  << " = " << value_to_string(mod.new_value) << "\n";
    }
    
    impl_->fire_event("diff_applied", Value{});
}

void EngineSimulator::apply_full_state(const Value& state) {
    std::cout << "[Engine] Applying full state update\n";
    impl_->fire_event("state_updated", state);
}

Value EngineSimulator::get_state_as_value() const {
    // Convert scene to Value for serialization
    ValueMap objects_map;
    for (const auto& [id, obj] : impl_->scene.objects) {
        objects_map = objects_map.set(id, ValueBox{obj.data});
    }
    
    ValueMap scene_value;
    scene_value = scene_value.set("objects", ValueBox{Value{objects_map}});
    scene_value = scene_value.set("root_id", ValueBox{Value{impl_->scene.root_id}});
    scene_value = scene_value.set("version", ValueBox{Value{static_cast<int>(impl_->scene.version)}});
    
    return Value{scene_value};
}

void EngineSimulator::on_event(EngineCallback callback) {
    impl_->callbacks.push_back(std::move(callback));
}

void EngineSimulator::print_state() const {
    std::cout << "\n=== Engine Scene State ===\n";
    std::cout << "Root: " << impl_->scene.root_id << "\n";
    std::cout << "Version: " << impl_->scene.version << "\n";
    std::cout << "Objects:\n";
    
    for (const auto& [id, obj] : impl_->scene.objects) {
        std::cout << "  [" << id << "] Type: " << obj.type << "\n";
        std::cout << "    Data:\n";
        print_value(obj.data, "      ", 3);
    }
}

// ============================================================
// EditorController Implementation
// ============================================================

struct EditorController::Impl {
    EditorModel model;
    EditorEffects effects;
    std::vector<WatchCallback> watchers;
    Value previous_state_value;  // For diff calculation
    
    void notify_watchers() {
        for (auto& watcher : watchers) {
            if (watcher) {
                watcher(model);
            }
        }
    }
    
    void check_and_notify_changes() {
        if (!model.dirty) return;
        
        // Get current state as Value for diff
        Value current_state_value = scene_to_value(model.scene);
        
        // Calculate diff
        if (effects.on_state_changed) {
            DiffResult diff = collect_diff(previous_state_value, current_state_value);
            if (!diff.empty()) {
                effects.on_state_changed(diff);
            }
        }
        
        previous_state_value = current_state_value;
        model.dirty = false;
    }
    
    static Value scene_to_value(const SceneState& scene) {
        ValueMap objects_map;
        for (const auto& [id, obj] : scene.objects) {
            objects_map = objects_map.set(id, ValueBox{obj.data});
        }
        
        ValueMap scene_map;
        scene_map = scene_map.set("objects", ValueBox{Value{objects_map}});
        scene_map = scene_map.set("selected_id", ValueBox{Value{scene.selected_id}});
        scene_map = scene_map.set("version", ValueBox{Value{static_cast<int>(scene.version)}});
        
        return Value{scene_map};
    }
};

EditorController::EditorController() : impl_(std::make_unique<Impl>()) {}
EditorController::~EditorController() = default;

void EditorController::initialize(const SceneState& initial_state) {
    impl_->model.scene = initial_state;
    impl_->model.undo_stack.clear();
    impl_->model.redo_stack.clear();
    impl_->model.dirty = false;
    impl_->previous_state_value = Impl::scene_to_value(initial_state);
}

void EditorController::dispatch(EditorAction action) {
    // Track selection changes
    std::string old_selection = impl_->model.scene.selected_id;
    
    // Apply action through reducer
    impl_->model = editor_update(impl_->model, action);
    
    // Notify selection change
    if (impl_->model.scene.selected_id != old_selection) {
        if (impl_->effects.on_selection_changed) {
            impl_->effects.on_selection_changed(impl_->model.scene.selected_id);
        }
    }
    
    // Check and notify state changes
    impl_->check_and_notify_changes();
    
    // Notify watchers
    impl_->notify_watchers();
}

const EditorModel& EditorController::get_model() const {
    return impl_->model;
}

const SceneObject* EditorController::get_selected_object() const {
    if (impl_->model.scene.selected_id.empty()) {
        return nullptr;
    }
    
    auto it = impl_->model.scene.objects.find(impl_->model.scene.selected_id);
    if (it == impl_->model.scene.objects.end()) {
        return nullptr;
    }
    
    return &it->second;
}

std::optional<Value> EditorController::get_property(const std::string& path) const {
    const SceneObject* obj = get_selected_object();
    if (!obj) return std::nullopt;
    
    Path parsed_path = parse_property_path(path);
    Value result = get_value_at_path(obj->data, parsed_path);
    
    // Check if we got a valid value (not null unless explicitly null)
    if (result.is_null() && !parsed_path.empty()) {
        // Try to check if the path exists
        Value parent = obj->data;
        for (size_t i = 0; i < parsed_path.size() - 1; ++i) {
            if (auto* key = std::get_if<std::string>(&parsed_path[i])) {
                parent = parent.at(*key);
            }
        }
        if (parent.is_null()) {
            return std::nullopt;
        }
    }
    
    return result;
}

void EditorController::set_property(const std::string& path, Value value) {
    dispatch(actions::SetProperty{path, std::move(value)});
}

bool EditorController::can_undo() const {
    return !impl_->model.undo_stack.empty();
}

bool EditorController::can_redo() const {
    return !impl_->model.redo_stack.empty();
}

void EditorController::undo() {
    dispatch(actions::Undo{});
}

void EditorController::redo() {
    dispatch(actions::Redo{});
}

void EditorController::set_effects(EditorEffects effects) {
    impl_->effects = std::move(effects);
}

void EditorController::step() {
    // For manual event loop - process any pending operations
    impl_->check_and_notify_changes();
}

std::function<void()> EditorController::watch(WatchCallback callback) {
    impl_->watchers.push_back(callback);
    size_t index = impl_->watchers.size() - 1;
    
    // Return unsubscribe function
    return [this, index]() {
        if (index < impl_->watchers.size()) {
            impl_->watchers[index] = nullptr;
        }
    };
}

// ============================================================
// Qt UI Binding Helpers
// ============================================================

std::vector<PropertyBinding> generate_property_bindings(
    EditorController& controller,
    const SceneObject& object)
{
    std::vector<PropertyBinding> bindings;
    
    for (const auto& prop : object.meta.properties) {
        PropertyBinding binding;
        binding.property_path = prop.name;
        binding.meta = prop;
        
        // Create getter closure
        std::string path = prop.name;
        binding.getter = [&controller, path]() -> Value {
            auto val = controller.get_property(path);
            return val.value_or(Value{});
        };
        
        // Create setter closure
        binding.setter = [&controller, path](Value value) {
            controller.set_property(path, std::move(value));
        };
        
        bindings.push_back(std::move(binding));
    }
    
    return bindings;
}

// ============================================================
// Helper: Print widget type name
// ============================================================
static std::string widget_type_name(WidgetType type) {
    switch (type) {
        case WidgetType::LineEdit: return "QLineEdit";
        case WidgetType::SpinBox: return "QSpinBox";
        case WidgetType::DoubleSpinBox: return "QDoubleSpinBox";
        case WidgetType::CheckBox: return "QCheckBox";
        case WidgetType::ColorPicker: return "ColorPicker";
        case WidgetType::Slider: return "QSlider";
        case WidgetType::ComboBox: return "QComboBox";
        case WidgetType::Vector3Edit: return "Vector3Edit";
        case WidgetType::FileSelector: return "QFileDialog";
        case WidgetType::ReadOnly: return "QLabel";
        default: return "Unknown";
    }
}

// ============================================================
// Demo: Complete Editor-Engine Flow
// ============================================================

void demo_editor_engine() {
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║     Editor-Engine Cross-Process State Management Demo        ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";
    
    // ===== Step 1: Initialize Engine (Process B) =====
    std::cout << "=== Step 1: Initialize Engine (Process B) ===\n";
    EngineSimulator engine;
    engine.initialize_sample_scene();
    std::cout << "Engine initialized with sample scene.\n";
    engine.print_state();
    
    // ===== Step 2: Editor Gets Initial State (Process A) =====
    std::cout << "\n=== Step 2: Editor Gets Initial State (Process A) ===\n";
    EditorController editor;
    
    // Set up effects to notify engine of changes
    editor.set_effects({
        // on_state_changed - send diff to engine
        [&engine](const DiffResult& diff) {
            std::cout << "\n[Editor -> Engine] State changed, sending diff...\n";
            engine.apply_diff(diff);
        },
        // on_selection_changed
        [](const std::string& object_id) {
            std::cout << "[Editor] Selection changed to: " << object_id << "\n";
        }
    });
    
    SceneState initial_state = engine.get_initial_state();
    editor.initialize(initial_state);
    std::cout << "Editor initialized with " << initial_state.objects.size() << " objects.\n";
    
    // ===== Step 3: Select an Object for Editing =====
    std::cout << "\n=== Step 3: Select Object for Editing ===\n";
    editor.dispatch(actions::SelectObject{"light_sun"});
    
    const SceneObject* selected = editor.get_selected_object();
    if (selected) {
        std::cout << "Selected: " << selected->id << " (Type: " << selected->type << ")\n";
        std::cout << "Current data:\n";
        print_value(selected->data, "  ", 1);
    }
    
    // ===== Step 4: Generate Qt UI Bindings =====
    std::cout << "\n=== Step 4: Generate Qt UI Bindings ===\n";
    if (selected) {
        auto bindings = generate_property_bindings(editor, *selected);
        std::cout << "Generated " << bindings.size() << " property bindings:\n";
        
        for (const auto& binding : bindings) {
            std::cout << "  - " << binding.meta.display_name 
                      << " (" << binding.property_path << ")"
                      << " -> " << widget_type_name(binding.meta.widget_type);
            
            if (binding.meta.range) {
                std::cout << " [" << binding.meta.range->min_value 
                          << " - " << binding.meta.range->max_value << "]";
            }
            
            // Show current value
            Value current = binding.getter();
            std::cout << " = " << value_to_string(current) << "\n";
        }
    }
    
    // ===== Step 5: Edit Property (simulating Qt UI interaction) =====
    std::cout << "\n=== Step 5: Edit Property (Qt UI Simulation) ===\n";
    std::cout << "Changing light intensity from 1.5 to 2.0...\n";
    editor.set_property("intensity", Value{2.0});
    
    selected = editor.get_selected_object();
    if (selected) {
        std::cout << "Updated data:\n";
        print_value(selected->data, "  ", 1);
    }
    
    // ===== Step 6: Edit Another Property =====
    std::cout << "\n=== Step 6: Edit Another Property ===\n";
    std::cout << "Changing light color to #FF0000...\n";
    editor.set_property("color", Value{std::string{"#FF0000"}});
    
    // ===== Step 7: Undo/Redo Demo =====
    std::cout << "\n=== Step 7: Undo/Redo Demo ===\n";
    std::cout << "Can undo: " << (editor.can_undo() ? "yes" : "no") << "\n";
    std::cout << "Can redo: " << (editor.can_redo() ? "yes" : "no") << "\n";
    
    std::cout << "\nPerforming UNDO...\n";
    editor.undo();
    
    selected = editor.get_selected_object();
    if (selected) {
        auto color = editor.get_property("color");
        std::cout << "Color after undo: " << value_to_string(color.value_or(Value{})) << "\n";
    }
    
    std::cout << "\nPerforming REDO...\n";
    editor.redo();
    
    selected = editor.get_selected_object();
    if (selected) {
        auto color = editor.get_property("color");
        std::cout << "Color after redo: " << value_to_string(color.value_or(Value{})) << "\n";
    }
    
    // ===== Step 8: Switch to Different Object =====
    std::cout << "\n=== Step 8: Switch to Different Object ===\n";
    editor.dispatch(actions::SelectObject{"cube_1"});
    
    selected = editor.get_selected_object();
    if (selected) {
        std::cout << "Now editing: " << selected->id << " (Type: " << selected->type << ")\n";
        std::cout << "Properties:\n";
        
        auto bindings = generate_property_bindings(editor, *selected);
        for (const auto& binding : bindings) {
            Value current = binding.getter();
            std::cout << "  " << binding.meta.display_name << ": " 
                      << value_to_string(current) << "\n";
        }
    }
    
    // ===== Summary =====
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                      Demo Summary                            ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  1. Engine creates scene objects with reflection data        ║\n";
    std::cout << "║  2. Editor receives initial state from Engine                ║\n";
    std::cout << "║  3. User selects object -> Qt UI is generated from metadata ║\n";
    std::cout << "║  4. User edits property -> State updated via lager reducer   ║\n";
    std::cout << "║  5. State diff is sent to Engine for application             ║\n";
    std::cout << "║  6. Undo/Redo works through state history stack              ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";
}

// ============================================================
// Demo: Property Editing with Qt Binding Simulation
// ============================================================

void demo_property_editing() {
    std::cout << "\n=== Property Editing Demo ===\n\n";
    
    EngineSimulator engine;
    engine.initialize_sample_scene();
    
    EditorController editor;
    editor.initialize(engine.get_initial_state());
    
    // Select the camera object
    editor.dispatch(actions::SelectObject{"camera_main"});
    
    const SceneObject* camera = editor.get_selected_object();
    if (!camera) {
        std::cout << "Failed to select camera!\n";
        return;
    }
    
    std::cout << "Editing: " << camera->id << "\n";
    std::cout << "Initial position.y: " << value_to_string(editor.get_property("position.y").value_or(Value{})) << "\n";
    
    // Simulate UI editing - change position Y
    std::cout << "\nSimulating slider change: position.y -> 10.0\n";
    editor.set_property("position.y", Value{10.0});
    
    std::cout << "New position.y: " << value_to_string(editor.get_property("position.y").value_or(Value{})) << "\n";
    
    // Batch update
    std::cout << "\nSimulating batch update (drag 3D gizmo):\n";
    editor.dispatch(actions::SetProperties{{
        {"position.x", Value{5.0}},
        {"position.y", Value{7.5}},
        {"position.z", Value{-15.0}}
    }});
    
    std::cout << "New position: ("
              << value_to_string(editor.get_property("position.x").value_or(Value{})) << ", "
              << value_to_string(editor.get_property("position.y").value_or(Value{})) << ", "
              << value_to_string(editor.get_property("position.z").value_or(Value{})) << ")\n";
    
    std::cout << "\n=== Demo End ===\n\n";
}

// ============================================================
// Demo: Undo/Redo Functionality
// ============================================================

void demo_undo_redo() {
    std::cout << "\n=== Undo/Redo Demo ===\n\n";
    
    EngineSimulator engine;
    engine.initialize_sample_scene();
    
    EditorController editor;
    editor.set_effects({
        [](const DiffResult& diff) {
            std::cout << "  [Diff] " << diff.modified.size() << " modifications\n";
        },
        nullptr
    });
    
    editor.initialize(engine.get_initial_state());
    editor.dispatch(actions::SelectObject{"light_sun"});
    
    std::cout << "Initial intensity: " 
              << value_to_string(editor.get_property("intensity").value_or(Value{})) << "\n";
    
    // Make several changes
    std::cout << "\n--- Making changes ---\n";
    
    std::cout << "Set intensity = 2.0\n";
    editor.set_property("intensity", Value{2.0});
    
    std::cout << "Set intensity = 3.0\n";
    editor.set_property("intensity", Value{3.0});
    
    std::cout << "Set intensity = 4.0\n";
    editor.set_property("intensity", Value{4.0});
    
    std::cout << "\nCurrent intensity: " 
              << value_to_string(editor.get_property("intensity").value_or(Value{})) << "\n";
    std::cout << "Undo stack size: " << editor.get_model().undo_stack.size() << "\n";
    std::cout << "Redo stack size: " << editor.get_model().redo_stack.size() << "\n";
    
    // Undo all changes
    std::cout << "\n--- Undoing all changes ---\n";
    while (editor.can_undo()) {
        editor.undo();
        std::cout << "After undo: intensity = " 
                  << value_to_string(editor.get_property("intensity").value_or(Value{})) << "\n";
    }
    
    // Redo all changes
    std::cout << "\n--- Redoing all changes ---\n";
    while (editor.can_redo()) {
        editor.redo();
        std::cout << "After redo: intensity = " 
                  << value_to_string(editor.get_property("intensity").value_or(Value{})) << "\n";
    }
    
    std::cout << "\n=== Demo End ===\n\n";
}

} // namespace immer_lens
