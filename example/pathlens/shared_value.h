// shared_value.h
// 共享内存 Value 类型 - 支持跨进程零拷贝访问
// 
// 核心思路：
// 1. 使用固定地址映射，确保两个进程看到相同的虚拟地址
// 2. 自定义 immer 内存策略，所有分配都在共享内存上
// 3. B 进程构造完成后，A 进程可以直接拷贝到本地内存
//
// ============================================================
// 共享内存 Value 类型
// 
// 类型体系概览：
//   - Value       : 线程安全版本（default_memory_policy，原子引用计数）
//   - LocalValue  : 单线程高性能版本（非原子引用计数，无锁）
//   - SharedValue : 共享内存版本（本文件定义）
//
// SharedValue - 完全共享内存的 Value 类型
//   - 使用 SharedString，所有数据都在共享内存中
//   - 真正的零拷贝跨进程访问
//   - A 进程可以直接只读访问，或使用 deep_copy_to_local() 深拷贝
//
// 主要 API：
//   - deep_copy_to_shared(Value) -> SharedValue  (B进程写入)
//   - deep_copy_to_local(SharedValue) -> Value   (A进程读取)
//   - SharedValueHandle - 便捷的共享内存管理句柄
// ============================================================

#pragma once

#include "value.h"

// immer transient 头文件（deep_copy 函数使用 transient 优化需要）
#include <immer/map_transient.hpp>
#include <immer/vector_transient.hpp>
#include <immer/table_transient.hpp>

// 避免 Windows.h 宏冲突
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <cstdint>
#include <cstddef>
#include <new>
#include <atomic>
#include <string>
#include <stdexcept>
#include <memory>
#include <functional>

namespace shared_memory {

//==============================================================================
// 共享内存区域管理
//==============================================================================

// 共享内存头部信息
struct SharedMemoryHeader {
    uint32_t magic;              // 魔数，用于验证
    uint32_t version;            // 版本号
    void*    fixed_base_address; // 固定的映射基址
    size_t   total_size;         // 总大小
    size_t   heap_offset;        // 堆区起始偏移
    size_t   heap_size;          // 堆区大小
    std::atomic<size_t> heap_used;   // 堆区已使用大小
    std::atomic<size_t> value_offset; // Value 对象的偏移（0 表示未初始化）
    
    static constexpr uint32_t MAGIC = 0x53484D56; // "SHMV"
    static constexpr uint32_t CURRENT_VERSION = 1;
};

// 共享内存区域
class SharedMemoryRegion {
public:
    // 推荐的固定基址（选择一个不太可能被占用的地址）
    // Windows x64: 用户空间是 0x00000000 - 0x7FFFFFFFFFFF
    // 选择一个高地址区域
    static constexpr void* DEFAULT_BASE_ADDRESS = reinterpret_cast<void*>(0x0000600000000000ULL);
    
    SharedMemoryRegion() = default;
    ~SharedMemoryRegion() { close(); }
    
    // 禁止拷贝
    SharedMemoryRegion(const SharedMemoryRegion&) = delete;
    SharedMemoryRegion& operator=(const SharedMemoryRegion&) = delete;
    
    // 允许移动
    SharedMemoryRegion(SharedMemoryRegion&& other) noexcept {
        swap(other);
    }
    SharedMemoryRegion& operator=(SharedMemoryRegion&& other) noexcept {
        if (this != &other) {
            close();
            swap(other);
        }
        return *this;
    }
    
    // 创建新的共享内存区域（B 进程调用）
    bool create(const char* name, size_t size, void* base_address = DEFAULT_BASE_ADDRESS) {
        close();
        
        name_ = name;
        size_ = size;
        is_owner_ = true;
        
        // 创建文件映射对象
        h_map_ = CreateFileMappingA(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            static_cast<DWORD>(size >> 32),
            static_cast<DWORD>(size & 0xFFFFFFFF),
            name);
        
        if (!h_map_) {
            return false;
        }
        
        // 尝试映射到固定地址
        base_ = MapViewOfFileEx(
            h_map_,
            FILE_MAP_ALL_ACCESS,
            0, 0,
            size,
            base_address);
        
        if (!base_) {
            // 固定地址映射失败，尝试让系统分配
            base_ = MapViewOfFile(h_map_, FILE_MAP_ALL_ACCESS, 0, 0, size);
            if (!base_) {
                CloseHandle(h_map_);
                h_map_ = nullptr;
                return false;
            }
        }
        
        // 初始化头部
        auto* header = reinterpret_cast<SharedMemoryHeader*>(base_);
        header->magic = SharedMemoryHeader::MAGIC;
        header->version = SharedMemoryHeader::CURRENT_VERSION;
        header->fixed_base_address = base_;
        header->total_size = size;
        header->heap_offset = sizeof(SharedMemoryHeader);
        header->heap_size = size - sizeof(SharedMemoryHeader);
        header->heap_used.store(0, std::memory_order_relaxed);
        header->value_offset.store(0, std::memory_order_relaxed);
        
        return true;
    }
    
    // 打开已存在的共享内存区域（A 进程调用）
    bool open(const char* name) {
        close();
        
        name_ = name;
        is_owner_ = false;
        
        // 打开文件映射对象
        h_map_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (!h_map_) {
            return false;
        }
        
        // 首先映射头部以获取固定基址
        void* temp_base = MapViewOfFile(h_map_, FILE_MAP_READ, 0, 0, sizeof(SharedMemoryHeader));
        if (!temp_base) {
            CloseHandle(h_map_);
            h_map_ = nullptr;
            return false;
        }
        
        auto* temp_header = reinterpret_cast<SharedMemoryHeader*>(temp_base);
        if (temp_header->magic != SharedMemoryHeader::MAGIC) {
            UnmapViewOfFile(temp_base);
            CloseHandle(h_map_);
            h_map_ = nullptr;
            return false;
        }
        
        void* fixed_base = temp_header->fixed_base_address;
        size_ = temp_header->total_size;
        UnmapViewOfFile(temp_base);
        
        // 尝试映射到相同的固定地址
        base_ = MapViewOfFileEx(
            h_map_,
            FILE_MAP_ALL_ACCESS,
            0, 0,
            size_,
            fixed_base);
        
        if (!base_) {
            // 固定地址映射失败
            // 这是关键问题！如果失败，指针将无效
            CloseHandle(h_map_);
            h_map_ = nullptr;
            return false;
        }
        
        // 验证映射地址
        if (base_ != fixed_base) {
            UnmapViewOfFile(base_);
            CloseHandle(h_map_);
            base_ = nullptr;
            h_map_ = nullptr;
            return false;
        }
        
        return true;
    }
    
    void close() {
        if (base_) {
            UnmapViewOfFile(base_);
            base_ = nullptr;
        }
        if (h_map_) {
            CloseHandle(h_map_);
            h_map_ = nullptr;
        }
        size_ = 0;
        is_owner_ = false;
    }
    
    bool is_valid() const { return base_ != nullptr; }
    void* base() const { return base_; }
    size_t size() const { return size_; }
    bool is_owner() const { return is_owner_; }
    
    SharedMemoryHeader* header() const {
        return reinterpret_cast<SharedMemoryHeader*>(base_);
    }
    
    // 获取堆区起始地址
    void* heap_base() const {
        if (!base_) return nullptr;
        return reinterpret_cast<char*>(base_) + header()->heap_offset;
    }
    
    // 在堆区分配内存（简单的 bump allocator）
    void* allocate(size_t size, size_t alignment = 8) {
        if (!base_) return nullptr;
        
        auto* h = header();
        size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
        
        size_t old_used = h->heap_used.fetch_add(aligned_size, std::memory_order_relaxed);
        if (old_used + aligned_size > h->heap_size) {
            h->heap_used.fetch_sub(aligned_size, std::memory_order_relaxed);
            return nullptr; // 内存不足
        }
        
        return reinterpret_cast<char*>(heap_base()) + old_used;
    }
    
private:
    void swap(SharedMemoryRegion& other) noexcept {
        std::swap(h_map_, other.h_map_);
        std::swap(base_, other.base_);
        std::swap(size_, other.size_);
        std::swap(is_owner_, other.is_owner_);
        std::swap(name_, other.name_);
    }
    
    HANDLE h_map_ = nullptr;
    void* base_ = nullptr;
    size_t size_ = 0;
    bool is_owner_ = false;
    std::string name_;
};

//==============================================================================
// 共享内存分配器（用于 immer）
//==============================================================================

// 全局共享内存区域指针（线程局部存储）
inline thread_local SharedMemoryRegion* g_current_shared_region = nullptr;

// 设置当前线程的共享内存区域
inline void set_current_shared_region(SharedMemoryRegion* region) {
    g_current_shared_region = region;
}

//==============================================================================
// SharedString - 共享内存字符串类型
//
// 用于 FullSharedValue，实现真正的跨进程零拷贝字符串访问
//
// 设计特点：
// - 字符数据存储在共享内存中
// - SSO（小字符串优化）：小于等于 15 字节的字符串内联存储
// - 不可变：一旦构造就不能修改
// - 与 std::string 接口兼容（常用方法）
//==============================================================================

class SharedString {
public:
    // 最大内联长度（SSO）
    static constexpr size_t SSO_CAPACITY = 15;
    
    // 默认构造：空字符串
    SharedString() noexcept : size_(0) {
        inline_data_[0] = '\0';
    }
    
    // 从 C 字符串构造
    SharedString(const char* str) {
        if (!str) {
            size_ = 0;
            inline_data_[0] = '\0';
            return;
        }
        init_from(str, std::strlen(str));
    }
    
    // 从 std::string 构造
    SharedString(const std::string& str) {
        init_from(str.data(), str.size());
    }
    
    // 从 C 字符串和长度构造
    SharedString(const char* str, size_t len) {
        init_from(str, len);
    }
    
    // 拷贝构造
    SharedString(const SharedString& other) {
        if (other.is_inline()) {
            size_ = other.size_;
            std::memcpy(inline_data_, other.inline_data_, SSO_CAPACITY + 1);
        } else {
            // 对于堆分配的字符串，在共享内存中创建新副本
            init_from(other.data(), other.size());
        }
    }
    
    // 移动构造
    SharedString(SharedString&& other) noexcept {
        size_ = other.size_;
        if (other.is_inline()) {
            std::memcpy(inline_data_, other.inline_data_, SSO_CAPACITY + 1);
        } else {
            heap_data_ = other.heap_data_;
            other.size_ = 0;
            other.inline_data_[0] = '\0';
        }
    }
    
    // 拷贝赋值
    SharedString& operator=(const SharedString& other) {
        if (this != &other) {
            SharedString temp(other);
            swap(temp);
        }
        return *this;
    }
    
    // 移动赋值
    SharedString& operator=(SharedString&& other) noexcept {
        if (this != &other) {
            swap(other);
        }
        return *this;
    }
    
    // 析构（共享内存版本什么都不做，因为使用 bump allocator）
    ~SharedString() = default;
    
    // 基本访问
    const char* data() const noexcept {
        return is_inline() ? inline_data_ : heap_data_;
    }
    
    const char* c_str() const noexcept { return data(); }
    size_t size() const noexcept { return size_; }
    size_t length() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    
    // 迭代器
    const char* begin() const noexcept { return data(); }
    const char* end() const noexcept { return data() + size_; }
    
    // 索引访问
    char operator[](size_t pos) const noexcept { return data()[pos]; }
    char at(size_t pos) const {
        if (pos >= size_) throw std::out_of_range("SharedString::at");
        return data()[pos];
    }
    
    // 转换为 std::string
    std::string to_string() const { return std::string(data(), size_); }
    operator std::string() const { return to_string(); }
    
    // 比较运算符
    bool operator==(const SharedString& other) const noexcept {
        if (size_ != other.size_) return false;
        return std::memcmp(data(), other.data(), size_) == 0;
    }
    
    bool operator!=(const SharedString& other) const noexcept {
        return !(*this == other);
    }
    
    bool operator<(const SharedString& other) const noexcept {
        int cmp = std::memcmp(data(), other.data(), 
                              size_ < other.size_ ? size_ : other.size_);
        if (cmp != 0) return cmp < 0;
        return size_ < other.size_;
    }
    
    bool operator==(const std::string& other) const noexcept {
        if (size_ != other.size()) return false;
        return std::memcmp(data(), other.data(), size_) == 0;
    }
    
    bool operator==(const char* other) const noexcept {
        return std::strcmp(data(), other ? other : "") == 0;
    }
    
    // 哈希支持（用于 immer::map 的键）
    size_t hash() const noexcept {
        // FNV-1a hash
        size_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < size_; ++i) {
            hash ^= static_cast<unsigned char>(data()[i]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

private:
    bool is_inline() const noexcept {
        return size_ <= SSO_CAPACITY;
    }
    
    void init_from(const char* str, size_t len) {
        size_ = len;
        if (len <= SSO_CAPACITY) {
            // 内联存储
            std::memcpy(inline_data_, str, len);
            inline_data_[len] = '\0';
        } else {
            // 需要在共享内存中分配
            if (!g_current_shared_region || !g_current_shared_region->is_valid()) {
                throw std::bad_alloc();
            }
            char* buf = static_cast<char*>(g_current_shared_region->allocate(len + 1, 1));
            if (!buf) {
                throw std::bad_alloc();
            }
            std::memcpy(buf, str, len);
            buf[len] = '\0';
            heap_data_ = buf;
        }
    }
    
    void swap(SharedString& other) noexcept {
        // 保存当前状态
        size_t this_size = size_;
        size_t other_size = other.size_;
        bool this_inline = is_inline();
        bool other_inline = other.is_inline();
        
        // 交换 size
        std::swap(size_, other.size_);
        
        // 根据状态交换数据
        if (this_inline && other_inline) {
            // 两者都是内联：直接交换内联数据
            char temp[SSO_CAPACITY + 1];
            std::memcpy(temp, inline_data_, SSO_CAPACITY + 1);
            std::memcpy(inline_data_, other.inline_data_, SSO_CAPACITY + 1);
            std::memcpy(other.inline_data_, temp, SSO_CAPACITY + 1);
        } else if (!this_inline && !other_inline) {
            // 两者都是堆：直接交换指针
            std::swap(heap_data_, other.heap_data_);
        } else {
            // 一个内联一个堆：需要特殊处理
            if (this_inline) {
                // this 是内联，other 是堆
                char temp[SSO_CAPACITY + 1];
                std::memcpy(temp, inline_data_, this_size + 1);
                heap_data_ = other.heap_data_;
                std::memcpy(other.inline_data_, temp, this_size + 1);
            } else {
                // this 是堆，other 是内联
                char* temp_ptr = heap_data_;
                std::memcpy(inline_data_, other.inline_data_, other_size + 1);
                other.heap_data_ = temp_ptr;
            }
        }
    }
    
    size_t size_;
    union {
        char inline_data_[SSO_CAPACITY + 1];  // +1 for null terminator
        char* heap_data_;
    };
};

// SharedString 的哈希函数（用于 immer::map 等）
struct SharedStringHash {
    size_t operator()(const SharedString& s) const noexcept {
        return s.hash();
    }
};

// SharedString 的相等函数
struct SharedStringEqual {
    bool operator()(const SharedString& a, const SharedString& b) const noexcept {
        return a == b;
    }
};

} // namespace shared_memory

// std::hash 特化（用于标准库容器）
namespace std {
template <>
struct hash<shared_memory::SharedString> {
    size_t operator()(const shared_memory::SharedString& s) const noexcept {
        return s.hash();
    }
};
} // namespace std

//==============================================================================
// 使用示例和说明
//==============================================================================
/*

使用方法：

1. B 进程（创建者）：
   
   shared_memory::SharedMemoryRegion region;
   if (!region.create("MySharedValue", 1024 * 1024 * 100)) { // 100MB
       // 创建失败处理
   }
   
   // 设置当前共享区域
   shared_memory::set_current_shared_region(&region);
   
   // 构造 Value（需要 immer 使用自定义内存策略）
   // ... 构造逻辑 ...
   
   // 通知 A 进程数据已准备好
   
2. A 进程（读取者）：
   
   shared_memory::SharedMemoryRegion region;
   if (!region.open("MySharedValue")) {
       // 如果固定地址映射失败，需要回退到序列化方案
   }
   
   // 现在可以直接读取 Value！
   // 因为地址相同，所有指针都有效
   
   // 拷贝到本地内存进行状态管理
   Value local_copy = deep_copy(shared_value);

注意事项：
- 固定地址映射可能失败（地址被占用）
- 需要准备回退方案（序列化）
- 共享内存大小需要预先估计
- Bump allocator 不支持单独释放内存

正确的使用流程：

┌─────────────────────────────────────────────────────────────┐
│ B 进程（Engine）- 直接在共享内存上构造                        │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   // 1. 创建共享内存区域                                     │
│   SharedMemoryRegion region;                                │
│   region.create("SceneState", 256 * 1024 * 1024);           │
│                                                             │
│   // 2. 设置当前线程使用共享内存分配器                        │
│   set_current_shared_region(&region);                       │
│                                                             │
│   // 3. 直接在共享内存上构造 SharedValue！                   │
│   //    不需要 deep_copy_to_shared()                        │
│   SharedValueMap scene;                                     │
│   scene = scene.set("name", SharedValueBox{SharedValue{"MyScene"}});  │
│                                                             │
│   SharedValueVector objects;                                │
│   for (int i = 0; i < 100000; ++i) {                        │
│       SharedValueMap obj;                                   │
│       obj = obj.set("id", SharedValueBox{SharedValue{i}});  │
│       objects = objects.push_back(SharedValueBox{SharedValue{obj}});  │
│   }                                                         │
│   scene = scene.set("objects", SharedValueBox{SharedValue{objects}}); │
│                                                             │
│   // 4. 存储根对象指针到共享内存头部                          │
│   store_root_value(region, scene);                          │
│                                                             │
│   // 5. 清除当前线程的共享内存设置                            │
│   set_current_shared_region(nullptr);                       │
│                                                             │
│   // 6. 通知 A 进程数据就绪                                  │
│   notify_ready();                                           │
│                                                             │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│ A 进程（Editor）- 深拷贝到本地                               │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   // 1. 打开共享内存                                         │
│   SharedMemoryRegion region;                                │
│   region.open("SceneState");                                │
│                                                             │
│   // 2. 获取根对象（指针在共享内存中有效！）                   │
│   const SharedValue* root = get_root_value(region);         │
│                                                             │
│   // 3. 深拷贝到本地进程内存                                 │
│   Value local_state = deep_copy_to_local(*root);            │
│                                                             │
│   // 4. 关闭共享内存（可选，也可以保持打开用于后续同步）       │
│   region.close();                                           │
│                                                             │
│   // 5. 使用本地 Value 进行状态管理                          │
│   auto store = lager::make_store<Action>(                   │
│       EditorModel{local_state}, ...);                       │
│                                                             │
└─────────────────────────────────────────────────────────────┘

关键点：
- B 进程：直接使用 SharedValue 类型构造，不需要转换
- A 进程：使用 deep_copy_to_local() 拷贝到本地 Value
- 性能：B 进程零拷贝构造，A 进程一次性遍历拷贝

*/

//==============================================================================
// 共享内存堆 - 符合 immer heap 接口
// 
// 这是一个极简的 bump allocator，专为一次性构造场景优化：
// - 分配：O(1)，只需指针加法
// - 释放：无操作（整个区域一起释放）
//==============================================================================

namespace shared_memory {

// 共享内存堆（直接使用，不需要 heap_policy 包装）
// 
// 实现 immer heap 接口，包括：
// - allocate(size_t) - 基本分配
// - allocate(size_t, norefs_tag) - gc_transience_policy 需要的分配
// - deallocate(size_t, void*) - 释放（无操作）
struct shared_heap {
    // 类型定义 - immer heap 接口要求
    using type = shared_heap;
    
    // 分配内存 - 极快！只是指针加法
    static void* allocate(size_t size) {
        if (!g_current_shared_region || !g_current_shared_region->is_valid()) {
            throw std::bad_alloc();
        }
        void* p = g_current_shared_region->allocate(size, 16); // 16字节对齐
        if (!p) {
            throw std::bad_alloc();
        }
        return p;
    }
    
    // gc_transience_policy 需要的分配接口
    // norefs_tag 表示分配的内存不需要引用计数跟踪
    static void* allocate(size_t size, immer::norefs_tag) {
        return allocate(size);  // 对于 bump allocator，两者相同
    }
    
    // 释放内存 - 无操作！
    static void deallocate(size_t, void*) {
        // bump allocator 不支持单独释放
        // 整个共享内存区域会一起释放
    }
};

} // namespace shared_memory

//==============================================================================
// SharedValue 类型定义
// 
// 使用共享内存分配器的 Value 类型
// 
// 设计目标：最高性能的一次性构造
// - 堆策略：bump allocator（分配只需指针加法）
// - 引用计数：无操作（no_refcount_policy）
// - 锁：无
// - Transience：无
//
// 重要：由于使用 no_refcount_policy，SharedValue 的生命周期
// 完全由共享内存区域管理。区域关闭时所有对象一起销毁。
//
// ⚠️ 重要限制：std::string 成员不在共享内存上！
// 
// SharedValue 的 data 成员中的 std::string 使用进程本地堆分配，
// 因此在跨进程访问时字符串内容是无效的。
//
// 正确使用方法：
// 1. B 进程构造 SharedValue（此时 std::string 在 B 进程堆上）
// 2. A 进程打开共享内存后，立即调用 deep_copy_to_local() 
//    将数据复制到本地进程的 Value（此时 string 被正确复制）
// 3. A 进程不应该直接访问 SharedValue 中的 std::string！
//
// 如果需要真正的零拷贝字符串，需要使用自定义的共享内存字符串类型。
//==============================================================================

namespace immer_lens {

// 共享内存的内存策略 - 最高性能配置（无引用计数版本）
// 
// 关键优化：
// 1. shared_heap: bump allocator，分配只需一次原子加法
// 2. no_refcount_policy: 完全跳过引用计数（最大性能提升！）
// 3. no_lock_policy: 无锁
// 4. no_transience_policy: 不使用 transience（共享内存场景是一次性构造）
//
// 注意：no_refcount_policy 意味着对象不会被自动释放
// 这正是我们想要的！因为：
// - B 进程：一次性构造，然后整个区域一起释放
// - A 进程：只读访问，然后深拷贝到本地
//
// 注意：由于使用 no_transience_policy，SharedValue 类型不能使用 transient()
// 方法。深拷贝函数需要使用普通的不可变操作。
using shared_memory_policy = immer::memory_policy<
    immer::heap_policy<shared_memory::shared_heap>,
    immer::no_refcount_policy,      // 无引用计数！最高性能
    immer::no_lock_policy,          // 无锁
    immer::no_transience_policy,    // 不使用 transience（与自定义堆兼容）
    false,                          // 不使用 prefer_fewer_bigger_objects
    false                           // 不使用 use_transient_rvalues
>;

//==============================================================================
// SharedValue 类型 - 完全共享内存的 Value（使用 SharedString）
//
// 该类型的所有数据（包括字符串）都完全在共享内存中，
// 支持真正的零拷贝跨进程访问。
//
// 使用场景：
// - 字符串数据量很大的场景
// - 需要 A 进程直接只读访问字符串（不需要深拷贝）
//
// 限制：
// - 这是一个独立的类型，不使用 BasicValue 模板
// - 没有完整的 at()/set() 方法（可按需添加）
//==============================================================================

// 前向声明
struct SharedValue;

// 容器类型别名（使用 SharedString 作为键）
using SharedValueBox    = immer::box<SharedValue, shared_memory_policy>;
using SharedValueMap    = immer::map<shared_memory::SharedString, 
                                      SharedValueBox,
                                      shared_memory::SharedStringHash,
                                      shared_memory::SharedStringEqual,
                                      shared_memory_policy>;
using SharedValueVector = immer::vector<SharedValueBox, shared_memory_policy>;
using SharedValueArray  = immer::array<SharedValueBox, shared_memory_policy>;

// Table entry 使用 SharedString 作为 id
struct SharedTableEntry {
    shared_memory::SharedString id;
    SharedValueBox value;
    
    bool operator==(const SharedTableEntry& other) const {
        return id == other.id && value == other.value;
    }
    bool operator!=(const SharedTableEntry& other) const {
        return !(*this == other);
    }
};

// 自定义 table_key_fn 用于 SharedTableEntry
struct SharedTableKeyFn {
    const shared_memory::SharedString& operator()(const SharedTableEntry& e) const {
        return e.id;
    }
};

using SharedValueTable = immer::table<SharedTableEntry,
                                       SharedTableKeyFn,
                                       shared_memory::SharedStringHash,
                                       shared_memory::SharedStringEqual,
                                       shared_memory_policy>;

// 共享内存的 Value 结构（所有数据包括字符串都在共享内存中）
struct SharedValue {
    using string_type   = shared_memory::SharedString;
    using value_box     = SharedValueBox;
    using value_map     = SharedValueMap;
    using value_vector  = SharedValueVector;
    using value_array   = SharedValueArray;
    using value_table   = SharedValueTable;
    using table_entry   = SharedTableEntry;
    
    std::variant<int,
                 int64_t,
                 float,
                 double,
                 bool,
                 shared_memory::SharedString,  // 使用 SharedString！
                 value_map,
                 value_vector,
                 value_array,
                 value_table,
                 std::monostate>
        data;

    // 构造函数
    SharedValue() : data(std::monostate{}) {}
    SharedValue(int v) : data(v) {}
    SharedValue(int64_t v) : data(v) {}
    SharedValue(float v) : data(v) {}
    SharedValue(double v) : data(v) {}
    SharedValue(bool v) : data(v) {}
    SharedValue(const shared_memory::SharedString& v) : data(v) {}
    SharedValue(shared_memory::SharedString&& v) : data(std::move(v)) {}
    SharedValue(const std::string& v) : data(shared_memory::SharedString(v)) {}
    SharedValue(const char* v) : data(shared_memory::SharedString(v)) {}
    SharedValue(value_map v) : data(std::move(v)) {}
    SharedValue(value_vector v) : data(std::move(v)) {}
    SharedValue(value_array v) : data(std::move(v)) {}
    SharedValue(value_table v) : data(std::move(v)) {}

    // 类型检查
    template <typename T>
    const T* get_if() const { return std::get_if<T>(&data); }
    
    template <typename T>
    bool is() const { return std::holds_alternative<T>(data); }
    
    std::size_t type_index() const noexcept { return data.index(); }
    bool is_null() const noexcept { return std::holds_alternative<std::monostate>(data); }
    
    // 获取字符串值
    const shared_memory::SharedString* get_string() const {
        return std::get_if<shared_memory::SharedString>(&data);
    }
    
    // Size 方法（针对容器类型）
    std::size_t size() const {
        if (auto* m = get_if<value_map>()) return m->size();
        if (auto* v = get_if<value_vector>()) return v->size();
        if (auto* a = get_if<value_array>()) return a->size();
        if (auto* t = get_if<value_table>()) return t->size();
        return 0;
    }
};

// 比较运算符
inline bool operator==(const SharedValue& a, const SharedValue& b) {
    return a.data == b.data;
}
inline bool operator!=(const SharedValue& a, const SharedValue& b) {
    return !(a == b);
}

//==============================================================================
// 深拷贝函数：SharedValue <-> Value
//==============================================================================

// 前向声明
Value deep_copy_to_local(const SharedValue& shared);
SharedValue deep_copy_to_shared(const Value& local);

namespace detail {

inline ValueBox copy_shared_box_to_local(const SharedValueBox& shared_box) {
    return ValueBox{deep_copy_to_local(shared_box.get())};
}

inline ValueMap copy_shared_map_to_local(const SharedValueMap& shared_map) {
    auto transient = ValueMap{}.transient();
    for (const auto& [key, value_box] : shared_map) {
        // SharedString -> std::string
        transient.set(key.to_string(), copy_shared_box_to_local(value_box));
    }
    return transient.persistent();
}

inline ValueVector copy_shared_vector_to_local(const SharedValueVector& shared_vec) {
    auto transient = ValueVector{}.transient();
    for (const auto& value_box : shared_vec) {
        transient.push_back(copy_shared_box_to_local(value_box));
    }
    return transient.persistent();
}

inline ValueArray copy_shared_array_to_local(const SharedValueArray& shared_arr) {
    ValueArray result;
    for (const auto& value_box : shared_arr) {
        result = std::move(result).push_back(copy_shared_box_to_local(value_box));
    }
    return result;
}

inline ValueTable copy_shared_table_to_local(const SharedValueTable& shared_table) {
    auto transient = ValueTable{}.transient();
    for (const auto& entry : shared_table) {
        transient.insert(TableEntry{
            entry.id.to_string(),  // SharedString -> std::string
            copy_shared_box_to_local(entry.value)
        });
    }
    return transient.persistent();
}

inline SharedValueBox copy_local_box_to_shared(const ValueBox& local_box) {
    return SharedValueBox{deep_copy_to_shared(local_box.get())};
}

inline SharedValueMap copy_local_map_to_shared(const ValueMap& local_map) {
    SharedValueMap result;
    for (const auto& [key, value_box] : local_map) {
        // std::string -> SharedString（会在共享内存中分配）
        result = std::move(result).set(
            shared_memory::SharedString(key), 
            copy_local_box_to_shared(value_box));
    }
    return result;
}

inline SharedValueVector copy_local_vector_to_shared(const ValueVector& local_vec) {
    SharedValueVector result;
    for (const auto& value_box : local_vec) {
        result = std::move(result).push_back(copy_local_box_to_shared(value_box));
    }
    return result;
}

inline SharedValueArray copy_local_array_to_shared(const ValueArray& local_arr) {
    SharedValueArray result;
    for (const auto& value_box : local_arr) {
        result = std::move(result).push_back(copy_local_box_to_shared(value_box));
    }
    return result;
}

inline SharedValueTable copy_local_table_to_shared(const ValueTable& local_table) {
    SharedValueTable result;
    for (const auto& entry : local_table) {
        result = std::move(result).insert(SharedTableEntry{
            shared_memory::SharedString(entry.id),  // std::string -> SharedString
            copy_local_box_to_shared(entry.value)
        });
    }
    return result;
}

} // namespace detail

inline Value deep_copy_to_local(const SharedValue& shared) {
    return std::visit([](const auto& data) -> Value {
        using T = std::decay_t<decltype(data)>;
        
        if constexpr (std::is_same_v<T, std::monostate>) {
            return Value{};
        }
        else if constexpr (std::is_same_v<T, int>) {
            return Value{data};
        }
        else if constexpr (std::is_same_v<T, int64_t>) {
            return Value{data};
        }
        else if constexpr (std::is_same_v<T, float>) {
            return Value{data};
        }
        else if constexpr (std::is_same_v<T, double>) {
            return Value{data};
        }
        else if constexpr (std::is_same_v<T, bool>) {
            return Value{data};
        }
        else if constexpr (std::is_same_v<T, shared_memory::SharedString>) {
            return Value{data.to_string()};  // SharedString -> std::string
        }
        else if constexpr (std::is_same_v<T, SharedValueMap>) {
            return Value{detail::copy_shared_map_to_local(data)};
        }
        else if constexpr (std::is_same_v<T, SharedValueVector>) {
            return Value{detail::copy_shared_vector_to_local(data)};
        }
        else if constexpr (std::is_same_v<T, SharedValueArray>) {
            return Value{detail::copy_shared_array_to_local(data)};
        }
        else if constexpr (std::is_same_v<T, SharedValueTable>) {
            return Value{detail::copy_shared_table_to_local(data)};
        }
        else {
            return Value{};
        }
    }, shared.data);
}

inline SharedValue deep_copy_to_shared(const Value& local) {
    return std::visit([](const auto& data) -> SharedValue {
        using T = std::decay_t<decltype(data)>;
        
        if constexpr (std::is_same_v<T, std::monostate>) {
            return SharedValue{};
        }
        else if constexpr (std::is_same_v<T, int>) {
            return SharedValue{data};
        }
        else if constexpr (std::is_same_v<T, int64_t>) {
            return SharedValue{data};
        }
        else if constexpr (std::is_same_v<T, float>) {
            return SharedValue{data};
        }
        else if constexpr (std::is_same_v<T, double>) {
            return SharedValue{data};
        }
        else if constexpr (std::is_same_v<T, bool>) {
            return SharedValue{data};
        }
        else if constexpr (std::is_same_v<T, std::string>) {
            // std::string -> SharedString（在共享内存中分配）
            return SharedValue{shared_memory::SharedString(data)};
        }
        else if constexpr (std::is_same_v<T, ValueMap>) {
            return SharedValue{detail::copy_local_map_to_shared(data)};
        }
        else if constexpr (std::is_same_v<T, ValueVector>) {
            return SharedValue{detail::copy_local_vector_to_shared(data)};
        }
        else if constexpr (std::is_same_v<T, ValueArray>) {
            return SharedValue{detail::copy_local_array_to_shared(data)};
        }
        else if constexpr (std::is_same_v<T, ValueTable>) {
            return SharedValue{detail::copy_local_table_to_shared(data)};
        }
        else {
            return SharedValue{};
        }
    }, local.data);
}

//==============================================================================
// SharedValueHandle - 共享 Value 的句柄
// 
// 封装共享内存区域和其中存储的 SharedValue
// 提供便捷的创建和访问接口
//
// 重要：SharedValue 数据存储在共享内存头部之后的固定位置，
// 确保两个进程都能正确访问。
//==============================================================================

class SharedValueHandle {
public:
    SharedValueHandle() = default;
    ~SharedValueHandle() = default;
    
    // 禁止拷贝
    SharedValueHandle(const SharedValueHandle&) = delete;
    SharedValueHandle& operator=(const SharedValueHandle&) = delete;
    
    // 允许移动
    SharedValueHandle(SharedValueHandle&&) = default;
    SharedValueHandle& operator=(SharedValueHandle&&) = default;
    
    // 创建共享内存并写入 Value（B 进程调用）
    bool create(const char* name, const Value& value, size_t max_size = 100 * 1024 * 1024) {
        if (!region_.create(name, max_size)) {
            return false;
        }
        
        shared_memory::set_current_shared_region(&region_);
        
        try {
            // 在共享内存中分配 SharedValue 的存储空间
            void* value_storage = region_.allocate(sizeof(SharedValue), alignof(SharedValue));
            if (!value_storage) {
                shared_memory::set_current_shared_region(nullptr);
                region_.close();
                return false;
            }
            
            // 记录偏移量到共享内存头部
            auto* header = region_.header();
            size_t offset = static_cast<char*>(value_storage) - static_cast<char*>(region_.base());
            header->value_offset.store(offset, std::memory_order_release);
            
            // 使用 placement new 在共享内存中构造
            new (value_storage) SharedValue(deep_copy_to_shared(value));
            
            shared_memory::set_current_shared_region(nullptr);
            return true;
        }
        catch (const std::bad_alloc&) {
            shared_memory::set_current_shared_region(nullptr);
            region_.close();
            return false;
        }
    }
    
    // 打开共享内存（A 进程调用）
    bool open(const char* name) {
        return region_.open(name);
    }
    
    // 获取共享的 Value（真正的零拷贝只读访问！）
    // 注意：必须在 open() 成功后调用
    const SharedValue* shared_value() const noexcept {
        if (!region_.is_valid()) {
            return nullptr;
        }
        auto* header = region_.header();
        size_t offset = header->value_offset.load(std::memory_order_acquire);
        if (offset == 0) {
            return nullptr;
        }
        return reinterpret_cast<const SharedValue*>(
            static_cast<char*>(region_.base()) + offset);
    }
    
    // 深拷贝到本地 Value
    Value copy_to_local() const {
        const SharedValue* sv = shared_value();
        if (!sv) {
            return Value{};
        }
        return deep_copy_to_local(*sv);
    }
    
    bool is_valid() const noexcept { return region_.is_valid(); }
    
    // 检查 Value 是否已初始化
    bool is_value_ready() const noexcept {
        if (!region_.is_valid()) return false;
        return region_.header()->value_offset.load(std::memory_order_acquire) != 0;
    }
    
    shared_memory::SharedMemoryRegion& region() noexcept { return region_; }
    const shared_memory::SharedMemoryRegion& region() const noexcept { return region_; }
    
private:
    shared_memory::SharedMemoryRegion region_;
};

} // namespace immer_lens
