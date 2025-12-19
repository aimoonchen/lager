// shared_value_demo.cpp
// 演示 SharedValue 的跨进程零拷贝传输
//
// 这个演示程序展示：
// 1. B 进程如何创建共享内存并写入 Value
// 2. A 进程如何打开共享内存并深拷贝到本地
// 3. 性能对比：共享内存方案 vs 序列化方案

// 必须在 Windows.h 之前定义，防止 min/max 宏与 std::min/std::max 冲突
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
// 性能测试工具
//==============================================================================

// 获取当前时间戳（毫秒）
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
// 测试数据生成
//==============================================================================

// 生成大规模测试数据（模拟场景对象）- 本地 Value 版本
Value generate_large_scene(size_t object_count) {
    std::cout << "Generating scene with " << object_count << " objects (local Value)...\n";
    
    Timer timer;
    timer.start();
    
    // 使用 transient 提高构造性能
    auto objects_transient = ValueVector{}.transient();
    
    for (size_t i = 0; i < object_count; ++i) {
        // 每个对象有多个属性
        ValueMap obj;
        obj = obj.set("id", ValueBox{Value{static_cast<int64_t>(i)}});
        obj = obj.set("name", ValueBox{Value{"Object_" + std::to_string(i)}});
        obj = obj.set("visible", ValueBox{Value{true}});
        
        // Transform 属性
        ValueMap transform;
        transform = transform.set("x", ValueBox{Value{static_cast<double>(i % 1000)}});
        transform = transform.set("y", ValueBox{Value{static_cast<double>((i / 1000) % 1000)}});
        transform = transform.set("z", ValueBox{Value{static_cast<double>(i / 1000000)}});
        transform = transform.set("rotation", ValueBox{Value{static_cast<double>(i % 360)}});
        transform = transform.set("scale", ValueBox{Value{1.0}});
        obj = obj.set("transform", ValueBox{Value{transform}});
        
        // 材质属性
        ValueMap material;
        material = material.set("color", ValueBox{Value{"#" + std::to_string(i % 0xFFFFFF)}});
        material = material.set("opacity", ValueBox{Value{1.0}});
        material = material.set("roughness", ValueBox{Value{0.5}});
        obj = obj.set("material", ValueBox{Value{material}});
        
        // 标签
        ValueVector tags;
        tags = tags.push_back(ValueBox{Value{"tag_" + std::to_string(i % 10)}});
        tags = tags.push_back(ValueBox{Value{"layer_" + std::to_string(i % 5)}});
        obj = obj.set("tags", ValueBox{Value{tags}});
        
        objects_transient.push_back(ValueBox{Value{obj}});
        
        // 进度显示
        if ((i + 1) % 10000 == 0) {
            std::cout << "  Generated " << (i + 1) << " objects...\n";
        }
    }
    
    ValueMap scene;
    scene = scene.set("version", ValueBox{Value{1}});
    scene = scene.set("name", ValueBox{Value{"Large Scene"}});
    scene = scene.set("objects", ValueBox{Value{objects_transient.persistent()}});
    
    double elapsed = timer.elapsed_ms();
    std::cout << "Scene generation completed in " << std::fixed << std::setprecision(2) 
              << elapsed << " ms\n";
    
    return Value{scene};
}

// 直接在共享内存上生成大规模测试数据（模拟场景对象）- SharedValue 版本
// 这是真正高性能的方案：数据直接构造在共享内存上，无需中间拷贝
SharedValue generate_large_scene_shared(size_t object_count) {
    std::cout << "Generating scene with " << object_count << " objects (direct SharedValue)...\n";
    
    Timer timer;
    timer.start();
    
    // 注意：SharedValue 由于使用 no_transience_policy，不能使用 transient
    // 但由于 bump allocator 分配非常快，性能依然很好
    SharedValueVector objects;
    
    for (size_t i = 0; i < object_count; ++i) {
        // 每个对象有多个属性
        SharedValueMap obj;
        obj = std::move(obj).set(shared_memory::SharedString("id"), 
                                  SharedValueBox{SharedValue{static_cast<int64_t>(i)}});
        obj = std::move(obj).set(shared_memory::SharedString("name"), 
                                  SharedValueBox{SharedValue{"Object_" + std::to_string(i)}});
        obj = std::move(obj).set(shared_memory::SharedString("visible"), 
                                  SharedValueBox{SharedValue{true}});
        
        // Transform 属性
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
        
        // 材质属性
        SharedValueMap material;
        material = std::move(material).set(shared_memory::SharedString("color"), 
                                            SharedValueBox{SharedValue{"#" + std::to_string(i % 0xFFFFFF)}});
        material = std::move(material).set(shared_memory::SharedString("opacity"), 
                                            SharedValueBox{SharedValue{1.0}});
        material = std::move(material).set(shared_memory::SharedString("roughness"), 
                                            SharedValueBox{SharedValue{0.5}});
        obj = std::move(obj).set(shared_memory::SharedString("material"), 
                                  SharedValueBox{SharedValue{std::move(material)}});
        
        // 标签
        SharedValueVector tags;
        tags = std::move(tags).push_back(SharedValueBox{SharedValue{"tag_" + std::to_string(i % 10)}});
        tags = std::move(tags).push_back(SharedValueBox{SharedValue{"layer_" + std::to_string(i % 5)}});
        obj = std::move(obj).set(shared_memory::SharedString("tags"), 
                                  SharedValueBox{SharedValue{std::move(tags)}});
        
        objects = std::move(objects).push_back(SharedValueBox{SharedValue{std::move(obj)}});
        
        // 进度显示
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
// 单进程模拟测试
//==============================================================================

void demo_single_process() {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "Demo: Single Process Simulation\n";
    std::cout << std::string(60, '=') << "\n\n";
    
    // 生成测试数据
    constexpr size_t OBJECT_COUNT = 1000;  // 1000 个对象用于快速测试
    SharedValue original = generate_large_scene(OBJECT_COUNT);
    
    std::cout << "\nOriginal SharedValue created.\n";
    std::cout << "Scene objects count: " << original.at("objects").size() << "\n";
    
    // 方案 1: 序列化/反序列化
    std::cout << "\n--- Method 1: Serialization/Deserialization ---\n";
    {
        Timer timer;
        
        timer.start();
        ByteBuffer buffer = serialize(original);
        double serialize_time = timer.elapsed_ms();
        
        timer.start();
        SharedValue deserialized = deserialize(buffer);
        double deserialize_time = timer.elapsed_ms();
        
        std::cout << "Serialized size: " << buffer.size() << " bytes ("
                  << std::fixed << std::setprecision(2) 
                  << (buffer.size() / 1024.0 / 1024.0) << " MB)\n";
        std::cout << "Serialize time: " << serialize_time << " ms\n";
        std::cout << "Deserialize time: " << deserialize_time << " ms\n";
        std::cout << "Total time: " << (serialize_time + deserialize_time) << " ms\n";
        
        // 验证
        if (deserialized == original) {
            std::cout << "Verification: PASSED\n";
        } else {
            std::cout << "Verification: FAILED\n";
        }
    }
    
    // 方案 2: 共享内存深拷贝
    std::cout << "\n--- Method 2: Shared Memory Deep Copy ---\n";
    {
        Timer timer;
        
        // 模拟 B 进程：创建共享内存并写入
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
        
        // 模拟 A 进程：从共享内存深拷贝
        timer.start();
        Value copied = deep_copy_to_local(shared);
        double copy_time = timer.elapsed_ms();
        
        std::cout << "Deep copy to local time: " << copy_time << " ms\n";
        std::cout << "Total time: " << (write_time + copy_time) << " ms\n";
        
        // 验证
        if (copied == original) {
            std::cout << "Verification: PASSED\n";
        } else {
            std::cout << "Verification: FAILED\n";
        }
        
        region.close();
    }
}

//==============================================================================
// 跨进程测试 - Publisher (B 进程) - 高性能版本
// 直接在共享内存上构造 SharedValue，无需中间拷贝
//==============================================================================

void demo_publisher(size_t object_count) {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "Demo: Publisher Process (Engine/B Process)\n";
    std::cout << "Using HIGH-PERFORMANCE direct SharedValue construction!\n";
    std::cout << std::string(60, '=') << "\n\n";
    
    Timer timer;
    
    // 先创建共享内存区域
    size_t estimated_size = object_count * 500;  // 估计每个对象约 500 字节
    estimated_size = std::max(estimated_size, size_t(64 * 1024 * 1024));  // 至少 64MB
    
    shared_memory::SharedMemoryRegion region;
    if (!region.create("EditorEngineSharedState", estimated_size)) {
        std::cerr << "Failed to create shared memory!\n";
        std::cerr << "Error code: " << GetLastError() << "\n";
        return;
    }
    
    std::cout << "Shared memory created at: " << region.base() << "\n";
    std::cout << "Shared memory size: " << (estimated_size / 1024.0 / 1024.0) << " MB\n\n";
    
    // 设置当前共享内存区域，这样所有 SharedValue 都会分配在这里
    shared_memory::set_current_shared_region(&region);
    
    // 直接在共享内存上构造场景数据（高性能方案）
    timer.start();
    SharedValue shared_scene = generate_large_scene_shared(object_count);
    double build_time = timer.elapsed_ms();
    
    // 将 SharedValue 存储到共享内存头部，供订阅者访问
    // 注意：SharedValue 本身也在共享内存上，所以这里只是记录其地址
    auto* header = region.header();
    
    // 在共享内存中分配一个 SharedValue 来存储场景
    void* value_storage = shared_memory::shared_heap::allocate(sizeof(SharedValue));
    new (value_storage) SharedValue(std::move(shared_scene));
    
    // 记录偏移量到头部（使用 memory_order_release 确保之前的写入都可见）
    header->value_offset.store(
        static_cast<char*>(value_storage) - static_cast<char*>(region.base()),
        std::memory_order_release);
    
    shared_memory::set_current_shared_region(nullptr);
    
    std::cout << "\n--- Performance Stats ---\n";
    std::cout << "Direct build time: " << std::fixed << std::setprecision(2) << build_time << " ms\n";
    std::cout << "Memory used: " << header->heap_used.load() 
              << " bytes (" << (header->heap_used.load() / 1024.0 / 1024.0) << " MB)\n";
    std::cout << "Value stored at offset: " << header->value_offset << "\n";
    
    // 对比：如果使用序列化方案会需要多久
    std::cout << "\n--- Comparison: What if using serialization? ---\n";
    Value local_scene = generate_large_scene(object_count);
    timer.start();
    ByteBuffer buffer = serialize(local_scene);
    double ser_time = timer.elapsed_ms();
    std::cout << "Serialization would take: " << ser_time << " ms\n";
    std::cout << "Serialized size: " << (buffer.size() / 1024.0 / 1024.0) << " MB\n";
    
    // 等待订阅者连接
    std::cout << "\nPublisher ready. Run another instance with 'subscribe' to test.\n";
    std::cout << "Press Enter to exit...\n";
    std::cin.get();
    
    region.close();
    std::cout << "Publisher exited.\n";
}

//==============================================================================
// 跨进程测试 - Subscriber (A 进程)
//==============================================================================

void demo_subscriber() {
    std::cout << "\n" << std::string(60, '=') << "\n";
    std::cout << "Demo: Subscriber Process (Editor/A Process)\n";
    std::cout << std::string(60, '=') << "\n\n";
    
    // 使用 SharedValueHandle 打开共享内存
    SharedValueHandle handle;
    
    std::cout << "Trying to open shared memory...\n";
    
    if (!handle.open("EditorEngineSharedState")) {
        std::cerr << "Failed to open shared memory!\n";
        std::cerr << "Make sure the publisher is running first.\n";
        std::cerr << "Error code: " << GetLastError() << "\n";
        return;
    }
    
    std::cout << "Shared memory opened at: " << handle.region().base() << "\n";
    std::cout << "Shared memory size: " << handle.region().size() << " bytes\n";
    std::cout << "Memory used: " << handle.region().header()->heap_used.load() << " bytes\n";
    
    // 验证地址匹配
    if (handle.region().base() != handle.region().header()->fixed_base_address) {
        std::cerr << "ERROR: Address mismatch!\n";
        std::cerr << "Expected: " << handle.region().header()->fixed_base_address << "\n";
        std::cerr << "Got: " << handle.region().base() << "\n";
        std::cerr << "This would cause pointer issues. Cannot proceed with zero-copy.\n";
        return;
    }
    
    std::cout << "Address verification: PASSED\n\n";
    
    // 检查 SharedValue 是否已准备好
    if (!handle.is_value_ready()) {
        std::cerr << "SharedValue not ready in shared memory!\n";
        return;
    }
    
    // 获取共享的 Value（零拷贝只读访问）
    const SharedValue* shared = handle.shared_value();
    if (!shared) {
        std::cerr << "Failed to get SharedValue pointer!\n";
        return;
    }
    
    std::cout << "SharedValue found in shared memory.\n";
    
    // 测量深拷贝性能
    Timer timer;
    timer.start();
    Value local = handle.copy_to_local();
    double copy_time = timer.elapsed_ms();
    
    std::cout << "Deep copy to local completed in " << std::fixed << std::setprecision(2) 
              << copy_time << " ms\n";
    
    // 显示数据摘要
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
// 辅助函数：遍历 SharedValue（模拟只读访问）
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
// 性能对比测试（4 种方案）
//==============================================================================

void performance_comparison() {
    constexpr size_t OBJECT_COUNT = 100000;  // 10万个对象
    
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
    // 方案 1：序列化/反序列化
    //==========================================================================
    std::cout << "=== Method 1: Serialization ===\n";
    {
        // 生成本地 Value
        Value data = generate_large_scene(OBJECT_COUNT);
        
        // 序列化
        timer.start();
        ByteBuffer buffer = serialize(data);
        serialize_time = timer.elapsed_ms();
        serialized_size = buffer.size();
        
        // 反序列化
        timer.start();
        Value deser = deserialize(buffer);
        deserialize_time = timer.elapsed_ms();
        
        std::cout << "  Serialize:   " << std::fixed << std::setprecision(2) << serialize_time << " ms\n";
        std::cout << "  Deserialize: " << deserialize_time << " ms\n";
        std::cout << "  Total:       " << (serialize_time + deserialize_time) << " ms\n";
        std::cout << "  Data size:   " << (serialized_size / 1024.0 / 1024.0) << " MB\n\n";
    }
    
    //==========================================================================
    // 方案 2：共享内存（2次拷贝：local -> shared -> local）
    //==========================================================================
    std::cout << "=== Method 2: SharedMem (2-copy) ===\n";
    {
        // 生成本地 Value
        Value data = generate_large_scene(OBJECT_COUNT);
        
        // 创建共享内存
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTest2", 1024 * 1024 * 1024)) {  // 1GB
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        
        shared_memory::set_current_shared_region(&region);
        
        // 深拷贝到共享内存
        timer.start();
        SharedValue shared = deep_copy_to_shared(data);
        deep_copy_to_shared_time = timer.elapsed_ms();
        
        // 深拷贝回本地
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
    // 方案 3：共享内存（1次拷贝：直接在共享内存构造 -> 拷贝到本地）
    //==========================================================================
    std::cout << "=== Method 3: SharedMem (Direct Build - 1-copy) ===\n";
    {
        // 创建共享内存
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTest3", 1024 * 1024 * 1024)) {  // 1GB
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        
        shared_memory::set_current_shared_region(&region);
        
        // 直接在共享内存上构造
        timer.start();
        SharedValue shared_direct = generate_large_scene_shared(OBJECT_COUNT);
        direct_build_time = timer.elapsed_ms();
        
        // 深拷贝回本地（这是 Editor 进程需要做的唯一操作）
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
    // 方案 4：共享内存（零拷贝：直接读取，不拷贝！）
    // 这是真正的零拷贝 - Editor 直接读取共享内存中的数据
    //==========================================================================
    std::cout << "=== Method 4: SharedMem (TRUE ZERO-COPY - Direct Read) ===\n";
    double direct_read_time = 0;
    size_t node_count = 0;
    {
        // 创建共享内存
        shared_memory::SharedMemoryRegion region;
        if (!region.create("PerfTest4", 1024 * 1024 * 1024)) {  // 1GB
            std::cerr << "Failed to create shared memory!\n";
            return;
        }
        
        shared_memory::set_current_shared_region(&region);
        
        // 直接在共享内存上构造（Engine 端）
        SharedValue shared_direct = generate_large_scene_shared(OBJECT_COUNT);
        
        // 零拷贝：Editor 直接遍历读取共享内存中的数据，不做任何拷贝！
        // 这模拟了 Editor 只读访问场景（例如在 UI 中显示属性）
        timer.start();
        node_count = traverse_shared_value(shared_direct);
        direct_read_time = timer.elapsed_ms();
        
        shared_memory::set_current_shared_region(nullptr);
        region.close();
        
        std::cout << "  Direct read (no copy!): " << std::fixed << std::setprecision(2) << direct_read_time << " ms\n";
        std::cout << "  Nodes traversed:        " << node_count << "\n\n";
    }
    
    //==========================================================================
    // 结果汇总
    //==========================================================================
    // 从 Editor 进程角度看的时间（不包含 Engine 构造数据的时间）
    double editor_m1 = deserialize_time;  // Editor 只需要反序列化
    double editor_m2 = deep_copy_to_local_time_m2;  // Editor 只需要 deep_copy_to_local
    double editor_m3 = deep_copy_to_local_time_m3;  // Editor 只需要 deep_copy_to_local
    double editor_m4 = direct_read_time;  // Editor 直接读取，零拷贝！
    
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
// 主函数
//==============================================================================

void print_usage() {
    std::cout << "Usage: shared_value_demo [command]\n";
    std::cout << "\nCommands:\n";
    std::cout << "  single     - Single process demo (default)\n";
    std::cout << "  publish N  - Run as publisher with N objects\n";
    std::cout << "  subscribe  - Run as subscriber\n";
    std::cout << "  perf       - Performance comparison\n";
    std::cout << "\nExamples:\n";
    std::cout << "  shared_value_demo single\n";
    std::cout << "  shared_value_demo publish 10000\n";
    std::cout << "  shared_value_demo subscribe\n";
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
    else {
        print_usage();
        return 1;
    }
    
    return 0;
}