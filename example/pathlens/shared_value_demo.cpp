// shared_value_demo.cpp
// Demonstrates SharedValue cross-process zero-copy transfer
//
// This demo shows:
// 1. How process B creates shared memory and writes Value
// 2. How process A opens shared memory and deep copies to local
// 3. Performance comparison: shared memory vs serialization

// Must be defined before Windows.h to prevent min/max macro conflicts
#define NOMINMAX

#include "shared_value.h"
#include "value.h"

#include <chrono>
#include <iostream>
#include <iomanip>
#include <thread>
#include <sstream>

using namespace immer_lens;

//==============================================================================
// Performance Testing Utilities
//==============================================================================

inline uint64_t get_timestamp_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

class Timer {
public:
    void start() {
        start_ = std::chrono::high_resolution_clock::now();
    }
    
    double elapsed_ms() const {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(end - start_).count();
    }
    
private:
    std::chrono::high_resolution_clock::time_point start_;
};

//==============================================================================
// Test Data Generation - Using real scene_object_map.json structure
//==============================================================================

// Helper: generate a UUID-like ID (e.g., "9993E719-8830D0A6-ADD6393F-F677E33E")
std::string generate_uuid_like_id(size_t index) {
    // Use index-based deterministic generation for reproducibility
    uint32_t a = static_cast<uint32_t>(index * 0x9E3779B9 + 0x12345678);
    uint32_t b = static_cast<uint32_t>(index * 0x85EBCA6B + 0x87654321);
    uint32_t c = static_cast<uint32_t>(index * 0xC2B2AE35 + 0xABCDEF01);
    uint32_t d = static_cast<uint32_t>(index * 0x27D4EB2F + 0xFEDCBA98);
    
    char buf[64];
    snprintf(buf, sizeof(buf), "%08X-%08X-%08X-%08X", a, b, c, d);
    return std::string(buf);
}

// Helper: create a single realistic scene object (Value version)
// Structure based on D:\scene_object_map.json
ValueMap create_scene_object(size_t index) {
    std::string id = generate_uuid_like_id(index);
    
    ValueMap obj;
    
    // Basic properties from scene_object_map
    ValueMap property;
    property = property.set("name", ValueBox{Value{"IEntity"}});
    obj = obj.set("property", ValueBox{Value{property}});
    
    obj = obj.set("filename", ValueBox{Value{""}});
    obj = obj.set("space_object_type", ValueBox{Value{static_cast<int64_t>(1048576)}});
    obj = obj.set("scene_object_id", ValueBox{Value{id}});
    obj = obj.set("parent", ValueBox{Value{"A7DC0D1A-7B421DB0-5B8D7D86-FDB2A65F"}});
    obj = obj.set("level", ValueBox{Value{"CB9552E0-F1495927-71830CA6-BE6E082F"}});
    
    // Transform: position, scale, euler (Vec3)
    ValueVector position;
    position = position.push_back(ValueBox{Value{static_cast<double>(index % 1000)}});
    position = position.push_back(ValueBox{Value{0.06}});
    position = position.push_back(ValueBox{Value{static_cast<double>((index / 1000) % 1000)}});
    obj = obj.set("position", ValueBox{Value{position}});
    
    ValueVector scale;
    scale = scale.push_back(ValueBox{Value{1.0}});
    scale = scale.push_back(ValueBox{Value{1.0}});
    scale = scale.push_back(ValueBox{Value{1.0}});
    obj = obj.set("scale", ValueBox{Value{scale}});
    
    ValueVector euler;
    euler = euler.push_back(ValueBox{Value{0.0}});
    euler = euler.push_back(ValueBox{Value{static_cast<double>(index % 360)}});
    euler = euler.push_back(ValueBox{Value{0.0}});
    obj = obj.set("euler", ValueBox{Value{euler}});
    
    // Boolean and integer properties
    obj = obj.set("visible_mask", ValueBox{Value{true}});
    obj = obj.set("in_world", ValueBox{Value{true}});
    obj = obj.set("scene_object_layer", ValueBox{Value{static_cast<int64_t>(143)}});
    obj = obj.set("name", ValueBox{Value{"SM_CM_1L_Building_" + std::to_string(index)}});
    obj = obj.set("file", ValueBox{Value{"Scenes/Architecture/CloudMansion/Structure/SM_CM_L1_Building_" + std::to_string(index % 100)}});
    obj = obj.set("scene_object_locked", ValueBox{Value{false}});
    obj = obj.set("scene_object_type", ValueBox{Value{static_cast<int64_t>(9)}});
    obj = obj.set("ModelResource", ValueBox{Value{"Scenes/Architecture/CloudMansion/Structure/SM_CM_L1_Building_" + std::to_string(index % 100)}});
    
    // PropertyData - complex nested structure
    ValueMap propertyData;
    propertyData = propertyData.set("GenerateOccluder", ValueBox{Value{false}});
    propertyData = propertyData.set("DeleteOccluder", ValueBox{Value{false}});
    propertyData = propertyData.set("IsVisible", ValueBox{Value{true}});
    propertyData = propertyData.set("IsDisableCollision", ValueBox{Value{false}});
    propertyData = propertyData.set("IsBillboard", ValueBox{Value{false}});
    propertyData = propertyData.set("IsReflectionVisible", ValueBox{Value{false}});
    propertyData = propertyData.set("IsOutlined", ValueBox{Value{false}});
    propertyData = propertyData.set("IsThermalVisible", ValueBox{Value{false}});
    propertyData = propertyData.set("DetailLevel", ValueBox{Value{static_cast<int64_t>(0)}});
    propertyData = propertyData.set("TechState", ValueBox{Value{static_cast<int64_t>(0)}});
    
    // TechParam vectors (Vec3, Vec4)
    ValueVector techParam;
    techParam = techParam.push_back(ValueBox{Value{0.0}});
    techParam = techParam.push_back(ValueBox{Value{0.0}});
    techParam = techParam.push_back(ValueBox{Value{0.0}});
    propertyData = propertyData.set("TechParam", ValueBox{Value{techParam}});
    
    ValueVector techParam2;
    for (int j = 0; j < 4; ++j) techParam2 = techParam2.push_back(ValueBox{Value{0.0}});
    propertyData = propertyData.set("TechParam2", ValueBox{Value{techParam2}});
    
    // TintColor (Vec4)
    ValueVector tintColor;
    for (int j = 0; j < 4; ++j) tintColor = tintColor.push_back(ValueBox{Value{1.0}});
    propertyData = propertyData.set("TintColor1", ValueBox{Value{tintColor}});
    propertyData = propertyData.set("TintColor2", ValueBox{Value{tintColor}});
    propertyData = propertyData.set("TintColor3", ValueBox{Value{tintColor}});
    
    // LodThreshold, Anchor (Vec3)
    propertyData = propertyData.set("LodThreshold", ValueBox{Value{techParam}});
    propertyData = propertyData.set("Anchor", ValueBox{Value{techParam}});
    
    propertyData = propertyData.set("IsCastDynamicShadow", ValueBox{Value{true}});
    propertyData = propertyData.set("IsReceiveDynamicShadow", ValueBox{Value{true}});
    propertyData = propertyData.set("IsSDFGen", ValueBox{Value{true}});
    propertyData = propertyData.set("HasCollision", ValueBox{Value{true}});
    propertyData = propertyData.set("[Type]", ValueBox{Value{"SceneObjectType_9"}});
    propertyData = propertyData.set("WorldName", ValueBox{Value{"L_CloudMansion_02"}});
    propertyData = propertyData.set("LevelName", ValueBox{Value{"L_CloudMansion_Mesh_02"}});
    
    // Primitives array with ModelComponent
    ValueVector primitives;
    ValueMap modelComp;
    modelComp = modelComp.set("CustomRenderSet", ValueBox{Value{static_cast<int64_t>(0)}});
    modelComp = modelComp.set("CustomStencil", ValueBox{Value{static_cast<int64_t>(0)}});
    modelComp = modelComp.set("IsCastDynamicShadow", ValueBox{Value{true}});
    modelComp = modelComp.set("IsReceiveDynamicShadow", ValueBox{Value{true}});
    modelComp = modelComp.set("HasPhysics", ValueBox{Value{true}});
    modelComp = modelComp.set("ReceiveDecals", ValueBox{Value{true}});
    modelComp = modelComp.set("Lightmap", ValueBox{Value{"AuroraAuto/Model_lightmap/L_CloudMansion_02/atlas_0"}});
    modelComp = modelComp.set("[Type]", ValueBox{Value{"ModelComponent"}});
    
    // LightmapScale, LightmapOffset (Vec4)
    ValueVector lmScale;
    lmScale = lmScale.push_back(ValueBox{Value{0.76}});
    lmScale = lmScale.push_back(ValueBox{Value{0.71}});
    lmScale = lmScale.push_back(ValueBox{Value{0.51}});
    lmScale = lmScale.push_back(ValueBox{Value{1.0}});
    modelComp = modelComp.set("LightmapScale", ValueBox{Value{lmScale}});
    modelComp = modelComp.set("LightmapOffset", ValueBox{Value{lmScale}});
    
    // SyncModel sub-component
    ValueMap syncModel;
    syncModel = syncModel.set("GroupID", ValueBox{Value{static_cast<int64_t>(0)}});
    syncModel = syncModel.set("NeedBake", ValueBox{Value{true}});
    syncModel = syncModel.set("NeedGenLitmap", ValueBox{Value{true}});
    syncModel = syncModel.set("NeedCastShadow", ValueBox{Value{true}});
    syncModel = syncModel.set("NeedReceiveShadow", ValueBox{Value{true}});
    syncModel = syncModel.set("Occluder", ValueBox{Value{true}});
    syncModel = syncModel.set("Occludee", ValueBox{Value{true}});
    syncModel = syncModel.set("CastGIScale", ValueBox{Value{1.0}});
    syncModel = syncModel.set("[Type]", ValueBox{Value{"SyncModelComponent"}});
    modelComp = modelComp.set("SyncModel", ValueBox{Value{syncModel}});
    
    primitives = primitives.push_back(ValueBox{Value{modelComp}});
    propertyData = propertyData.set("Primitives", ValueBox{Value{primitives}});
    
    // RigidBodies array
    ValueVector rigidBodies;
    ValueMap rigidBody;
    rigidBody = rigidBody.set("ComponentType", ValueBox{Value{"PhysicsStaticSceneBody"}});
    rigidBody = rigidBody.set("EnableContactNotify", ValueBox{Value{false}});
    rigidBody = rigidBody.set("Unwalkable", ValueBox{Value{false}});
    rigidBody = rigidBody.set("TemplateRes", ValueBox{Value{"Scenes/Architecture/CloudMansion/Structure/AutoPhyRBTemplate"}});
    rigidBody = rigidBody.set("[Type]", ValueBox{Value{"PhysicsStaticSceneBody"}});
    rigidBodies = rigidBodies.push_back(ValueBox{Value{rigidBody}});
    propertyData = propertyData.set("RigidBodies", ValueBox{Value{rigidBodies}});
    
    // Appearance, Tag components
    ValueMap appearance;
    appearance = appearance.set("DepthOffset", ValueBox{Value{static_cast<int64_t>(0)}});
    appearance = appearance.set("[Type]", ValueBox{Value{"IAppearanceComponent"}});
    propertyData = propertyData.set("Appearance", ValueBox{Value{appearance}});
    
    ValueMap tag;
    tag = tag.set("TagString", ValueBox{Value{""}});
    tag = tag.set("[Type]", ValueBox{Value{"TagComponent"}});
    propertyData = propertyData.set("Tag", ValueBox{Value{tag}});
    
    obj = obj.set("PropertyData", ValueBox{Value{propertyData}});
    
    // PropertyPaths array
    ValueVector propertyPaths;
    propertyPaths = propertyPaths.push_back(ValueBox{Value{"PropertyData"}});
    propertyPaths = propertyPaths.push_back(ValueBox{Value{"PropertyData/Primitives/0"}});
    propertyPaths = propertyPaths.push_back(ValueBox{Value{"PropertyData/Primitives/0/SyncModel"}});
    propertyPaths = propertyPaths.push_back(ValueBox{Value{"PropertyData/RigidBodies/0"}});
    obj = obj.set("PropertyPaths", ValueBox{Value{propertyPaths}});
    
    // Components array
    ValueVector components;
    ValueMap comp1;
    comp1 = comp1.set("DisplayName", ValueBox{Value{"[ModelComponent]"}});
    comp1 = comp1.set("Icon", ValueBox{Value{"Comp_Model"}});
    components = components.push_back(ValueBox{Value{comp1}});
    obj = obj.set("Components", ValueBox{Value{components}});
    
    return obj;
}

// Generate large-scale test data using real scene object structure - Value version
Value generate_large_scene(size_t object_count) {
    std::cout << "Generating scene with " << object_count << " objects (Value - single-threaded)...\n";
    std::cout << "Using real scene_object_map.json structure\n";
    
    Timer timer;
    timer.start();
    
    auto objects_transient = ValueMap{}.transient();
    
    for (size_t i = 0; i < object_count; ++i) {
        std::string key = generate_uuid_like_id(i);  // UUID-like key
        ValueMap obj = create_scene_object(i);
        objects_transient.set(key, ValueBox{Value{obj}});
        
        if ((i + 1) % 10000 == 0) {
            std::cout << "  Generated " << (i + 1) << " objects...\n";
        }
    }
    
    ValueMap scene;
    scene = scene.set("scene_object_map", ValueBox{Value{objects_transient.persistent()}});
    
    double elapsed = timer.elapsed_ms();
    std::cout << "Scene generation completed in " << std::fixed << std::setprecision(2) 
              << elapsed << " ms\n";
    
    return Value{scene};
}

// Helper: create a single realistic scene object (SyncValue version)
SyncValueMap create_scene_object_sync(size_t index) {
    std::string id = generate_uuid_like_id(index);
    
    SyncValueMap obj;
    
    // Basic properties from scene_object_map
    SyncValueMap property;
    property = property.set("name", SyncValueBox{SyncValue{"IEntity"}});
    obj = obj.set("property", SyncValueBox{SyncValue{property}});
    
    obj = obj.set("filename", SyncValueBox{SyncValue{""}});
    obj = obj.set("space_object_type", SyncValueBox{SyncValue{static_cast<int64_t>(1048576)}});
    obj = obj.set("scene_object_id", SyncValueBox{SyncValue{id}});
    obj = obj.set("parent", SyncValueBox{SyncValue{"A7DC0D1A-7B421DB0-5B8D7D86-FDB2A65F"}});
    obj = obj.set("level", SyncValueBox{SyncValue{"CB9552E0-F1495927-71830CA6-BE6E082F"}});
    
    // Transform: position, scale, euler (Vec3)
    SyncValueVector position;
    position = position.push_back(SyncValueBox{SyncValue{static_cast<double>(index % 1000)}});
    position = position.push_back(SyncValueBox{SyncValue{0.06}});
    position = position.push_back(SyncValueBox{SyncValue{static_cast<double>((index / 1000) % 1000)}});
    obj = obj.set("position", SyncValueBox{SyncValue{position}});
    
    SyncValueVector scale;
    scale = scale.push_back(SyncValueBox{SyncValue{1.0}});
    scale = scale.push_back(SyncValueBox{SyncValue{1.0}});
    scale = scale.push_back(SyncValueBox{SyncValue{1.0}});
    obj = obj.set("scale", SyncValueBox{SyncValue{scale}});
    
    SyncValueVector euler;
    euler = euler.push_back(SyncValueBox{SyncValue{0.0}});
    euler = euler.push_back(SyncValueBox{SyncValue{static_cast<double>(index % 360)}});
    euler = euler.push_back(SyncValueBox{SyncValue{0.0}});
    obj = obj.set("euler", SyncValueBox{SyncValue{euler}});
    
    // Boolean and integer properties
    obj = obj.set("visible_mask", SyncValueBox{SyncValue{true}});
    obj = obj.set("in_world", SyncValueBox{SyncValue{true}});
    obj = obj.set("scene_object_layer", SyncValueBox{SyncValue{static_cast<int64_t>(143)}});
    obj = obj.set("name", SyncValueBox{SyncValue{"SM_CM_1L_Building_" + std::to_string(index)}});
    obj = obj.set("file", SyncValueBox{SyncValue{"Scenes/Architecture/CloudMansion/Structure/SM_CM_L1_Building_" + std::to_string(index % 100)}});
    obj = obj.set("scene_object_locked", SyncValueBox{SyncValue{false}});
    obj = obj.set("scene_object_type", SyncValueBox{SyncValue{static_cast<int64_t>(9)}});
    obj = obj.set("ModelResource", SyncValueBox{SyncValue{"Scenes/Architecture/CloudMansion/Structure/SM_CM_L1_Building_" + std::to_string(index % 100)}});
    
    // PropertyData - complex nested structure
    SyncValueMap propertyData;
    propertyData = propertyData.set("GenerateOccluder", SyncValueBox{SyncValue{false}});
    propertyData = propertyData.set("DeleteOccluder", SyncValueBox{SyncValue{false}});
    propertyData = propertyData.set("IsVisible", SyncValueBox{SyncValue{true}});
    propertyData = propertyData.set("IsDisableCollision", SyncValueBox{SyncValue{false}});
    propertyData = propertyData.set("IsBillboard", SyncValueBox{SyncValue{false}});
    propertyData = propertyData.set("IsReflectionVisible", SyncValueBox{SyncValue{false}});
    propertyData = propertyData.set("IsOutlined", SyncValueBox{SyncValue{false}});
    propertyData = propertyData.set("IsThermalVisible", SyncValueBox{SyncValue{false}});
    propertyData = propertyData.set("DetailLevel", SyncValueBox{SyncValue{static_cast<int64_t>(0)}});
    propertyData = propertyData.set("TechState", SyncValueBox{SyncValue{static_cast<int64_t>(0)}});
    
    // TechParam vectors (Vec3, Vec4)
    SyncValueVector techParam;
    techParam = techParam.push_back(SyncValueBox{SyncValue{0.0}});
    techParam = techParam.push_back(SyncValueBox{SyncValue{0.0}});
    techParam = techParam.push_back(SyncValueBox{SyncValue{0.0}});
    propertyData = propertyData.set("TechParam", SyncValueBox{SyncValue{techParam}});
    
    SyncValueVector techParam2;
    for (int j = 0; j < 4; ++j) techParam2 = techParam2.push_back(SyncValueBox{SyncValue{0.0}});
    propertyData = propertyData.set("TechParam2", SyncValueBox{SyncValue{techParam2}});
    
    // TintColor (Vec4)
    SyncValueVector tintColor;
    for (int j = 0; j < 4; ++j) tintColor = tintColor.push_back(SyncValueBox{SyncValue{1.0}});
    propertyData = propertyData.set("TintColor1", SyncValueBox{SyncValue{tintColor}});
    propertyData = propertyData.set("TintColor2", SyncValueBox{SyncValue{tintColor}});
    propertyData = propertyData.set("TintColor3", SyncValueBox{SyncValue{tintColor}});
    
    // LodThreshold, Anchor (Vec3)
    propertyData = propertyData.set("LodThreshold", SyncValueBox{SyncValue{techParam}});
    propertyData = propertyData.set("Anchor", SyncValueBox{SyncValue{techParam}});
    
    propertyData = propertyData.set("IsCastDynamicShadow", SyncValueBox{SyncValue{true}});
    propertyData = propertyData.set("IsReceiveDynamicShadow", SyncValueBox{SyncValue{true}});
    propertyData = propertyData.set("IsSDFGen", SyncValueBox{SyncValue{true}});
    propertyData = propertyData.set("HasCollision", SyncValueBox{SyncValue{true}});
    propertyData = propertyData.set("[Type]", SyncValueBox{SyncValue{"SceneObjectType_9"}});
    propertyData = propertyData.set("WorldName", SyncValueBox{SyncValue{"L_CloudMansion_02"}});
    propertyData = propertyData.set("LevelName", SyncValueBox{SyncValue{"L_CloudMansion_Mesh_02"}});
    
    // Primitives array with ModelComponent
    SyncValueVector primitives;
    SyncValueMap modelComp;
    modelComp = modelComp.set("CustomRenderSet", SyncValueBox{SyncValue{static_cast<int64_t>(0)}});
    modelComp = modelComp.set("CustomStencil", SyncValueBox{SyncValue{static_cast<int64_t>(0)}});
    modelComp = modelComp.set("IsCastDynamicShadow", SyncValueBox{SyncValue{true}});
    modelComp = modelComp.set("IsReceiveDynamicShadow", SyncValueBox{SyncValue{true}});
    modelComp = modelComp.set("HasPhysics", SyncValueBox{SyncValue{true}});
    modelComp = modelComp.set("ReceiveDecals", SyncValueBox{SyncValue{true}});
    modelComp = modelComp.set("Lightmap", SyncValueBox{SyncValue{"AuroraAuto/Model_lightmap/L_CloudMansion_02/atlas_0"}});
    modelComp = modelComp.set("[Type]", SyncValueBox{SyncValue{"ModelComponent"}});
    
    // LightmapScale, LightmapOffset (Vec4)
    SyncValueVector lmScale;
    lmScale = lmScale.push_back(SyncValueBox{SyncValue{0.76}});
    lmScale = lmScale.push_back(SyncValueBox{SyncValue{0.71}});
    lmScale = lmScale.push_back(SyncValueBox{SyncValue{0.51}});
    lmScale = lmScale.push_back(SyncValueBox{SyncValue{1.0}});
    modelComp = modelComp.set("LightmapScale", SyncValueBox{SyncValue{lmScale}});
    modelComp = modelComp.set("LightmapOffset", SyncValueBox{SyncValue{lmScale}});
    
    // SyncModel sub-component
    SyncValueMap syncModel;
    syncModel = syncModel.set("GroupID", SyncValueBox{SyncValue{static_cast<int64_t>(0)}});
    syncModel = syncModel.set("NeedBake", SyncValueBox{SyncValue{true}});
    syncModel = syncModel.set("NeedGenLitmap", SyncValueBox{SyncValue{true}});
    syncModel = syncModel.set("NeedCastShadow", SyncValueBox{SyncValue{true}});
    syncModel = syncModel.set("NeedReceiveShadow", SyncValueBox{SyncValue{true}});
    syncModel = syncModel.set("Occluder", SyncValueBox{SyncValue{true}});
    syncModel = syncModel.set("Occludee", SyncValueBox{SyncValue{true}});
    syncModel = syncModel.set("CastGIScale", SyncValueBox{SyncValue{1.0}});
    syncModel = syncModel.set("[Type]", SyncValueBox{SyncValue{"SyncModelComponent"}});
    modelComp = modelComp.set("SyncModel", SyncValueBox{SyncValue{syncModel}});
    
    primitives = primitives.push_back(SyncValueBox{SyncValue{modelComp}});
    propertyData = propertyData.set("Primitives", SyncValueBox{SyncValue{primitives}});
    
    // RigidBodies array
    SyncValueVector rigidBodies;
    SyncValueMap rigidBody;
    rigidBody = rigidBody.set("ComponentType", SyncValueBox{SyncValue{"PhysicsStaticSceneBody"}});
    rigidBody = rigidBody.set("EnableContactNotify", SyncValueBox{SyncValue{false}});
    rigidBody = rigidBody.set("Unwalkable", SyncValueBox{SyncValue{false}});
    rigidBody = rigidBody.set("TemplateRes", SyncValueBox{SyncValue{"Scenes/Architecture/CloudMansion/Structure/AutoPhyRBTemplate"}});
    rigidBody = rigidBody.set("[Type]", SyncValueBox{SyncValue{"PhysicsStaticSceneBody"}});
    rigidBodies = rigidBodies.push_back(SyncValueBox{SyncValue{rigidBody}});
    propertyData = propertyData.set("RigidBodies", SyncValueBox{SyncValue{rigidBodies}});
    
    // Appearance, Tag components
    SyncValueMap appearance;
    appearance = appearance.set("DepthOffset", SyncValueBox{SyncValue{static_cast<int64_t>(0)}});
    appearance = appearance.set("[Type]", SyncValueBox{SyncValue{"IAppearanceComponent"}});
    propertyData = propertyData.set("Appearance", SyncValueBox{SyncValue{appearance}});
    
    SyncValueMap tag;
    tag = tag.set("TagString", SyncValueBox{SyncValue{""}});
    tag = tag.set("[Type]", SyncValueBox{SyncValue{"TagComponent"}});
    propertyData = propertyData.set("Tag", SyncValueBox{SyncValue{tag}});
    
    obj = obj.set("PropertyData", SyncValueBox{SyncValue{propertyData}});
    
    // PropertyPaths array
    SyncValueVector propertyPaths;
    propertyPaths = propertyPaths.push_back(SyncValueBox{SyncValue{"PropertyData"}});
    propertyPaths = propertyPaths.push_back(SyncValueBox{SyncValue{"PropertyData/Primitives/0"}});
    propertyPaths = propertyPaths.push_back(SyncValueBox{SyncValue{"PropertyData/Primitives/0/SyncModel"}});
    propertyPaths = propertyPaths.push_back(SyncValueBox{SyncValue{"PropertyData/RigidBodies/0"}});
    obj = obj.set("PropertyPaths", SyncValueBox{SyncValue{propertyPaths}});
    
    // Components array
    SyncValueVector components;
    SyncValueMap comp1;
    comp1 = comp1.set("DisplayName", SyncValueBox{SyncValue{"[ModelComponent]"}});
    comp1 = comp1.set("Icon", SyncValueBox{SyncValue{"Comp_Model"}});
    components = components.push_back(SyncValueBox{SyncValue{comp1}});
    obj = obj.set("Components", SyncValueBox{SyncValue{components}});
    
    return obj;
}

// Generate large-scale test data using real scene object structure - SyncValue version
SyncValue generate_large_scene_sync(size_t object_count) {
    std::cout << "Generating scene with " << object_count << " objects (SyncValue - thread-safe)...\n";
    std::cout << "Using real scene_object_map.json structure\n";
    
    Timer timer;
    timer.start();
    
    auto objects_transient = SyncValueMap{}.transient();
    
    for (size_t i = 0; i < object_count; ++i) {
        std::string key = generate_uuid_like_id(i);  // UUID-like key
        SyncValueMap obj = create_scene_object_sync(i);
        objects_transient.set(key, SyncValueBox{SyncValue{obj}});
        
        if ((i + 1) % 10000 == 0) {
            std::cout << "  Generated " << (i + 1) << " objects...\n";
        }
    }
    
    SyncValueMap scene;
    scene = scene.set("scene_object_map", SyncValueBox{SyncValue{objects_transient.persistent()}});
    
    double elapsed = timer.elapsed_ms();
    std::cout << "Scene generation completed in " << std::fixed << std::setprecision(2) 
              << elapsed << " ms\n";
    
    return SyncValue{scene};
}

// Generate large-scale test data directly in shared memory - SharedValue version
// This is the truly high-performance approach: data is constructed directly in shared memory
SharedValue generate_large_scene_shared(size_t object_count) {
    std::cout << "Generating scene with " << object_count << " objects (direct SharedValue)...\n";
    
    Timer timer;
    timer.start();
    
    // Note: SharedValue uses no_transience_policy, so transient cannot be used
    // But bump allocator allocation is very fast, so performance is still good
    SharedValueVector objects;
    
    for (size_t i = 0; i < object_count; ++i) {
        SharedValueMap obj;
        obj = std::move(obj).set(shared_memory::SharedString("id"), 
                                  SharedValueBox{SharedValue{static_cast<int64_t>(i)}});
        obj = std::move(obj).set(shared_memory::SharedString("name"), 
                                  SharedValueBox{SharedValue{"Object_" + std::to_string(i)}});
        obj = std::move(obj).set(shared_memory::SharedString("visible"), 
                                  SharedValueBox{SharedValue{true}});
        
        // Transform properties
        SharedValueMap transform;
        transform = std::move(transform).set(shared_memory::SharedString("x"), 
                                              SharedValueBox{SharedValue{static_cast<double>(i % 1000)}});
        transform = std::move(transform).set(shared_memory::SharedString("y"), 
                                              SharedValueBox{SharedValue{static_cast<double>((i / 1000) % 1000)}});
        transform = std::move(transform).set(shared_memory::SharedString("z"), 
                                              SharedValueBox{SharedValue{static_cast<double>(i / 1000000)}});
        transform = std::move(transform).set(shared_memory::SharedString("rotation"), 
                                              SharedValueBox{SharedValue{static_cast<double>(i % 360)}});
        transform = std::move(transform).set(shared_memory::SharedString("scale"), 
                                              SharedValueBox{SharedValue{1.0}});
        obj = std::move(obj).set(shared_memory::SharedString("transform"), 
                                  SharedValueBox{SharedValue{std::move(transform)}});
        
        // Material properties
        SharedValueMap material;
        material = std::move(material).set(shared_memory::SharedString("color"), 
                                            SharedValueBox{SharedValue{"#" + std::to_string(i % 0xFFFFFF)}});
        material = std::move(material).set(shared_memory::SharedString("opacity"), 
                                            SharedValueBox{SharedValue{1.0}});
        material = std::move(material).set(shared_memory::SharedString("roughness"), 
                                            SharedValueBox{SharedValue{0.5}});
        obj = std::move(obj).set(shared_memory::SharedString("material"), 
                                  SharedValueBox{SharedValue{std::move(material)}});
        
        // Tags
        SharedValueVector tags;
        tags = std::move(tags).push_back(SharedValueBox{SharedValue{"tag_" + std::to_string(i % 10)}});
        tags = std::move(tags).push_back(SharedValueBox{SharedValue{"layer_" + std::to_string(i % 5)}});
        obj = std::move(obj).set(shared_memory::SharedString("tags"), 
                                  SharedValueBox{SharedValue{std::move(tags)}});
        
        objects = std::move(objects).push_back(SharedValueBox{SharedValue{std::move(obj)}});
        
        // Progress display
        if ((i + 1) % 10000 == 0) {
            std::cout << "  Generated " << (i + 1) << " objects...\n";
        }
    }
    
    SharedValueMap scene;
    scene = std::move(scene).set(shared_memory::SharedString("version"), 
                                  SharedValueBox{SharedValue{1}});
    scene = std::move(scene).set(shared_memory::SharedString("name"), 
                                  SharedValueBox{SharedValue{"Large Scene"}});
    scene = std::move(scene).set(shared_memory::SharedString("objects"), 
                                  SharedValueBox{SharedValue{std::move(objects)}});
    
    double elapsed = timer.elapsed_ms();
    std::cout << "Scene generation completed in " << std::fixed << std::setprecision(2) 
              << elapsed << " ms\n";
    
    return SharedValue{std::move(scene)};
}

//==============================================================================
// Single Process Simulation Test
//==============================================================================

void demo_single_process() {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "Demo: Single Process Simulation\n";
    std::cout << std::string(60, '=') << "\n\n";
    
    // Generate test data (using Value type)
    constexpr size_t OBJECT_COUNT = 1000;  // 1000 objects for quick test
    Value original = generate_large_scene(OBJECT_COUNT);
    
    std::cout << "\nOriginal Value created.\n";
    std::cout << "Scene objects count: " << original.at("objects").size() << "\n";
    
    // Method 1: Serialization/Deserialization
    std::cout << "\n--- Method 1: Serialization/Deserialization ---\n";
    {
        Timer timer;
        
        timer.start();
        ByteBuffer buffer = serialize(original);
        double serialize_time = timer.elapsed_ms();
        
        timer.start();
        Value deserialized = deserialize(buffer);
        double deserialize_time = timer.elapsed_ms();
        
        std::cout << "Serialized size: " << buffer.size() << " bytes ("
                  << std::fixed << std::setprecision(2) 
                  << (buffer.size() / 1024.0 / 1024.0) << " MB)\n";
        std::cout << "Serialize time: " << serialize_time << " ms\n";
        std::cout << "Deserialize time: " << deserialize_time << " ms\n";
        std::cout << "Total time: " << (serialize_time + deserialize_time) << " ms\n";
        
        // Verify
        if (deserialized == original) {
            std::cout << "Verification: PASSED\n";
        } else {
            std::cout << "Verification: FAILED\n";
        }
    }
    
    // Method 2: Shared memory deep copy
    std::cout << "\n--- Method 2: Shared Memory Deep Copy ---\n";
    {
        Timer timer;
        
        // Simulate process B: create shared memory and write
        timer.start();
        shared_memory::SharedMemoryRegion region;
        if (!region.create("TestSharedValue", 256 * 1024 * 1024)) { // 256MB
            std::cerr << "Failed to create shared memory region\n";
            return;
        }
        
        shared_memory::set_current_shared_region(&region);
        SharedValue shared = deep_copy_to_shared(original);
        shared_memory::set_current_shared_region(nullptr);
        double write_time = timer.elapsed_ms();
        
        std::cout << "Shared memory base: " << region.base() << "\n";
        std::cout << "Shared memory used: " << region.header()->heap_used.load() 
                  << " bytes (" << std::fixed << std::setprecision(2)
                  << (region.header()->heap_used.load() / 1024.0 / 1024.0) << " MB)\n";
        std::cout << "Write to shared memory time: " << write_time << " ms\n";
        
        // Simulate process A: deep copy from shared memory
        timer.start();
        Value copied = deep_copy_to_local(shared);
        double copy_time = timer.elapsed_ms();
        
        std::cout << "Deep copy to local time: " << copy_time << " ms\n";
        std::cout << "Total time: " << (write_time + copy_time) << " ms\n";
        
        // Verify
        if (copied == original) {
            std::cout << "Verification: PASSED\n";
        } else {
            std::cout << "Verification: FAILED\n";
        }
        
        region.close();
    }
}

//==============================================================================
// Cross-Process Test - Publisher (Process B) - High-Performance Version
// Constructs SharedValue directly in shared memory, no intermediate copy
//==============================================================================

void demo_publisher(size_t object_count) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "Demo: Publisher Process (Engine/B Process)\n";
    std::cout << "Using HIGH-PERFORMANCE direct SharedValue construction!\n";
    std::cout << std::string(60, '=') << "\n\n";
    
    Timer timer;
    
    // First create shared memory region
    size_t estimated_size = object_count * 500;  // Estimate ~500 bytes per object
    estimated_size = std::max(estimated_size, size_t(64 * 1024 * 1024));  // At least 64MB
    
    shared_memory::SharedMemoryRegion region;
    if (!region.create("EditorEngineSharedState", estimated_size)) {
        std::cerr << "Failed to create shared memory!\n";
        return;
    }
    
    std::cout << "Shared memory created at: " << region.base() << "\n";
    std::cout << "Shared memory size: " << (estimated_size / 1024.0 / 1024.0) << " MB\n\n";
    
    // Set current shared memory region so all SharedValue allocations go there
    shared_memory::set_current_shared_region(&region);
    
    // Construct scene data directly in shared memory (high-performance approach)
    timer.start();
    SharedValue shared_scene = generate_large_scene_shared(object_count);
    double build_time = timer.elapsed_ms();
    
    // Store SharedValue in shared memory header for subscriber access
    // Note: SharedValue itself is also in shared memory, so we just record its address
    auto* header = region.header();
    
    // Allocate a SharedValue in shared memory to store the scene
    void* value_storage = shared_memory::shared_heap::allocate(sizeof(SharedValue));
    new (value_storage) SharedValue(std::move(shared_scene));
    
    // Record offset to header (use memory_order_release to ensure prior writes are visible)
    header->value_offset.store(
        static_cast<char*>(value_storage) - static_cast<char*>(region.base()),
        std::memory_order_release);
    
    shared_memory::set_current_shared_region(nullptr);
    
    std::cout << "\n--- Performance Stats ---\n";
    std::cout << "Direct build time: " << std::fixed << std::setprecision(2) << build_time << " ms\n";
    std::cout << "Memory used: " << header->heap_used.load() 
              << " bytes (" << (header->heap_used.load() / 1024.0 / 1024.0) << " MB)\n";
    std::cout << "Value stored at offset: " << header->value_offset << "\n";
    
    // Comparison: how long would serialization take?
    std::cout << "\n--- Comparison: What if using serialization? ---\n";
    Value local_scene = generate_large_scene(object_count);
    timer.start();
    ByteBuffer buffer = serialize(local_scene);
    double ser_time = timer.elapsed_ms();
    std::cout << "Serialization would take: " << ser_time << " ms\n";
    std::cout << "Serialized size: " << (buffer.size() / 1024.0 / 1024.0) << " MB\n";
    
    // Wait for subscriber to connect
    std::cout << "\nPublisher ready. Run another instance with 'subscribe' to test.\n";
    std::cout << "Press Enter to exit...\n";
    std::cin.get();
    
    region.close();
    std::cout << "Publisher exited.\n";
}

//==============================================================================
// Cross-Process Test - Subscriber (Process A)
//==============================================================================

void demo_subscriber() {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "Demo: Subscriber Process (Editor/A Process)\n";
    std::cout << std::string(60, '=') << "\n\n";
    
    // Use SharedValueHandle to open shared memory
    SharedValueHandle handle;
    
    std::cout << "Trying to open shared memory...\n";
    
    if (!handle.open("EditorEngineSharedState")) {
        std::cerr << "Failed to open shared memory!\n";
        std::cerr << "Make sure the publisher is running first.\n";
        return;
    }
    
    std::cout << "Shared memory opened at: " << handle.region().base() << "\n";
    std::cout << "Shared memory size: " << handle.region().size() << " bytes\n";
    std::cout << "Memory used: " << handle.region().header()->heap_used.load() << " bytes\n";
    
    // Verify address match
    if (handle.region().base() != handle.region().header()->fixed_base_address) {
        std::cerr << "ERROR: Address mismatch!\n";
        std::cerr << "Expected: " << handle.region().header()->fixed_base_address << "\n";
        std::cerr << "Got: " << handle.region().base() << "\n";
        std::cerr << "This would cause pointer issues. Cannot proceed with zero-copy.\n";
        return;
    }
    
    std::cout << "Address verification: PASSED\n\n";
    
    // Check if SharedValue is ready
    if (!handle.is_value_ready()) {
        std::cerr << "SharedValue not ready in shared memory!\n";
        return;
    }
    
    // Get shared Value (zero-copy read-only access)
    const SharedValue* shared = handle.shared_value();
    if (!shared) {
        std::cerr << "Failed to get SharedValue pointer!\n";
        return;
    }
    
    std::cout << "SharedValue found in shared memory.\n";
    
    // Measure deep copy performance
    Timer timer;
    timer.start();
    Value local = handle.copy_to_local();
    double copy_time = timer.elapsed_ms();
    
    std::cout << "Deep copy to local completed in " << std::fixed << std::setprecision(2) 
              << copy_time << " ms\n";
    
    // Display data summary
    std::cout << "\n--- Data Summary ---\n";
    if (auto* map = local.get_if<ValueMap>()) {
        if (auto it = map->find("name"); it) {
            if (auto* name = (*it)->get_if<std::string>()) {
                std::cout << "Scene name: " << *name << "\n";
            }
        }
        if (auto it = map->find("version"); it) {
            if (auto* ver = (*it)->get_if<int>()) {
                std::cout << "Version: " << *ver << "\n";
            }
        }
        if (auto it = map->find("objects"); it) {
            std::cout << "Objects count: " << (*it)->size() << "\n";
        }
    }
    
    std::cout << "\nSubscriber connected and data copied successfully.\n";
    std::cout << "Press Enter to exit...\n";
    std::cin.get();
    
    std::cout << "Subscriber exited.\n";
}

//==============================================================================
// Helper function: traverse SharedValue (simulating read-only access)
//==============================================================================

size_t traverse_shared_value(const SharedValue& sv) {
    size_t count = 1;
    
    if (auto* map = sv.get_if<SharedValueMap>()) {
        for (const auto& [key, box] : *map) {
            count += traverse_shared_value(box.get());
        }
    }
    else if (auto* vec = sv.get_if<SharedValueVector>()) {
        for (const auto& box : *vec) {
            count += traverse_shared_value(box.get());
        }
    }
    else if (auto* arr = sv.get_if<SharedValueArray>()) {
        for (const auto& box : *arr) {
            count += traverse_shared_value(box.get());
        }
    }
    
    return count;
}

size_t traverse_value(const Value& v) {
    size_t count = 1;
    
    if (auto* map = v.get_if<ValueMap>()) {
        for (const auto& [key, box] : *map) {
            count += traverse_value(box.get());
        }
    }
    else if (auto* vec = v.get_if<ValueVector>()) {
        for (const auto& box : *vec) {
            count += traverse_value(box.get());
        }
    }
    else if (auto* arr = v.get_if<ValueArray>()) {
        for (const auto& box : *arr) {
            count += traverse_value(box.get());
        }
    }
    
    return count;
}

//==============================================================================
// Performance Comparison Test (4 Methods)
//==============================================================================

void performance_comparison() {
    constexpr size_t OBJECT_COUNT = 50000;  // 50,000 objects
    
    std::cout << "\n" << std::string(100, '=') << "\n";
    std::cout << "Performance Comparison: Four Methods (" << OBJECT_COUNT << " objects)\n";
    std::cout << std::string(100, '=') << "\n\n";
    
    std::cout << "Methods compared:\n";
    std::cout << "  1. Binary Serialization: Value -> serialize -> deserialize -> Value (custom binary)\n";
    std::cout << "  2. SharedMem (2-copy): Value -> deep_copy_to_shared -> deep_copy_to_local\n";
    std::cout << "  3. SharedMem (1-copy): SharedValue (direct) -> deep_copy_to_local\n";
    std::cout << "  4. SharedMem (ZERO-COPY): SharedValue (direct) -> direct read (no copy!)\n";
    std::cout << "\n";
    
    Timer timer;
    double serialize_time, deserialize_time;
    double deep_copy_to_shared_time, deep_copy_to_local_time_m2;
    double direct_build_time, deep_copy_to_local_time_m3;
    size_t serialized_size = 0;
    size_t shared_memory_used_m2 = 0;
    size_t shared_memory_used_m3 = 0;
    
    //==========================================================================
    // Method 1: Serialization/Deserialization
    //==========================================================================
    std::cout << "=== Method 1: Serialization ===\n";
    {
        // Generate local Value
        Value data = generate_large_scene(OBJECT_COUNT);
        
        // Serialize
        timer.start();
        ByteBuffer buffer = serialize(data);
        serialize_time = timer.elapsed_ms();
        serialized_size = buffer.size();
        
        // Deserialize
        timer.start();
        Value deser = deserialize(buffer);
        deserialize_time = timer.elapsed_ms();
        
        std::cout << "  Serialize:   " << std::fixed << std::setprecision(2) << serialize_time << " ms\n";
        std::cout << "  Deserialize: " << deserialize_time << " ms\n";
        std::cout << "  Total:       " << (serialize_time + deserialize_time) << " ms\n";
        std::cout << "  Data size:   " << (serialized_size / 1024.0 / 1024.0) << " MB\n\n";
    }
    
    //==========================================================================
    // Method 2: Shared Memory (2-copy: local -> shared -> local)
    //==========================================================================
    std::cout << "=== Method 2: SharedMem (2-copy) ===\n";
    {
        // Generate local Value
        Value data = generate_large_scene(OBJECT_COUNT);
        
        // Create shared memory
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTest2", 1024 * 1024 * 1024)) {  // 1GB
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        
        shared_memory::set_current_shared_region(&region);
        
        // Deep copy to shared memory
        timer.start();
        SharedValue shared = deep_copy_to_shared(data);
        deep_copy_to_shared_time = timer.elapsed_ms();
        
        // Deep copy back to local
        timer.start();
        Value local = deep_copy_to_local(shared);
        deep_copy_to_local_time_m2 = timer.elapsed_ms();
        
        shared_memory_used_m2 = region.header()->heap_used.load();
        
        shared_memory::set_current_shared_region(nullptr);
        region.close();
        
        std::cout << "  Copy to shared:   " << std::fixed << std::setprecision(2) << deep_copy_to_shared_time << " ms\n";
        std::cout << "  Copy to local:    " << deep_copy_to_local_time_m2 << " ms\n";
        std::cout << "  Total:            " << (deep_copy_to_shared_time + deep_copy_to_local_time_m2) << " ms\n";
        std::cout << "  Shared mem used:  " << (shared_memory_used_m2 / 1024.0 / 1024.0) << " MB\n\n";
    }
    
    //==========================================================================
    // Method 3: Shared Memory (1-copy: construct directly in shared memory -> copy to local)
    //==========================================================================
    std::cout << "=== Method 3: SharedMem (Direct Build - 1-copy) ===\n";
    {
        // Create shared memory
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTest3", 1024 * 1024 * 1024)) {  // 1GB
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        
        shared_memory::set_current_shared_region(&region);
        
        // Construct directly in shared memory
        timer.start();
        SharedValue shared_direct = generate_large_scene_shared(OBJECT_COUNT);
        direct_build_time = timer.elapsed_ms();
        
        // Deep copy back to local (the only operation Editor process needs to do)
        timer.start();
        Value local = deep_copy_to_local(shared_direct);
        deep_copy_to_local_time_m3 = timer.elapsed_ms();
        
        shared_memory_used_m3 = region.header()->heap_used.load();
        
        shared_memory::set_current_shared_region(nullptr);
        region.close();
        
        std::cout << "  Direct build:     " << std::fixed << std::setprecision(2) << direct_build_time << " ms\n";
        std::cout << "  Copy to local:    " << deep_copy_to_local_time_m3 << " ms\n";
        std::cout << "  Total:            " << (direct_build_time + deep_copy_to_local_time_m3) << " ms\n";
        std::cout << "  Shared mem used:  " << (shared_memory_used_m3 / 1024.0 / 1024.0) << " MB\n\n";
    }
    
    //==========================================================================
    // Method 4: Shared Memory (ZERO-COPY: direct read, no copy!)
    // This is true zero-copy - Editor directly reads data in shared memory
    //==========================================================================
    std::cout << "=== Method 4: SharedMem (TRUE ZERO-COPY - Direct Read) ===\n";
    double direct_read_time = 0;
    size_t node_count = 0;
    {
        // Create shared memory
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTest4", 1024 * 1024 * 1024)) {  // 1GB
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        
        shared_memory::set_current_shared_region(&region);
        
        // Construct directly in shared memory (Engine side)
        SharedValue shared_direct = generate_large_scene_shared(OBJECT_COUNT);
        
        // ZERO-COPY: Editor directly traverses and reads data in shared memory, no copy at all!
        // This simulates Editor read-only access to the scene (e.g., displaying properties in UI)
        timer.start();
        node_count = traverse_shared_value(shared_direct);
        direct_read_time = timer.elapsed_ms();
        
        shared_memory::set_current_shared_region(nullptr);
        region.close();
        
        std::cout << "  Direct read (no copy!): " << std::fixed << std::setprecision(2) << direct_read_time << " ms\n";
        std::cout << "  Nodes traversed:        " << node_count << "\n\n";
    }
    
    //==========================================================================
    // Results Summary
    //==========================================================================
    // Time from Editor process perspective (not including Engine data construction time)
    double editor_m1 = deserialize_time;           // Editor only needs to deserialize
    double editor_m2 = deep_copy_to_local_time_m2; // Editor only needs deep_copy_to_local
    double editor_m3 = deep_copy_to_local_time_m3; // Editor only needs deep_copy_to_local
    double editor_m4 = direct_read_time;           // Editor reads directly, zero-copy!
    
    std::cout << std::string(100, '=') << "\n";
    std::cout << "SUMMARY (" << OBJECT_COUNT << " objects)\n";
    std::cout << std::string(100, '-') << "\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "                    | Method 1     | Method 2     | Method 3     | Method 4     \n";
    std::cout << "                    | (CustomBin)  | (2-copy)     | (1-copy)     | (ZERO-COPY)  \n";
    std::cout << std::string(100, '-') << "\n";
    std::cout << "Engine side time    | " << std::setw(10) << serialize_time << " | " 
              << std::setw(10) << deep_copy_to_shared_time << " | " 
              << std::setw(10) << direct_build_time << " | " 
              << std::setw(10) << direct_build_time << " ms\n";
    std::cout << "Editor side time    | " << std::setw(10) << editor_m1 << " | " 
              << std::setw(10) << editor_m2 << " | " 
              << std::setw(10) << editor_m3 << " | " 
              << std::setw(10) << editor_m4 << " ms\n";
    std::cout << "Data size (MB)      | " << std::setw(10) << (serialized_size / 1024.0 / 1024.0) << " | " 
              << std::setw(10) << (shared_memory_used_m2 / 1024.0 / 1024.0) << " | " 
              << std::setw(10) << (shared_memory_used_m3 / 1024.0 / 1024.0) << " | " 
              << std::setw(10) << (shared_memory_used_m3 / 1024.0 / 1024.0) << " MB\n";
    std::cout << std::string(100, '-') << "\n";
    std::cout << "Engine speedup vs M3| " << std::setw(10) << (direct_build_time / serialize_time) << "x | " 
              << std::setw(10) << (direct_build_time / deep_copy_to_shared_time) << "x | " 
              << std::setw(10) << "1.00x" << " | "
              << std::setw(10) << "1.00x\n";
    std::cout << "Editor speedup vs M1| " << std::setw(10) << "1.00x" << " | " 
              << std::setw(10) << (editor_m1 / editor_m2) << "x | " 
              << std::setw(10) << (editor_m1 / editor_m3) << "x | "
              << std::setw(10) << (editor_m1 / editor_m4) << "x\n";
    std::cout << std::string(100, '=') << "\n\n";
    
    std::cout << "Conclusion:\n";
    std::cout << "  - Method 4 (TRUE ZERO-COPY) is the FASTEST for read-only access!\n";
    std::cout << "    Editor directly reads SharedValue in shared memory - NO COPY at all.\n";
    std::cout << "    Speedup vs custom binary deserialization: " << (editor_m1 / editor_m4) << "x faster!\n\n";
    
    std::cout << "  - Method 3 (Direct SharedValue) is best for editable local copy.\n";
    std::cout << "    Engine constructs directly in shared memory, Editor copies once.\n\n";
    
    std::cout << "  - Method 1 (CustomBin) has smallest data size.\n";
    std::cout << "    Good option when shared memory is not available.\n\n";
    
    std::cout << "Recommendations:\n";
    std::cout << "  - For READ-ONLY access: Use Method 4 (ZERO-COPY)\n";
    std::cout << "  - For EDITABLE local copy: Use Method 3 (1-copy) or Method 1 (CustomBin)\n";
    std::cout << "  - Use Method 4 for: UI display, property inspection, read-only queries.\n";
    std::cout << "  - Use Method 3 for: Undo/redo, local modifications, state management.\n";
}

//==============================================================================
// Value vs SyncValue Performance Comparison
// Compares single-threaded (non-atomic refcount) vs thread-safe (atomic refcount)
//==============================================================================

size_t traverse_sync_value(const SyncValue& v) {
    size_t count = 1;
    
    if (auto* map = v.get_if<SyncValueMap>()) {
        for (const auto& [key, box] : *map) {
            count += traverse_sync_value(box.get());
        }
    }
    else if (auto* vec = v.get_if<SyncValueVector>()) {
        for (const auto& box : *vec) {
            count += traverse_sync_value(box.get());
        }
    }
    else if (auto* arr = v.get_if<SyncValueArray>()) {
        for (const auto& box : *arr) {
            count += traverse_sync_value(box.get());
        }
    }
    
    return count;
}

void value_vs_sync_comparison() {
    constexpr size_t OBJECT_COUNT = 50000;  // 50,000 objects for fair comparison
    
    std::cout << "\n" << std::string(100, '=') << "\n";
    std::cout << "Value vs SyncValue Performance Comparison (" << OBJECT_COUNT << " objects)\n";
    std::cout << std::string(100, '=') << "\n\n";
    
    std::cout << "This test compares:\n";
    std::cout << "  - Value (UnsafeValue):     Non-atomic refcount, no locks, highest performance\n";
    std::cout << "  - SyncValue (ThreadSafeValue): Atomic refcount + spinlock, thread-safe\n";
    std::cout << "\n";
    
    Timer timer;
    
    //==========================================================================
    // Phase 1: Construction Performance
    //==========================================================================
    std::cout << "=== Phase 1: Construction Performance ===\n\n";
    
    double value_construct_time = 0;
    double sync_construct_time = 0;
    
    // Value construction
    {
        timer.start();
        Value scene = generate_large_scene(OBJECT_COUNT);
        value_construct_time = timer.elapsed_ms();
    }
    
    // SyncValue construction
    {
        timer.start();
        SyncValue scene = generate_large_scene_sync(OBJECT_COUNT);
        sync_construct_time = timer.elapsed_ms();
    }
    
    std::cout << "\n--- Construction Results ---\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Value construction:     " << value_construct_time << " ms\n";
    std::cout << "  SyncValue construction: " << sync_construct_time << " ms\n";
    std::cout << "  Overhead: " << ((sync_construct_time / value_construct_time) - 1.0) * 100.0 << "%\n";
    std::cout << "  Value is " << (sync_construct_time / value_construct_time) << "x faster\n\n";
    
    //==========================================================================
    // Phase 2: Copy/Clone Performance (triggers refcount operations)
    //==========================================================================
    std::cout << "=== Phase 2: Copy/Clone Performance (refcount stress test) ===\n\n";
    
    constexpr size_t COPY_COUNT = 1000;
    double value_copy_time = 0;
    double sync_copy_time = 0;
    
    // Value copy test
    {
        Value scene = generate_large_scene(OBJECT_COUNT);
        std::vector<Value> copies;
        copies.reserve(COPY_COUNT);
        
        timer.start();
        for (size_t i = 0; i < COPY_COUNT; ++i) {
            copies.push_back(scene);  // Shallow copy, increments refcount
        }
        value_copy_time = timer.elapsed_ms();
        
        std::cout << "  Value: " << COPY_COUNT << " shallow copies completed in " << value_copy_time << " ms\n";
    }
    
    // SyncValue copy test
    {
        SyncValue scene = generate_large_scene_sync(OBJECT_COUNT);
        std::vector<SyncValue> copies;
        copies.reserve(COPY_COUNT);
        
        timer.start();
        for (size_t i = 0; i < COPY_COUNT; ++i) {
            copies.push_back(scene);  // Shallow copy, atomic refcount increment
        }
        sync_copy_time = timer.elapsed_ms();
        
        std::cout << "  SyncValue: " << COPY_COUNT << " shallow copies completed in " << sync_copy_time << " ms\n";
    }
    
    std::cout << "\n--- Copy Results ---\n";
    std::cout << "  Value copy time:     " << value_copy_time << " ms\n";
    std::cout << "  SyncValue copy time: " << sync_copy_time << " ms\n";
    std::cout << "  Overhead: " << ((sync_copy_time / value_copy_time) - 1.0) * 100.0 << "%\n";
    std::cout << "  Value is " << (sync_copy_time / value_copy_time) << "x faster\n\n";
    
    //==========================================================================
    // Phase 3: Traversal Performance
    //==========================================================================
    std::cout << "=== Phase 3: Traversal Performance ===\n\n";
    
    double value_traverse_time = 0;
    double sync_traverse_time = 0;
    size_t value_node_count = 0;
    size_t sync_node_count = 0;
    
    // Value traversal
    {
        Value scene = generate_large_scene(OBJECT_COUNT);
        
        timer.start();
        value_node_count = traverse_value(scene);
        value_traverse_time = timer.elapsed_ms();
        
        std::cout << "  Value: Traversed " << value_node_count << " nodes in " << value_traverse_time << " ms\n";
    }
    
    // SyncValue traversal
    {
        SyncValue scene = generate_large_scene_sync(OBJECT_COUNT);
        
        timer.start();
        sync_node_count = traverse_sync_value(scene);
        sync_traverse_time = timer.elapsed_ms();
        
        std::cout << "  SyncValue: Traversed " << sync_node_count << " nodes in " << sync_traverse_time << " ms\n";
    }
    
    std::cout << "\n--- Traversal Results ---\n";
    std::cout << "  Value traversal time:     " << value_traverse_time << " ms\n";
    std::cout << "  SyncValue traversal time: " << sync_traverse_time << " ms\n";
    std::cout << "  Overhead: " << ((sync_traverse_time / value_traverse_time) - 1.0) * 100.0 << "%\n";
    std::cout << "  Value is " << (sync_traverse_time / value_traverse_time) << "x faster\n\n";
    
    //==========================================================================
    // Phase 4: Modification Performance (structural changes)
    //==========================================================================
    std::cout << "=== Phase 4: Modification Performance (set operations) ===\n\n";
    
    constexpr size_t MODIFY_COUNT = 10000;
    double value_modify_time = 0;
    double sync_modify_time = 0;
    
    // Value modification
    {
        Value scene = generate_large_scene(1000);  // Smaller scene for modification test
        
        timer.start();
        for (size_t i = 0; i < MODIFY_COUNT; ++i) {
            // Access and modify a property - creates new nodes, triggers refcount ops
            if (auto* map = scene.get_if<ValueMap>()) {
                ValueMap newMap = *map;
                newMap = newMap.set("counter", ValueBox{Value{static_cast<int64_t>(i)}});
                scene = Value{newMap};
            }
        }
        value_modify_time = timer.elapsed_ms();
        
        std::cout << "  Value: " << MODIFY_COUNT << " modifications completed in " << value_modify_time << " ms\n";
    }
    
    // SyncValue modification
    {
        SyncValue scene = generate_large_scene_sync(1000);  // Smaller scene for modification test
        
        timer.start();
        for (size_t i = 0; i < MODIFY_COUNT; ++i) {
            if (auto* map = scene.get_if<SyncValueMap>()) {
                SyncValueMap newMap = *map;
                newMap = newMap.set("counter", SyncValueBox{SyncValue{static_cast<int64_t>(i)}});
                scene = SyncValue{newMap};
            }
        }
        sync_modify_time = timer.elapsed_ms();
        
        std::cout << "  SyncValue: " << MODIFY_COUNT << " modifications completed in " << sync_modify_time << " ms\n";
    }
    
    std::cout << "\n--- Modification Results ---\n";
    std::cout << "  Value modification time:     " << value_modify_time << " ms\n";
    std::cout << "  SyncValue modification time: " << sync_modify_time << " ms\n";
    std::cout << "  Overhead: " << ((sync_modify_time / value_modify_time) - 1.0) * 100.0 << "%\n";
    std::cout << "  Value is " << (sync_modify_time / value_modify_time) << "x faster\n\n";
    
    //==========================================================================
    // Summary
    //==========================================================================
    std::cout << std::string(100, '=') << "\n";
    std::cout << "SUMMARY: Value vs SyncValue Performance\n";
    std::cout << std::string(100, '-') << "\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "                    | Value        | SyncValue    | Overhead     | Value Speedup\n";
    std::cout << std::string(100, '-') << "\n";
    std::cout << "Construction (ms)   | " << std::setw(12) << value_construct_time 
              << " | " << std::setw(12) << sync_construct_time 
              << " | " << std::setw(10) << ((sync_construct_time / value_construct_time) - 1.0) * 100.0 << "%" 
              << " | " << std::setw(10) << (sync_construct_time / value_construct_time) << "x\n";
    std::cout << "Copy " << COPY_COUNT << "x (ms)     | " << std::setw(12) << value_copy_time 
              << " | " << std::setw(12) << sync_copy_time 
              << " | " << std::setw(10) << ((sync_copy_time / value_copy_time) - 1.0) * 100.0 << "%" 
              << " | " << std::setw(10) << (sync_copy_time / value_copy_time) << "x\n";
    std::cout << "Traversal (ms)      | " << std::setw(12) << value_traverse_time 
              << " | " << std::setw(12) << sync_traverse_time 
              << " | " << std::setw(10) << ((sync_traverse_time / value_traverse_time) - 1.0) * 100.0 << "%" 
              << " | " << std::setw(10) << (sync_traverse_time / value_traverse_time) << "x\n";
    std::cout << "Modification (ms)   | " << std::setw(12) << value_modify_time 
              << " | " << std::setw(12) << sync_modify_time 
              << " | " << std::setw(10) << ((sync_modify_time / value_modify_time) - 1.0) * 100.0 << "%" 
              << " | " << std::setw(10) << (sync_modify_time / value_modify_time) << "x\n";
    std::cout << std::string(100, '=') << "\n\n";
    
    std::cout << "Conclusion:\n";
    std::cout << "  - Value (UnsafeValue) is faster for all operations due to non-atomic refcount\n";
    std::cout << "  - The overhead of SyncValue comes from atomic operations (memory barriers)\n";
    std::cout << "  - Use Value (default) for single-threaded scenarios\n";
    std::cout << "  - Use SyncValue only when data is shared between threads\n";
    std::cout << "\n";
    
    std::cout << "Recommendations:\n";
    std::cout << "  - Single-threaded applications: Use Value (default)\n";
    std::cout << "  - Multi-threaded with thread-local data: Use Value per thread\n";
    std::cout << "  - Multi-threaded with shared data: Use SyncValue\n";
    std::cout << "  - Cross-process communication: Use SharedValue\n";
}

//==============================================================================
// Main Function
//==============================================================================

void print_usage() {
    std::cout << "Usage: shared_value_demo [command]\n";
    std::cout << "\nCommands:\n";
    std::cout << "  single       - Single process demo (default)\n";
    std::cout << "  publish N    - Run as publisher with N objects\n";
    std::cout << "  subscribe    - Run as subscriber\n";
    std::cout << "  perf         - Performance comparison (4 methods)\n";
    std::cout << "  value_sync   - Value vs SyncValue performance comparison\n";
    std::cout << "\nExamples:\n";
    std::cout << "  shared_value_demo single\n";
    std::cout << "  shared_value_demo publish 10000\n";
    std::cout << "  shared_value_demo subscribe\n";
    std::cout << "  shared_value_demo value_sync\n";
}

int main(int argc, char* argv[]) {
    std::cout << "SharedValue Demo - Cross-Process Zero-Copy Transfer\n";
    std::cout << std::string(60, '=') << "\n";
    
    std::string command = "single";
    size_t object_count = 1000;
    
    if (argc > 1) {
        command = argv[1];
    }
    if (argc > 2) {
        object_count = std::stoul(argv[2]);
    }
    
    if (command == "single") {
        demo_single_process();
    }
    else if (command == "publish") {
        demo_publisher(object_count);
    }
    else if (command == "subscribe") {
        demo_subscriber();
    }
    else if (command == "perf") {
        performance_comparison();
    }
    else if (command == "value_sync") {
        value_vs_sync_comparison();
    }
    else {
        print_usage();
        return 1;
    }
    
    return 0;
}
