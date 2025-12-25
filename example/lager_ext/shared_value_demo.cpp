// shared_value_demo.cpp
// Demonstrates SharedValue cross-process zero-copy transfer
//
// This demo shows:
// 1. How process B creates shared memory and writes Value
// 2. How process A opens shared memory and deep copies to local
// 3. Performance comparison: shared memory vs serialization

// Must be defined before Windows.h to prevent min/max macro conflicts
#define NOMINMAX

#include <lager_ext/shared_value.h>
#include <lager_ext/fast_shared_value.h>
#include <lager_ext/value.h>

#include <chrono>
#include <iostream>
#include <iomanip>
#include <thread>
#include <sstream>

using namespace lager_ext;

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
//
// OPTIMIZED: Uses Builder API for O(n) construction instead of O(n log n)
// This demonstrates the recommended pattern for building complex Value structures.
Value create_scene_object(size_t index) {
    std::string id = generate_uuid_like_id(index);

    // Build techParam (Vec3) - reused in multiple places
    Value techParam = VectorBuilder()
        .push_back(0.0)
        .push_back(0.0)
        .push_back(0.0)
        .finish();

    // Build techParam2 (Vec4)
    Value techParam2 = VectorBuilder()
        .push_back(0.0)
        .push_back(0.0)
        .push_back(0.0)
        .push_back(0.0)
        .finish();

    // Build tintColor (Vec4) - all 1.0
    Value tintColor = VectorBuilder()
        .push_back(1.0)
        .push_back(1.0)
        .push_back(1.0)
        .push_back(1.0)
        .finish();

    // Build LightmapScale/Offset (Vec4)
    Value lmScale = VectorBuilder()
        .push_back(0.76)
        .push_back(0.71)
        .push_back(0.51)
        .push_back(1.0)
        .finish();

    // Build SyncModel sub-component
    Value syncModel = MapBuilder()
        .set("GroupID", static_cast<int64_t>(0))
        .set("NeedBake", true)
        .set("NeedGenLitmap", true)
        .set("NeedCastShadow", true)
        .set("NeedReceiveShadow", true)
        .set("Occluder", true)
        .set("Occludee", true)
        .set("CastGIScale", 1.0)
        .set("[Type]", "SyncModelComponent")
        .finish();

    // Build ModelComponent
    Value modelComp = MapBuilder()
        .set("CustomRenderSet", static_cast<int64_t>(0))
        .set("CustomStencil", static_cast<int64_t>(0))
        .set("IsCastDynamicShadow", true)
        .set("IsReceiveDynamicShadow", true)
        .set("HasPhysics", true)
        .set("ReceiveDecals", true)
        .set("Lightmap", "AuroraAuto/Model_lightmap/L_CloudMansion_02/atlas_0")
        .set("[Type]", "ModelComponent")
        .set("LightmapScale", lmScale)
        .set("LightmapOffset", lmScale)
        .set("SyncModel", syncModel)
        .finish();

    // Build Primitives array
    Value primitives = VectorBuilder()
        .push_back(modelComp)
        .finish();

    // Build RigidBody
    Value rigidBody = MapBuilder()
        .set("ComponentType", "PhysicsStaticSceneBody")
        .set("EnableContactNotify", false)
        .set("Unwalkable", false)
        .set("TemplateRes", "Scenes/Architecture/CloudMansion/Structure/AutoPhyRBTemplate")
        .set("[Type]", "PhysicsStaticSceneBody")
        .finish();

    // Build RigidBodies array
    Value rigidBodies = VectorBuilder()
        .push_back(rigidBody)
        .finish();

    // Build Appearance component
    Value appearance = MapBuilder()
        .set("DepthOffset", static_cast<int64_t>(0))
        .set("[Type]", "IAppearanceComponent")
        .finish();

    // Build Tag component
    Value tag = MapBuilder()
        .set("TagString", "")
        .set("[Type]", "TagComponent")
        .finish();

    // Build PropertyData - complex nested structure
    Value propertyData = MapBuilder()
        .set("GenerateOccluder", false)
        .set("DeleteOccluder", false)
        .set("IsVisible", true)
        .set("IsDisableCollision", false)
        .set("IsBillboard", false)
        .set("IsReflectionVisible", false)
        .set("IsOutlined", false)
        .set("IsThermalVisible", false)
        .set("DetailLevel", static_cast<int64_t>(0))
        .set("TechState", static_cast<int64_t>(0))
        .set("TechParam", techParam)
        .set("TechParam2", techParam2)
        .set("TintColor1", tintColor)
        .set("TintColor2", tintColor)
        .set("TintColor3", tintColor)
        .set("LodThreshold", techParam)
        .set("Anchor", techParam)
        .set("IsCastDynamicShadow", true)
        .set("IsReceiveDynamicShadow", true)
        .set("IsSDFGen", true)
        .set("HasCollision", true)
        .set("[Type]", "SceneObjectType_9")
        .set("WorldName", "L_CloudMansion_02")
        .set("LevelName", "L_CloudMansion_Mesh_02")
        .set("Primitives", primitives)
        .set("RigidBodies", rigidBodies)
        .set("Appearance", appearance)
        .set("Tag", tag)
        .finish();

    // Build PropertyPaths array
    Value propertyPaths = VectorBuilder()
        .push_back("PropertyData")
        .push_back("PropertyData/Primitives/0")
        .push_back("PropertyData/Primitives/0/SyncModel")
        .push_back("PropertyData/RigidBodies/0")
        .finish();

    // Build Components array
    Value components = VectorBuilder()
        .push_back(MapBuilder()
            .set("DisplayName", "[ModelComponent]")
            .set("Icon", "Comp_Model")
            .finish())
        .finish();

    // Build position (Vec3)
    Value position = VectorBuilder()
        .push_back(static_cast<double>(index % 1000))
        .push_back(0.06)
        .push_back(static_cast<double>((index / 1000) % 1000))
        .finish();

    // Build scale (Vec3)
    Value scale = VectorBuilder()
        .push_back(1.0)
        .push_back(1.0)
        .push_back(1.0)
        .finish();

    // Build euler (Vec3)
    Value euler = VectorBuilder()
        .push_back(0.0)
        .push_back(static_cast<double>(index % 360))
        .push_back(0.0)
        .finish();

    // Build property sub-object
    Value property = MapBuilder()
        .set("name", "IEntity")
        .finish();

    // Build the final scene object using Builder API - O(n) construction!
    return MapBuilder()
        .set("property", property)
        .set("filename", "")
        .set("space_object_type", static_cast<int64_t>(1048576))
        .set("scene_object_id", id)
        .set("parent", "A7DC0D1A-7B421DB0-5B8D7D86-FDB2A65F")
        .set("level", "CB9552E0-F1495927-71830CA6-BE6E082F")
        .set("position", position)
        .set("scale", scale)
        .set("euler", euler)
        .set("visible_mask", true)
        .set("in_world", true)
        .set("scene_object_layer", static_cast<int64_t>(143))
        .set("name", "SM_CM_1L_Building_" + std::to_string(index))
        .set("file", "Scenes/Architecture/CloudMansion/Structure/SM_CM_L1_Building_" + std::to_string(index % 100))
        .set("scene_object_locked", false)
        .set("scene_object_type", static_cast<int64_t>(9))
        .set("ModelResource", "Scenes/Architecture/CloudMansion/Structure/SM_CM_L1_Building_" + std::to_string(index % 100))
        .set("PropertyData", propertyData)
        .set("PropertyPaths", propertyPaths)
        .set("Components", components)
        .finish();
}

// Generate large-scale test data using real scene object structure - Value version
//
// OPTIMIZED: Uses Builder API for O(n) construction instead of O(n log n)
Value generate_large_scene(size_t object_count) {
    std::cout << "Generating scene with " << object_count << " objects (Value - Builder API)...\n";
    std::cout << "Using real scene_object_map.json structure with O(n) construction\n";

    Timer timer;
    timer.start();

    // Use MapBuilder for O(n) construction - each .set() is O(1) amortized
    MapBuilder objects_builder;

    for (size_t i = 0; i < object_count; ++i) {
        std::string key = generate_uuid_like_id(i);  // UUID-like key
        Value obj = create_scene_object(i);          // Now returns Value
        objects_builder.set(key, obj);

        if ((i + 1) % 10000 == 0) {
            std::cout << "  Generated " << (i + 1) << " objects...\n";
        }
    }

    double elapsed = timer.elapsed_ms();
    std::cout << "Scene generation completed in " << std::fixed << std::setprecision(2)
              << elapsed << " ms\n";

    // Build the final scene using Builder API
    return MapBuilder()
        .set("scene_object_map", objects_builder.finish())
        .finish();
}

// Helper: create a single realistic scene object (SyncValue version)
//
// OPTIMIZED: Uses SyncMapBuilder/SyncVectorBuilder for O(n) construction
SyncValue create_scene_object_sync(size_t index) {
    std::string id = generate_uuid_like_id(index);

    // Build techParam (Vec3) - reused in multiple places
    SyncValue techParam = SyncVectorBuilder()
        .push_back(0.0)
        .push_back(0.0)
        .push_back(0.0)
        .finish();

    // Build techParam2 (Vec4)
    SyncValue techParam2 = SyncVectorBuilder()
        .push_back(0.0)
        .push_back(0.0)
        .push_back(0.0)
        .push_back(0.0)
        .finish();

    // Build tintColor (Vec4) - all 1.0
    SyncValue tintColor = SyncVectorBuilder()
        .push_back(1.0)
        .push_back(1.0)
        .push_back(1.0)
        .push_back(1.0)
        .finish();

    // Build LightmapScale/Offset (Vec4)
    SyncValue lmScale = SyncVectorBuilder()
        .push_back(0.76)
        .push_back(0.71)
        .push_back(0.51)
        .push_back(1.0)
        .finish();

    // Build SyncModel sub-component
    SyncValue syncModel = SyncMapBuilder()
        .set("GroupID", static_cast<int64_t>(0))
        .set("NeedBake", true)
        .set("NeedGenLitmap", true)
        .set("NeedCastShadow", true)
        .set("NeedReceiveShadow", true)
        .set("Occluder", true)
        .set("Occludee", true)
        .set("CastGIScale", 1.0)
        .set("[Type]", "SyncModelComponent")
        .finish();

    // Build ModelComponent
    SyncValue modelComp = SyncMapBuilder()
        .set("CustomRenderSet", static_cast<int64_t>(0))
        .set("CustomStencil", static_cast<int64_t>(0))
        .set("IsCastDynamicShadow", true)
        .set("IsReceiveDynamicShadow", true)
        .set("HasPhysics", true)
        .set("ReceiveDecals", true)
        .set("Lightmap", "AuroraAuto/Model_lightmap/L_CloudMansion_02/atlas_0")
        .set("[Type]", "ModelComponent")
        .set("LightmapScale", lmScale)
        .set("LightmapOffset", lmScale)
        .set("SyncModel", syncModel)
        .finish();

    // Build Primitives array
    SyncValue primitives = SyncVectorBuilder()
        .push_back(modelComp)
        .finish();

    // Build RigidBody
    SyncValue rigidBody = SyncMapBuilder()
        .set("ComponentType", "PhysicsStaticSceneBody")
        .set("EnableContactNotify", false)
        .set("Unwalkable", false)
        .set("TemplateRes", "Scenes/Architecture/CloudMansion/Structure/AutoPhyRBTemplate")
        .set("[Type]", "PhysicsStaticSceneBody")
        .finish();

    // Build RigidBodies array
    SyncValue rigidBodies = SyncVectorBuilder()
        .push_back(rigidBody)
        .finish();

    // Build Appearance component
    SyncValue appearance = SyncMapBuilder()
        .set("DepthOffset", static_cast<int64_t>(0))
        .set("[Type]", "IAppearanceComponent")
        .finish();

    // Build Tag component
    SyncValue tag = SyncMapBuilder()
        .set("TagString", "")
        .set("[Type]", "TagComponent")
        .finish();

    // Build PropertyData - complex nested structure
    SyncValue propertyData = SyncMapBuilder()
        .set("GenerateOccluder", false)
        .set("DeleteOccluder", false)
        .set("IsVisible", true)
        .set("IsDisableCollision", false)
        .set("IsBillboard", false)
        .set("IsReflectionVisible", false)
        .set("IsOutlined", false)
        .set("IsThermalVisible", false)
        .set("DetailLevel", static_cast<int64_t>(0))
        .set("TechState", static_cast<int64_t>(0))
        .set("TechParam", techParam)
        .set("TechParam2", techParam2)
        .set("TintColor1", tintColor)
        .set("TintColor2", tintColor)
        .set("TintColor3", tintColor)
        .set("LodThreshold", techParam)
        .set("Anchor", techParam)
        .set("IsCastDynamicShadow", true)
        .set("IsReceiveDynamicShadow", true)
        .set("IsSDFGen", true)
        .set("HasCollision", true)
        .set("[Type]", "SceneObjectType_9")
        .set("WorldName", "L_CloudMansion_02")
        .set("LevelName", "L_CloudMansion_Mesh_02")
        .set("Primitives", primitives)
        .set("RigidBodies", rigidBodies)
        .set("Appearance", appearance)
        .set("Tag", tag)
        .finish();

    // Build PropertyPaths array
    SyncValue propertyPaths = SyncVectorBuilder()
        .push_back("PropertyData")
        .push_back("PropertyData/Primitives/0")
        .push_back("PropertyData/Primitives/0/SyncModel")
        .push_back("PropertyData/RigidBodies/0")
        .finish();

    // Build Components array
    SyncValue components = SyncVectorBuilder()
        .push_back(SyncMapBuilder()
            .set("DisplayName", "[ModelComponent]")
            .set("Icon", "Comp_Model")
            .finish())
        .finish();

    // Build position (Vec3)
    SyncValue position = SyncVectorBuilder()
        .push_back(static_cast<double>(index % 1000))
        .push_back(0.06)
        .push_back(static_cast<double>((index / 1000) % 1000))
        .finish();

    // Build scale (Vec3)
    SyncValue scale = SyncVectorBuilder()
        .push_back(1.0)
        .push_back(1.0)
        .push_back(1.0)
        .finish();

    // Build euler (Vec3)
    SyncValue euler = SyncVectorBuilder()
        .push_back(0.0)
        .push_back(static_cast<double>(index % 360))
        .push_back(0.0)
        .finish();

    // Build property sub-object
    SyncValue property = SyncMapBuilder()
        .set("name", "IEntity")
        .finish();

    // Build the final scene object using Builder API - O(n) construction!
    return SyncMapBuilder()
        .set("property", property)
        .set("filename", "")
        .set("space_object_type", static_cast<int64_t>(1048576))
        .set("scene_object_id", id)
        .set("parent", "A7DC0D1A-7B421DB0-5B8D7D86-FDB2A65F")
        .set("level", "CB9552E0-F1495927-71830CA6-BE6E082F")
        .set("position", position)
        .set("scale", scale)
        .set("euler", euler)
        .set("visible_mask", true)
        .set("in_world", true)
        .set("scene_object_layer", static_cast<int64_t>(143))
        .set("name", "SM_CM_1L_Building_" + std::to_string(index))
        .set("file", "Scenes/Architecture/CloudMansion/Structure/SM_CM_L1_Building_" + std::to_string(index % 100))
        .set("scene_object_locked", false)
        .set("scene_object_type", static_cast<int64_t>(9))
        .set("ModelResource", "Scenes/Architecture/CloudMansion/Structure/SM_CM_L1_Building_" + std::to_string(index % 100))
        .set("PropertyData", propertyData)
        .set("PropertyPaths", propertyPaths)
        .set("Components", components)
        .finish();
}

// Generate large-scale test data using real scene object structure - SyncValue version
//
// OPTIMIZED: Uses SyncMapBuilder for O(n) construction
SyncValue generate_large_scene_sync(size_t object_count) {
    std::cout << "Generating scene with " << object_count << " objects (SyncValue - Builder API)...\n";
    std::cout << "Using real scene_object_map.json structure with O(n) construction\n";

    Timer timer;
    timer.start();

    // Use SyncMapBuilder for O(n) construction
    SyncMapBuilder objects_builder;

    for (size_t i = 0; i < object_count; ++i) {
        std::string key = generate_uuid_like_id(i);  // UUID-like key
        SyncValue obj = create_scene_object_sync(i); // Now returns SyncValue
        objects_builder.set(key, obj);

        if ((i + 1) % 10000 == 0) {
            std::cout << "  Generated " << (i + 1) << " objects...\n";
        }
    }

    double elapsed = timer.elapsed_ms();
    std::cout << "Scene generation completed in " << std::fixed << std::setprecision(2)
              << elapsed << " ms\n";

    // Build the final scene using Builder API
    return SyncMapBuilder()
        .set("scene_object_map", objects_builder.finish())
        .finish();
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
        std::cout << "Shared memory used: " << region.header()->heap_used
                  << " bytes (" << std::fixed << std::setprecision(2)
                  << (region.header()->heap_used / 1024.0 / 1024.0) << " MB)\n";
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

    // Record offset to header
    header->value_offset = static_cast<char*>(value_storage) - static_cast<char*>(region.base());

    shared_memory::set_current_shared_region(nullptr);

    std::cout << "\n--- Performance Stats ---\n";
    std::cout << "Direct build time: " << std::fixed << std::setprecision(2) << build_time << " ms\n";
    std::cout << "Memory used: " << header->heap_used
              << " bytes (" << (header->heap_used / 1024.0 / 1024.0) << " MB)\n";
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
    std::cout << "Memory used: " << handle.region().header()->heap_used << " bytes\n";

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
    constexpr size_t OBJECT_COUNT = 30000;  // 30,000 objects

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
        // Generate local Value first (before creating shared memory)
        Value data = generate_large_scene(OBJECT_COUNT);

        // Create shared memory
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTest2", 1024 * 1024 * 1024)) {  // 1GB
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        shared_memory::set_current_shared_region(&region);

        // Deep copy to shared memory (measure this)
        timer.start();
        SharedValue shared = deep_copy_to_shared(data);
        deep_copy_to_shared_time = timer.elapsed_ms();

        // Release local data immediately to free memory before copy back
        data = Value{};

        // Deep copy back to local
        timer.start();
        Value local = deep_copy_to_local(shared);
        deep_copy_to_local_time_m2 = timer.elapsed_ms();

        shared_memory_used_m2 = region.header()->heap_used;

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

        shared_memory_used_m3 = region.header()->heap_used;

        shared_memory::set_current_shared_region(nullptr);
        region.close();

        std::cout << "  Direct build:     " << std::fixed << std::setprecision(2) << direct_build_time << " ms\n";
        std::cout << "  Copy to local:    " << deep_copy_to_local_time_m3 << " ms\n";
        std::cout << "  Total:            " << (direct_build_time + deep_copy_to_local_time_m3) << " ms\n";
        std::cout << "  Shared mem used:  " << (shared_memory_used_m3 / 1024.0 / 1024.0) << " MB\n\n";
    }

    //==========================================================================
    // Method 4: Shared Memory (ZERO-COPY: direct read, no copy!)
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

        // ZERO-COPY: Editor directly traverses and reads data in shared memory
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
    std::cout << std::string(100, '=') << "\n";
    std::cout << "SUMMARY (" << OBJECT_COUNT << " objects)\n";
    std::cout << std::string(100, '-') << "\n";
    std::cout << "                    | Method 1     | Method 2     | Method 3     | Method 4     \n";
    std::cout << "                    | (CustomBin)  | (2-copy)     | (1-copy)     | (ZERO-COPY)  \n";
    std::cout << std::string(100, '-') << "\n";

    double editor_m1 = deserialize_time;
    double editor_m2 = deep_copy_to_local_time_m2;
    double editor_m3 = deep_copy_to_local_time_m3;
    double editor_m4 = direct_read_time;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Engine side time    | " << std::setw(10) << serialize_time << " | "
              << std::setw(10) << deep_copy_to_shared_time << " | "
              << std::setw(10) << direct_build_time << " | "
              << std::setw(10) << direct_build_time << " ms\n";
    std::cout << "Editor side time    | " << std::setw(10) << editor_m1 << " | "
              << std::setw(10) << editor_m2 << " | "
              << std::setw(10) << editor_m3 << " | "
              << std::setw(10) << editor_m4 << " ms\n";
    std::cout << std::string(100, '=') << "\n\n";

    std::cout << "Conclusion:\n";
    std::cout << "  - Method 4 (TRUE ZERO-COPY) is the FASTEST for read-only access!\n";
    std::cout << "  - Method 3 (1-copy) is best for editable local copy.\n";
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
// Helper function: traverse FastSharedValue
//==============================================================================

size_t traverse_fast_shared_value(const FastSharedValue& sv) {
    size_t count = 1;

    if (auto* map = sv.get_if<FastSharedValueMap>()) {
        for (const auto& [key, box] : *map) {
            count += traverse_fast_shared_value(box.get());
        }
    }
    else if (auto* vec = sv.get_if<FastSharedValueVector>()) {
        for (const auto& box : *vec) {
            count += traverse_fast_shared_value(box.get());
        }
    }
    else if (auto* arr = sv.get_if<FastSharedValueArray>()) {
        for (const auto& box : *arr) {
            count += traverse_fast_shared_value(box.get());
        }
    }

    return count;
}

//==============================================================================
// Generate large-scale test data directly in shared memory - FastSharedValue version
// Uses transient for O(n) construction instead of O(n log n)
//==============================================================================

FastSharedValue generate_large_scene_fast_shared(size_t object_count) {
    std::cout << "Generating scene with " << object_count << " objects (FastSharedValue - O(n) transient)...\n";

    Timer timer;
    timer.start();

    // FastSharedValue uses fake_transience_policy, so we can use transient for O(n) performance!
    auto objects_transient = FastSharedValueVector{}.transient();

    for (size_t i = 0; i < object_count; ++i) {
        auto obj_transient = FastSharedValueMap{}.transient();
        obj_transient.set(shared_memory::SharedString("id"),
                          FastSharedValueBox{FastSharedValue{static_cast<int64_t>(i)}});
        obj_transient.set(shared_memory::SharedString("name"),
                          FastSharedValueBox{FastSharedValue{"Object_" + std::to_string(i)}});
        obj_transient.set(shared_memory::SharedString("visible"),
                          FastSharedValueBox{FastSharedValue{true}});

        // Transform properties (using transient for efficiency)
        auto transform_transient = FastSharedValueMap{}.transient();
        transform_transient.set(shared_memory::SharedString("x"),
                                FastSharedValueBox{FastSharedValue{static_cast<double>(i % 1000)}});
        transform_transient.set(shared_memory::SharedString("y"),
                                FastSharedValueBox{FastSharedValue{static_cast<double>((i / 1000) % 1000)}});
        transform_transient.set(shared_memory::SharedString("z"),
                                FastSharedValueBox{FastSharedValue{static_cast<double>(i / 1000000)}});
        transform_transient.set(shared_memory::SharedString("rotation"),
                                FastSharedValueBox{FastSharedValue{static_cast<double>(i % 360)}});
        transform_transient.set(shared_memory::SharedString("scale"),
                                FastSharedValueBox{FastSharedValue{1.0}});
        obj_transient.set(shared_memory::SharedString("transform"),
                          FastSharedValueBox{FastSharedValue{transform_transient.persistent()}});

        // Material properties
        auto material_transient = FastSharedValueMap{}.transient();
        material_transient.set(shared_memory::SharedString("color"),
                               FastSharedValueBox{FastSharedValue{"#" + std::to_string(i % 0xFFFFFF)}});
        material_transient.set(shared_memory::SharedString("opacity"),
                               FastSharedValueBox{FastSharedValue{1.0}});
        material_transient.set(shared_memory::SharedString("roughness"),
                               FastSharedValueBox{FastSharedValue{0.5}});
        obj_transient.set(shared_memory::SharedString("material"),
                          FastSharedValueBox{FastSharedValue{material_transient.persistent()}});

        // Tags
        auto tags_transient = FastSharedValueVector{}.transient();
        tags_transient.push_back(FastSharedValueBox{FastSharedValue{"tag_" + std::to_string(i % 10)}});
        tags_transient.push_back(FastSharedValueBox{FastSharedValue{"layer_" + std::to_string(i % 5)}});
        obj_transient.set(shared_memory::SharedString("tags"),
                          FastSharedValueBox{FastSharedValue{tags_transient.persistent()}});

        objects_transient.push_back(FastSharedValueBox{FastSharedValue{obj_transient.persistent()}});

        // Progress display
        if ((i + 1) % 10000 == 0) {
            std::cout << "  Generated " << (i + 1) << " objects...\n";
        }
    }

    auto scene_transient = FastSharedValueMap{}.transient();
    scene_transient.set(shared_memory::SharedString("version"),
                        FastSharedValueBox{FastSharedValue{1}});
    scene_transient.set(shared_memory::SharedString("name"),
                        FastSharedValueBox{FastSharedValue{"Large Scene (Fast)"}});
    scene_transient.set(shared_memory::SharedString("objects"),
                        FastSharedValueBox{FastSharedValue{objects_transient.persistent()}});

    double elapsed = timer.elapsed_ms();
    std::cout << "Scene generation completed in " << std::fixed << std::setprecision(2)
              << elapsed << " ms\n";

    return FastSharedValue{scene_transient.persistent()};
}

//==============================================================================
// SharedValue vs FastSharedValue Performance Comparison
// Compares O(n log n) construction vs O(n) transient construction
//==============================================================================

void shared_vs_fast_shared_comparison() {
    constexpr size_t OBJECT_COUNT = 50000;  // 50,000 objects

    std::cout << "\n" << std::string(100, '=') << "\n";
    std::cout << "SharedValue vs FastSharedValue Performance Comparison (" << OBJECT_COUNT << " objects)\n";
    std::cout << std::string(100, '=') << "\n\n";

    std::cout << "This test compares:\n";
    std::cout << "  - SharedValue:     no_transience_policy, O(n log n) construction\n";
    std::cout << "  - FastSharedValue: fake_transience_policy, O(n) transient construction\n";
    std::cout << "\n";
    std::cout << "Both use the same shared memory allocator (bump allocator).\n";
    std::cout << "The difference is in transient support for efficient bulk construction.\n";
    std::cout << "\n";

    Timer timer;

    //==========================================================================
    // Phase 1: Construction Performance (the key difference!)
    //==========================================================================
    std::cout << "=== Phase 1: Construction Performance (Key Difference) ===\n\n";

    double shared_construct_time = 0;
    double fast_shared_construct_time = 0;
    size_t shared_memory_used_1 = 0;
    size_t shared_memory_used_2 = 0;

    // SharedValue construction (O(n log n) - no transient support)
    std::cout << "--- SharedValue (no transient, O(n log n)) ---\n";
    {
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTestShared", 1024 * 1024 * 1024)) {
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        shared_memory::set_current_shared_region(&region);

        timer.start();
        SharedValue shared = generate_large_scene_shared(OBJECT_COUNT);
        shared_construct_time = timer.elapsed_ms();
        shared_memory_used_1 = region.header()->heap_used;

        shared_memory::set_current_shared_region(nullptr);
        region.close();
    }

    // FastSharedValue construction (O(n) - with transient support)
    std::cout << "\n--- FastSharedValue (with transient, O(n)) ---\n";
    {
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTestFastShared", 1024 * 1024 * 1024)) {
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        shared_memory::set_current_shared_region(&region);

        timer.start();
        FastSharedValue fast_shared = generate_large_scene_fast_shared(OBJECT_COUNT);
        fast_shared_construct_time = timer.elapsed_ms();
        shared_memory_used_2 = region.header()->heap_used;

        shared_memory::set_current_shared_region(nullptr);
        region.close();
    }

    std::cout << "\n--- Construction Results ---\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  SharedValue construction:     " << shared_construct_time << " ms (O(n log n))\n";
    std::cout << "  FastSharedValue construction: " << fast_shared_construct_time << " ms (O(n))\n";
    double speedup = shared_construct_time / fast_shared_construct_time;
    std::cout << "  Speedup: " << speedup << "x faster with FastSharedValue!\n";
    std::cout << "  Memory used (SharedValue):     " << (shared_memory_used_1 / 1024.0 / 1024.0) << " MB\n";
    std::cout << "  Memory used (FastSharedValue): " << (shared_memory_used_2 / 1024.0 / 1024.0) << " MB\n\n";

    //==========================================================================
    // Phase 2: Traversal Performance (should be similar)
    //==========================================================================
    std::cout << "=== Phase 2: Traversal Performance (should be similar) ===\n\n";

    double shared_traverse_time = 0;
    double fast_shared_traverse_time = 0;
    size_t shared_node_count = 0;
    size_t fast_shared_node_count = 0;

    // SharedValue traversal
    {
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTestSharedTrav", 1024 * 1024 * 1024)) {
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        shared_memory::set_current_shared_region(&region);

        SharedValue shared = generate_large_scene_shared(OBJECT_COUNT);

        timer.start();
        shared_node_count = traverse_shared_value(shared);
        shared_traverse_time = timer.elapsed_ms();

        shared_memory::set_current_shared_region(nullptr);
        region.close();

        std::cout << "  SharedValue: Traversed " << shared_node_count << " nodes in " << shared_traverse_time << " ms\n";
    }

    // FastSharedValue traversal
    {
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTestFastSharedTrav", 1024 * 1024 * 1024)) {
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        shared_memory::set_current_shared_region(&region);

        FastSharedValue fast_shared = generate_large_scene_fast_shared(OBJECT_COUNT);

        timer.start();
        fast_shared_node_count = traverse_fast_shared_value(fast_shared);
        fast_shared_traverse_time = timer.elapsed_ms();

        shared_memory::set_current_shared_region(nullptr);
        region.close();

        std::cout << "  FastSharedValue: Traversed " << fast_shared_node_count << " nodes in " << fast_shared_traverse_time << " ms\n";
    }

    std::cout << "\n--- Traversal Results ---\n";
    std::cout << "  SharedValue traversal time:     " << shared_traverse_time << " ms\n";
    std::cout << "  FastSharedValue traversal time: " << fast_shared_traverse_time << " ms\n";
    std::cout << "  (Traversal should be similar since both use same data structures)\n\n";

    //==========================================================================
    // Phase 3: Deep Copy to Local Performance (should be similar)
    //==========================================================================
    std::cout << "=== Phase 3: Deep Copy to Local Performance ===\n\n";

    double shared_copy_time = 0;
    double fast_shared_copy_time = 0;

    // SharedValue deep copy
    {
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTestSharedCopy", 1024 * 1024 * 1024)) {
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        shared_memory::set_current_shared_region(&region);

        SharedValue shared = generate_large_scene_shared(OBJECT_COUNT);

        timer.start();
        Value local = deep_copy_to_local(shared);
        shared_copy_time = timer.elapsed_ms();

        shared_memory::set_current_shared_region(nullptr);
        region.close();

        std::cout << "  SharedValue -> Value: " << shared_copy_time << " ms\n";
    }

    // FastSharedValue deep copy
    {
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTestFastSharedCopy", 1024 * 1024 * 1024)) {
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        shared_memory::set_current_shared_region(&region);

        FastSharedValue fast_shared = generate_large_scene_fast_shared(OBJECT_COUNT);

        timer.start();
        Value local = fast_deep_copy_to_local(fast_shared);
        fast_shared_copy_time = timer.elapsed_ms();

        shared_memory::set_current_shared_region(nullptr);
        region.close();

        std::cout << "  FastSharedValue -> Value: " << fast_shared_copy_time << " ms\n";
    }

    std::cout << "\n--- Deep Copy Results ---\n";
    std::cout << "  SharedValue copy time:     " << shared_copy_time << " ms\n";
    std::cout << "  FastSharedValue copy time: " << fast_shared_copy_time << " ms\n";
    std::cout << "  (Deep copy should be similar since destination uses transient)\n\n";

    //==========================================================================
    // Phase 4: Deep Copy TO Shared Memory (THE KEY DIFFERENCE!)
    // This is where O(n log n) vs O(n) really matters!
    //==========================================================================
    std::cout << "=== Phase 4: Deep Copy TO Shared Memory (Key Difference!) ===\n\n";
    std::cout << "This phase compares:\n";
    std::cout << "  - deep_copy_to_shared():      Value -> SharedValue (O(n log n), no transient)\n";
    std::cout << "  - fast_deep_copy_to_shared(): Value -> FastSharedValue (O(n), uses transient)\n\n";

    double to_shared_time = 0;
    double to_fast_shared_time = 0;
    size_t to_shared_memory = 0;
    size_t to_fast_shared_memory = 0;

    // First, generate a local Value to copy from
    std::cout << "Generating local Value for copy test...\n";
    Value local_data = generate_large_scene(OBJECT_COUNT);
    std::cout << "Local Value generated.\n\n";

    // Test deep_copy_to_shared (O(n log n) - no transient)
    std::cout << "--- deep_copy_to_shared (O(n log n)) ---\n";
    {
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTestToShared", 1024 * 1024 * 1024)) {
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        shared_memory::set_current_shared_region(&region);

        timer.start();
        SharedValue shared = deep_copy_to_shared(local_data);
        to_shared_time = timer.elapsed_ms();
        to_shared_memory = region.header()->heap_used;

        std::cout << "  Time: " << to_shared_time << " ms\n";
        std::cout << "  Memory used: " << (to_shared_memory / 1024.0 / 1024.0) << " MB\n";

        shared_memory::set_current_shared_region(nullptr);
        region.close();
    }

    // Test fast_deep_copy_to_shared (O(n) - uses transient)
    std::cout << "\n--- fast_deep_copy_to_shared (O(n)) ---\n";
    {
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTestToFastShared", 1024 * 1024 * 1024)) {
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        shared_memory::set_current_shared_region(&region);

        timer.start();
        FastSharedValue fast_shared = fast_deep_copy_to_shared(local_data);
        to_fast_shared_time = timer.elapsed_ms();
        to_fast_shared_memory = region.header()->heap_used;

        std::cout << "  Time: " << to_fast_shared_time << " ms\n";
        std::cout << "  Memory used: " << (to_fast_shared_memory / 1024.0 / 1024.0) << " MB\n";

        shared_memory::set_current_shared_region(nullptr);
        region.close();
    }

    std::cout << "\n--- Phase 4 Results (THE KEY COMPARISON!) ---\n";
    double to_shared_speedup = to_shared_time / to_fast_shared_time;
    double memory_ratio = static_cast<double>(to_shared_memory) / to_fast_shared_memory;
    std::cout << "  deep_copy_to_shared:      " << to_shared_time << " ms, "
              << (to_shared_memory / 1024.0 / 1024.0) << " MB\n";
    std::cout << "  fast_deep_copy_to_shared: " << to_fast_shared_time << " ms, "
              << (to_fast_shared_memory / 1024.0 / 1024.0) << " MB\n";
    std::cout << "  Speedup: " << to_shared_speedup << "x faster with fast_deep_copy_to_shared!\n";
    std::cout << "  Memory ratio: " << memory_ratio << "x (SharedValue uses more due to O(n log n) intermediates)\n\n";

    //==========================================================================
    // Summary
    //==========================================================================
    std::cout << std::string(100, '=') << "\n";
    std::cout << "SUMMARY: SharedValue vs FastSharedValue Performance\n";
    std::cout << std::string(100, '-') << "\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "                             | SharedValue   | FastSharedValue | Speedup\n";
    std::cout << std::string(100, '-') << "\n";
    std::cout << "Direct Construction (ms)     | " << std::setw(13) << shared_construct_time
              << " | " << std::setw(15) << fast_shared_construct_time
              << " | " << std::setw(6) << speedup << "x\n";
    std::cout << "Copy TO Shared (ms) [KEY!]   | " << std::setw(13) << to_shared_time
              << " | " << std::setw(15) << to_fast_shared_time
              << " | " << std::setw(6) << to_shared_speedup << "x\n";
    std::cout << "Memory for Copy (MB) [KEY!]  | " << std::setw(13) << (to_shared_memory / 1024.0 / 1024.0)
              << " | " << std::setw(15) << (to_fast_shared_memory / 1024.0 / 1024.0)
              << " | " << std::setw(6) << memory_ratio << "x\n";
    std::cout << "Traversal (ms)               | " << std::setw(13) << shared_traverse_time
              << " | " << std::setw(15) << fast_shared_traverse_time
              << " | " << std::setw(6) << (shared_traverse_time / fast_shared_traverse_time) << "x\n";
    std::cout << "Deep copy to local (ms)      | " << std::setw(13) << shared_copy_time
              << " | " << std::setw(15) << fast_shared_copy_time
              << " | " << std::setw(6) << (shared_copy_time / fast_shared_copy_time) << "x\n";
    std::cout << std::string(100, '=') << "\n\n";

    std::cout << "Conclusion:\n";
    std::cout << "  - [KEY] Copy TO Shared: fast_deep_copy_to_shared is " << to_shared_speedup << "x faster!\n";
    std::cout << "  - [KEY] Memory Usage: fast version uses " << memory_ratio << "x less memory!\n";
    std::cout << "  - Direct construction: both are fast (use std::move optimization)\n";
    std::cout << "  - Traversal and deep copy to local are similar.\n";
    std::cout << "\n";

    std::cout << "Recommendations:\n";
    std::cout << "  - Use FastSharedValue when building large data structures (>10000 elements)\n";
    std::cout << "  - Use SharedValue for small data or when you need the simplest API\n";
    std::cout << "  - Both can be deep-copied to local Value for editing\n";
    std::cout << "  - Both support zero-copy read-only access\n";
}

//==============================================================================
// Main Function
//==============================================================================

void print_usage() {
    std::cout << "Usage: shared_value_demo [command]\n";
    std::cout << "\nCommands:\n";
    std::cout << "  single         - Single process demo (default)\n";
    std::cout << "  publish N      - Run as publisher with N objects\n";
    std::cout << "  subscribe      - Run as subscriber\n";
    std::cout << "  perf           - Performance comparison (4 methods)\n";
    std::cout << "  value_sync     - Value vs SyncValue performance comparison\n";
    std::cout << "  shared_fast    - SharedValue vs FastSharedValue comparison\n";
    std::cout << "\nExamples:\n";
    std::cout << "  shared_value_demo single\n";
    std::cout << "  shared_value_demo publish 10000\n";
    std::cout << "  shared_value_demo subscribe\n";
    std::cout << "  shared_value_demo value_sync\n";
    std::cout << "  shared_value_demo shared_fast\n";
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
    else if (command == "shared_fast") {
        shared_vs_fast_shared_comparison();
    }
    else {
        print_usage();
        return 1;
    }

    return 0;
}
