// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file scene_types.h
/// @brief Common types shared across editor engines.
///
/// This file contains shared type definitions used by:
/// - editor_engine.h (snapshot-based undo)
/// - delta_undo_engine.h (delta-based undo)
/// - multi_store_engine.h (multi-store architecture)
///
/// Centralizing these types avoids code duplication and ensures consistency.

#pragma once

#include <lager_ext/value.h>

#include <immer/map.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lager_ext {

// ============================================================
// UI Metadata - Information for generating Qt UI widgets
// ============================================================

/// Widget type hints for Qt UI generation
enum class WidgetType {
    LineEdit,       ///< QString, single line text
    SpinBox,        ///< int
    DoubleSpinBox,  ///< float/double
    CheckBox,       ///< bool
    ColorPicker,    ///< color (stored as int or string)
    Slider,         ///< int/float with range
    ComboBox,       ///< enum or string selection
    Vector3Edit,    ///< 3D vector (x, y, z)
    FileSelector,   ///< file path
    ReadOnly,       ///< display only, not editable
};

/// Range constraint for numeric values
struct NumericRange {
    double min_value = 0.0;
    double max_value = 100.0;
    double step = 1.0;

    bool operator==(const NumericRange&) const = default;
};

/// Combo box options
struct ComboOptions {
    std::vector<std::string> options;
    int default_index = 0;

    bool operator==(const ComboOptions&) const = default;
};

/// Property UI metadata
struct PropertyMeta {
    std::string name;           ///< Property name (key in Value map)
    std::string display_name;   ///< Human-readable name for UI
    std::string tooltip;        ///< Tooltip text
    std::string category;       ///< Category for grouping in property editor
    WidgetType widget_type = WidgetType::LineEdit;

    /// Optional constraints
    std::optional<NumericRange> range;
    std::optional<ComboOptions> combo_options;

    bool read_only = false;
    bool visible = true;
    int sort_order = 0;         ///< For ordering in UI

    bool operator==(const PropertyMeta&) const = default;
};

/// UI metadata for scene objects (collection of property metadata)
/// Used to generate Qt property editor widgets
struct UIMeta {
    std::string type_name;      ///< Object type (e.g., "Transform", "Light")
    std::string icon_name;      ///< Icon for tree view
    std::vector<PropertyMeta> properties;

    /// Find property meta by name
    [[nodiscard]] const PropertyMeta* find_property(const std::string& name) const noexcept {
        for (const auto& prop : properties) {
            if (prop.name == name) return &prop;
        }
        return nullptr;
    }

    bool operator==(const UIMeta&) const = default;
};

// ============================================================
// Scene Object Structure
// ============================================================

/// Scene object with value data and metadata
struct SceneObject {
    std::string id;             ///< Unique object ID
    std::string type;           ///< Object type name
    Value data;                 ///< Object properties as Value
    UIMeta meta;                ///< UI metadata for Qt binding

    std::vector<std::string> children;  ///< Child object IDs

    // Note: Cannot use defaulted operator== due to Value type
};

/// Complete scene state - using immer::map for structural sharing benefits
struct SceneState {
    immer::map<std::string, SceneObject> objects;  ///< All objects by ID (immutable)
    std::string root_id;                            ///< Root object ID
    std::string selected_id;                        ///< Currently selected object
    uint64_t version = 0;                           ///< State version
};

} // namespace lager_ext
