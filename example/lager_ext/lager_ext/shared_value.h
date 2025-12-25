// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file shared_value.h
/// @brief Shared memory Value type - supports zero-copy cross-process access.
///
/// Core concepts:
/// 1. Uses fixed address mapping to ensure both processes see the same virtual address
/// 2. Custom immer memory policy, all allocations are in shared memory
/// 3. After process B constructs, process A can directly copy to local memory
///
/// Type system overview:
///   - UnsafeValue      : Single-threaded high-performance version (non-atomic refcount)
///   - ThreadSafeValue  : Thread-safe version (atomic refcount, spinlock)
///   - SharedValue      : Shared memory version for cross-process access (defined in this file)
///
/// Convenient aliases:
///   - Value     = UnsafeValue     (default, for single-threaded use)
///   - SyncValue = ThreadSafeValue (for multi-threaded use)
///
/// SharedValue - Fully shared memory Value type:
///   - Uses SharedString, all data is in shared memory
///   - True zero-copy cross-process access
///   - Process A can directly read-only access, or use deep_copy_to_local() for deep copy
///
/// Main APIs:
///   - deep_copy_to_shared(Value) -> SharedValue  (Process B writes)
///   - deep_copy_to_local(SharedValue) -> Value   (Process A reads)
///   - SharedValueHandle - Convenient shared memory management handle

#pragma once

#include <lager_ext/value.h>

// Transient headers (deep_copy functions use transient for optimization)
#include <immer/map_transient.hpp>
#include <immer/vector_transient.hpp>
#include <immer/table_transient.hpp>

// Boost.Interprocess headers
#include <boost/interprocess/windows_shared_memory.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include <cstdint>
#include <cstddef>
#include <new>
#include <string>
#include <stdexcept>
#include <memory>
#include <functional>

namespace shared_memory {

//==============================================================================
// Shared Memory Region Management
//==============================================================================

// Shared memory header information
//
// Memory layout (64-bit systems):
// +--------+--------+----------------+------------+-------------+
// | magic  | version| fixed_base_addr| total_size | heap_offset |
// | 4 bytes| 4 bytes| 8 bytes        | 8 bytes    | 8 bytes     |
// +--------+--------+----------------+------------+-------------+
// | heap_size | heap_used | value_offset | padding |
// | 8 bytes   | 8 bytes   | 8 bytes      | 8 bytes |
// +--------+--------+----------------+------------+-------------+
// Total: 64 bytes (cache line aligned)
//
// NOTE: This header is designed for single-writer scenarios.
// Only one process should write to the shared memory at a time.
struct alignas(64) SharedMemoryHeader {
    uint32_t magic;                       // Magic number for validation
    uint32_t version;                     // Version number
    void*    fixed_base_address;          // Fixed mapping base address
    size_t   total_size;                  // Total size
    size_t   heap_offset;                 // Heap area start offset
    size_t   heap_size;                   // Heap area size
    size_t   heap_used;                   // Heap area used size
    size_t   value_offset;                // Value object offset (0 = uninitialized)
    uint64_t _padding;                    // Explicit padding for 64-byte alignment

    static constexpr uint32_t MAGIC = 0x53484D56; // "SHMV"
    static constexpr uint32_t CURRENT_VERSION = 1;
};

// Ensure header is exactly 64 bytes for cross-compiler compatibility
static_assert(sizeof(SharedMemoryHeader) == 64,
    "SharedMemoryHeader must be 64 bytes for cache line alignment");

// Shared memory region
class SharedMemoryRegion {
public:
    // Recommended fixed base address (choose a high address unlikely to be occupied)
    // Windows x64: User space is 0x00000000 - 0x7FFFFFFFFFFF
    static constexpr void* DEFAULT_BASE_ADDRESS = reinterpret_cast<void*>(0x0000600000000000ULL);

    SharedMemoryRegion() = default;
    ~SharedMemoryRegion() { close(); }

    // Non-copyable
    SharedMemoryRegion(const SharedMemoryRegion&) = delete;
    SharedMemoryRegion& operator=(const SharedMemoryRegion&) = delete;

    // Movable
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

    // Create new shared memory region (called by process B)
    bool create(const char* name, size_t size, void* base_address = DEFAULT_BASE_ADDRESS) {
        using namespace boost::interprocess;

        close();

        name_ = name;
        size_ = size;
        is_owner_ = true;

        try {
            shm_ = std::make_unique<windows_shared_memory>(
                create_only,
                name,
                read_write,
                size
            );

            // Try to map to fixed address
            region_ = std::make_unique<mapped_region>(
                *shm_,
                read_write,
                0,
                size,
                base_address
            );

            void* base = region_->get_address();
            if (!base) {
                close();
                return false;
            }

            // Initialize header
            auto* header = reinterpret_cast<SharedMemoryHeader*>(base);
            header->magic = SharedMemoryHeader::MAGIC;
            header->version = SharedMemoryHeader::CURRENT_VERSION;
            header->fixed_base_address = base;
            header->total_size = size;
            header->heap_offset = sizeof(SharedMemoryHeader);
            header->heap_size = size - sizeof(SharedMemoryHeader);
            header->heap_used = 0;
            header->value_offset = 0;

            return true;
        }
        catch (const interprocess_exception&) {
            close();
            return false;
        }
    }

    // Open existing shared memory region (called by process A)
    bool open(const char* name) {
        using namespace boost::interprocess;

        close();

        name_ = name;
        is_owner_ = false;

        try {
            shm_ = std::make_unique<windows_shared_memory>(
                open_only,
                name,
                read_write
            );

            // First map header to get fixed base address and size
            mapped_region temp_region(*shm_, read_only, 0, sizeof(SharedMemoryHeader));

            auto* temp_header = reinterpret_cast<SharedMemoryHeader*>(temp_region.get_address());
            if (temp_header->magic != SharedMemoryHeader::MAGIC) {
                close();
                return false;
            }

            void* fixed_base = temp_header->fixed_base_address;
            size_ = temp_header->total_size;

            // Try to map to the same fixed address
            region_ = std::make_unique<mapped_region>(
                *shm_,
                read_write,
                0,
                size_,
                fixed_base
            );

            void* base = region_->get_address();

            // Verify mapping address
            if (base != fixed_base) {
                close();
                return false;
            }

            return true;
        }
        catch (const interprocess_exception&) {
            close();
            return false;
        }
    }

    void close() {
        region_.reset();
        shm_.reset();
        size_ = 0;
        is_owner_ = false;
    }

    bool is_valid() const { return region_ && region_->get_address() != nullptr; }
    void* base() const { return region_ ? region_->get_address() : nullptr; }
    size_t size() const { return size_; }
    bool is_owner() const { return is_owner_; }

    SharedMemoryHeader* header() const {
        return reinterpret_cast<SharedMemoryHeader*>(base());
    }

    void* heap_base() const {
        void* b = base();
        if (!b) return nullptr;
        return reinterpret_cast<char*>(b) + header()->heap_offset;
    }

    // Allocate memory in heap area (single-threaded bump allocator)
    //
    // This allocator is designed for single-writer scenarios:
    // - Uses a local cursor for zero-overhead allocation
    // - No atomic operations or memory barriers
    // - Full compiler optimization across multiple allocations
    // - Call sync_allocation_cursor() after batch operations to persist state
    //
    // Performance: O(1) - just pointer arithmetic
    void* allocate(size_t size, size_t alignment = 8) {
        if (!base()) return nullptr;

        auto* h = header();
        size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);

        // Initialize local cursor from shared state if needed
        if (local_heap_cursor_ == 0) {
            local_heap_cursor_ = h->heap_used;
        }

        size_t current = local_heap_cursor_;
        size_t next = current + aligned_size;

        if (next > h->heap_size) {
            return nullptr;  // Out of memory
        }

        local_heap_cursor_ = next;
        return reinterpret_cast<char*>(heap_base()) + current;
    }

    // Sync local cursor back to shared header
    // Call this after a batch of allocations
    void sync_allocation_cursor() {
        if (local_heap_cursor_ > 0 && base()) {
            header()->heap_used = local_heap_cursor_;
        }
    }

    // Reset local cursor (call after sync or when starting fresh)
    void reset_local_cursor() {
        local_heap_cursor_ = 0;
    }

    // Get current local cursor value (for debugging/diagnostics)
    size_t local_cursor() const {
        return local_heap_cursor_;
    }

private:
    void swap(SharedMemoryRegion& other) noexcept {
        std::swap(shm_, other.shm_);
        std::swap(region_, other.region_);
        std::swap(size_, other.size_);
        std::swap(is_owner_, other.is_owner_);
        std::swap(name_, other.name_);
        std::swap(local_heap_cursor_, other.local_heap_cursor_);
    }

    std::unique_ptr<boost::interprocess::windows_shared_memory> shm_;
    std::unique_ptr<boost::interprocess::mapped_region> region_;
    size_t size_ = 0;
    bool is_owner_ = false;
    std::string name_;

    // Local cursor for allocation (zero overhead bump allocator)
    // This is NOT in shared memory - each process has its own cursor
    size_t local_heap_cursor_ = 0;
};

//==============================================================================
// Shared Memory Allocator (for immer)
//==============================================================================

// Thread-local shared memory region pointer
inline thread_local SharedMemoryRegion* g_current_shared_region = nullptr;

// Set current thread's shared memory region
inline void set_current_shared_region(SharedMemoryRegion* region) {
    g_current_shared_region = region;
}

//==============================================================================
// SharedString - Shared memory string type
//
// Features:
// - String data stored in shared memory
// - SSO (Small String Optimization): strings <= 15 bytes are stored inline
// - Immutable: cannot be modified after construction
// - Compatible with std::string interface (common methods)
//==============================================================================

class SharedString {
public:
    static constexpr size_t SSO_CAPACITY = 15;
    static constexpr size_t MAX_STRING_SIZE = 256 * 1024 * 1024; // 256 MB safety limit

    SharedString() noexcept : size_(0) {
        inline_data_[0] = '\0';
    }

    SharedString(const char* str) {
        if (!str) {
            size_ = 0;
            inline_data_[0] = '\0';
            return;
        }
        size_t len = std::strlen(str);
        if (len > MAX_STRING_SIZE) {
            throw std::length_error("SharedString: string too large (limit 256MB)");
        }
        init_from(str, len);
    }

    SharedString(const std::string& str) {
        if (str.size() > MAX_STRING_SIZE) {
            throw std::length_error("SharedString: string too large (limit 256MB)");
        }
        init_from(str.data(), str.size());
    }

    SharedString(const char* str, size_t len) {
        if (len > MAX_STRING_SIZE) {
            throw std::length_error("SharedString: string too large (limit 256MB)");
        }
        if (len > 0 && !str) {
            throw std::invalid_argument("SharedString: null pointer with non-zero length");
        }
        if (len == 0) {
            size_ = 0;
            inline_data_[0] = '\0';
            return;
        }
        init_from(str, len);
    }

    /// @brief Copy constructor
    ///
    /// **WARNING: Shallow copy semantics for heap-allocated strings!**
    ///
    /// When copying a heap-allocated SharedString outside of a shared memory context:
    /// - The new SharedString will reference the SAME data as the original
    /// - If the original's shared memory region is closed, the copy becomes INVALID
    /// - This is intentional for zero-copy read-only access in Process A
    ///
    /// Safe usage patterns:
    /// 1. Process A reads SharedValue from shared memory (shallow copy OK - read only)
    /// 2. Process B constructs SharedValue (deep copy - has shared memory context)
    /// 3. Use to_string() to get an independent std::string copy
    ///
    /// Unsafe patterns:
    /// - Storing a copied SharedString after closing the shared memory region
    /// - Modifying a shallow-copied SharedString (would corrupt shared memory)
    SharedString(const SharedString& other) {
        if (other.is_inline()) {
            // Inline strings can always be copied safely
            size_ = other.size_;
            std::memcpy(inline_data_, other.inline_data_, SSO_CAPACITY + 1);
        } else {
            // Heap strings need shared memory context for allocation
            if (!g_current_shared_region || !g_current_shared_region->is_valid()) {
                // No shared memory context - SHALLOW COPY (reference semantics)
                //
                // This is intentional for the following scenarios:
                // 1. Process A reading from shared memory (read-only access)
                // 2. The original SharedString's data remains valid as long as
                //    the shared memory region is open
                //
                // [WARNING] WARNING: The copied SharedString is NOT independently valid!
                //    It references the original's data in shared memory.
                //    DO NOT use after closing the shared memory region!
                size_ = other.size_;
                heap_data_ = other.heap_data_;
            } else {
                // In shared memory context - DEEP COPY (value semantics)
                init_from(other.data(), other.size());
            }
        }
    }

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

    SharedString& operator=(const SharedString& other) {
        if (this != &other) {
            SharedString temp(other);
            swap(temp);
        }
        return *this;
    }

    SharedString& operator=(SharedString&& other) noexcept {
        if (this != &other) {
            swap(other);
        }
        return *this;
    }

    // Destructor does nothing (using bump allocator)
    ~SharedString() = default;

    // Basic accessors
    const char* data() const noexcept {
        return is_inline() ? inline_data_ : heap_data_;
    }

    const char* c_str() const noexcept { return data(); }
    size_t size() const noexcept { return size_; }
    size_t length() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    const char* begin() const noexcept { return data(); }
    const char* end() const noexcept { return data() + size_; }

    char operator[](size_t pos) const noexcept { return data()[pos]; }
    char at(size_t pos) const {
        if (pos >= size_) throw std::out_of_range("SharedString::at");
        return data()[pos];
    }

    std::string to_string() const { return std::string(data(), size_); }
    operator std::string() const { return to_string(); }

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

    // Hash support (for immer::map keys)
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
            std::memcpy(inline_data_, str, len);
            inline_data_[len] = '\0';
        } else {
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
        size_t this_size = size_;
        size_t other_size = other.size_;
        bool this_inline = is_inline();
        bool other_inline = other.is_inline();

        std::swap(size_, other.size_);

        if (this_inline && other_inline) {
            char temp[SSO_CAPACITY + 1];
            std::memcpy(temp, inline_data_, SSO_CAPACITY + 1);
            std::memcpy(inline_data_, other.inline_data_, SSO_CAPACITY + 1);
            std::memcpy(other.inline_data_, temp, SSO_CAPACITY + 1);
        } else if (!this_inline && !other_inline) {
            std::swap(heap_data_, other.heap_data_);
        } else {
            if (this_inline) {
                char temp[SSO_CAPACITY + 1];
                std::memcpy(temp, inline_data_, this_size + 1);
                heap_data_ = other.heap_data_;
                std::memcpy(other.inline_data_, temp, this_size + 1);
            } else {
                char* temp_ptr = heap_data_;
                std::memcpy(inline_data_, other.inline_data_, other_size + 1);
                other.heap_data_ = temp_ptr;
            }
        }
    }

    size_t size_;
    union {
        char inline_data_[SSO_CAPACITY + 1];
        char* heap_data_;
    };
};

struct SharedStringHash {
    size_t operator()(const SharedString& s) const noexcept {
        return s.hash();
    }
};

struct SharedStringEqual {
    bool operator()(const SharedString& a, const SharedString& b) const noexcept {
        return a == b;
    }
};

} // namespace shared_memory

namespace std {
template <>
struct hash<shared_memory::SharedString> {
    size_t operator()(const shared_memory::SharedString& s) const noexcept {
        return s.hash();
    }
};
} // namespace std

//==============================================================================
// Shared Memory Heap - Conforms to immer heap interface
//
// A minimal bump allocator optimized for one-time construction:
// - Allocation: O(1), just pointer arithmetic
// - Deallocation: No-op (entire region is released together)
//==============================================================================

namespace shared_memory {

// Implements immer heap interface:
// - allocate(size_t) - basic allocation (single-threaded by default for max perf)
// - allocate(size_t, norefs_tag) - allocation for gc_transience_policy
// - deallocate(size_t, void*) - deallocation (no-op)
//
// DEFAULT: Uses single-threaded allocator (zero atomic overhead)
//
// IMPORTANT: After all allocations are done, call:
//   g_current_shared_region->sync_allocation_cursor();
// to persist the final cursor position to shared memory header.
// Custom exception for shared memory allocation failures (more informative than bad_alloc)
class shared_memory_error : public std::bad_alloc {
public:
    enum class error_type {
        no_region,          // g_current_shared_region is nullptr
        invalid_region,     // Region is closed or not properly initialized
        out_of_memory       // Heap exhausted
    };

    explicit shared_memory_error(error_type type, size_t requested = 0,
                                  size_t used = 0, size_t total = 0) noexcept
        : type_(type), requested_(requested), used_(used), total_(total) {}

    const char* what() const noexcept override {
        switch (type_) {
            case error_type::no_region:
                return "shared_heap: g_current_shared_region is nullptr. "
                       "Call set_current_shared_region() before using SharedValue.";
            case error_type::invalid_region:
                return "shared_heap: shared memory region is invalid (closed or uninitialized).";
            case error_type::out_of_memory:
                return "shared_heap: out of shared memory. Increase region size.";
            default:
                return "shared_heap: unknown error";
        }
    }

    error_type type() const noexcept { return type_; }
    size_t requested() const noexcept { return requested_; }
    size_t used() const noexcept { return used_; }
    size_t total() const noexcept { return total_; }

private:
    error_type type_;
    size_t requested_;
    size_t used_;
    size_t total_;
};

struct shared_heap {
    using type = shared_heap;

    // Alignment requirement for SharedValue (ensure all allocations meet this)
    static constexpr size_t ALIGNMENT = 16;

    // Allocate memory from shared memory region
    //
    // Uses the bump allocator which:
    // - Uses a local cursor (no atomic operations)
    // - Allows full compiler optimization
    // - Requires sync_allocation_cursor() after batch operations
    //
    // @throws shared_memory_error on failure (subclass of std::bad_alloc)
    static void* allocate(size_t size) {
        if (!g_current_shared_region) {
            throw shared_memory_error(shared_memory_error::error_type::no_region);
        }
        if (!g_current_shared_region->is_valid()) {
            throw shared_memory_error(shared_memory_error::error_type::invalid_region);
        }

        // Use bump allocator for maximum performance
        void* p = g_current_shared_region->allocate(size, ALIGNMENT);
        if (!p) {
            auto* h = g_current_shared_region->header();
            throw shared_memory_error(
                shared_memory_error::error_type::out_of_memory,
                size,
                g_current_shared_region->local_cursor(),
                h->heap_size
            );
        }
        return p;
    }

    // Allocation interface for gc_transience_policy
    static void* allocate(size_t size, immer::norefs_tag) {
        return allocate(size);
    }

    // Deallocate - no-op! Bump allocator doesn't support individual deallocation
    static void deallocate(size_t, void*) noexcept {
        // Entire shared memory region is released together
    }
};

} // namespace shared_memory

//==============================================================================
// SharedValue Type Definition
//
// Value type using shared memory allocator
//
// Design goal: Maximum performance for one-time construction
// - Heap policy: bump allocator (allocation is just pointer arithmetic)
// - Reference counting: no-op (no_refcount_policy)
// - Lock: none
// - Transience: none
//
// Important: Due to no_refcount_policy, SharedValue's lifetime is managed
// entirely by the shared memory region. All objects are destroyed when
// the region is closed.
//
// NOTE: Since no_transience_policy is used, SharedValue cannot use the
// transient() method. Deep copy functions must use regular immutable operations.
//==============================================================================

namespace lager_ext {

// Shared memory policy - maximum performance configuration (no refcount)
//
// Key optimizations:
// 1. shared_heap: bump allocator, allocation requires only one atomic add
// 2. no_refcount_policy: completely skip reference counting (max performance!)
// 3. no_lock_policy: no locks
// 4. no_transience_policy: no transience (shared memory scenario is one-time construction)
//
// Note: no_refcount_policy means objects won't be auto-released.
// This is exactly what we want because:
// - Process B: one-time construction, then entire region is released together
// - Process A: read-only access, then deep copy to local
using shared_memory_policy = immer::memory_policy<
    immer::heap_policy<shared_memory::shared_heap>,
    immer::no_refcount_policy,
    immer::no_lock_policy,
    immer::no_transience_policy,
    false,
    false
>;

//==============================================================================
// SharedValue Type - Fully shared memory Value (uses SharedString)
//
// All data including strings are entirely in shared memory,
// supporting true zero-copy cross-process access.
//
// Use cases:
// - Scenarios with large string data
// - Process A needs direct read-only access to strings (no deep copy needed)
//
// Limitations:
// - This is an independent type, not using BasicValue template
// - Does not have complete at()/set() methods (can be added as needed)
//==============================================================================

struct SharedValue;

using SharedValueBox    = immer::box<SharedValue, shared_memory_policy>;
using SharedValueMap    = immer::map<shared_memory::SharedString,
                                      SharedValueBox,
                                      shared_memory::SharedStringHash,
                                      shared_memory::SharedStringEqual,
                                      shared_memory_policy>;
using SharedValueVector = immer::vector<SharedValueBox, shared_memory_policy>;
using SharedValueArray  = immer::array<SharedValueBox, shared_memory_policy>;

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

struct SharedValue {
    using string_type   = shared_memory::SharedString;
    using value_box     = SharedValueBox;
    using value_map     = SharedValueMap;
    using value_vector  = SharedValueVector;
    using value_array   = SharedValueArray;
    using value_table   = SharedValueTable;
    using table_entry   = SharedTableEntry;

    // Math types (same as Value's math types - fixed-size, trivially copyable)
    using vec2_type     = Vec2;
    using vec3_type     = Vec3;
    using vec4_type     = Vec4;
    using mat3_type     = Mat3;
    using mat4x3_type   = Mat4x3;
    using mat4_type     = Mat4;

    std::variant<int,
                 int64_t,
                 float,
                 double,
                 bool,
                 ::shared_memory::SharedString,
                 value_map,
                 value_vector,
                 value_array,
                 value_table,
                 // Math types - trivially copyable, safe in shared memory
                 Vec2,
                 Vec3,
                 Vec4,
                 Mat3,
                 Mat4x3,
                 Mat4,
                 std::monostate>
        data;

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
    // Math type constructors
    SharedValue(Vec2 v) : data(v) {}
    SharedValue(Vec3 v) : data(v) {}
    SharedValue(Vec4 v) : data(v) {}
    SharedValue(Mat3 v) : data(v) {}
    SharedValue(Mat4x3 v) : data(v) {}
    SharedValue(Mat4 v) : data(v) {}

    template <typename T>
    const T* get_if() const { return std::get_if<T>(&data); }

    template <typename T>
    bool is() const { return std::holds_alternative<T>(data); }

    std::size_t type_index() const noexcept { return data.index(); }
    bool is_null() const noexcept { return std::holds_alternative<std::monostate>(data); }

    const ::shared_memory::SharedString* get_string() const {
        return std::get_if<::shared_memory::SharedString>(&data);
    }

    std::size_t size() const {
        if (auto* m = get_if<value_map>()) return m->size();
        if (auto* v = get_if<value_vector>()) return v->size();
        if (auto* a = get_if<value_array>()) return a->size();
        if (auto* t = get_if<value_table>()) return t->size();
        return 0;
    }
};

inline bool operator==(const SharedValue& a, const SharedValue& b) {
    return a.data == b.data;
}
inline bool operator!=(const SharedValue& a, const SharedValue& b) {
    return !(a == b);
}

//==============================================================================
// Deep Copy Functions: SharedValue <-> Value
//
// [WARNING] PERFORMANCE WARNING:
// deep_copy_to_shared() uses O(n log n) construction complexity because
// SharedValue uses no_transience_policy. This creates many intermediate
// tree nodes that are NOT reclaimed by the bump allocator, resulting in
// 2-3x higher memory usage than the final data size.
//
// For large data (>10,000 elements), consider using FastSharedValue and
// fast_deep_copy_to_shared() instead, which has O(n) complexity.
// See fast_shared_value.h for details.
//==============================================================================

Value deep_copy_to_local(const SharedValue& shared);
SharedValue deep_copy_to_shared(const Value& local);

namespace detail {

inline ValueBox copy_shared_box_to_local(const SharedValueBox& shared_box) {
    return ValueBox{deep_copy_to_local(shared_box.get())};
}

inline ValueMap copy_shared_map_to_local(const SharedValueMap& shared_map) {
    auto transient = ValueMap{}.transient();
    for (const auto& [key, value_box] : shared_map) {
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
    // Reserve exact capacity, avoiding reallocations
    std::vector<ValueBox> temp;
    temp.reserve(shared_arr.size());
    for (const auto& value_box : shared_arr) {
        temp.push_back(copy_shared_box_to_local(value_box));
    }
    return ValueArray(temp.begin(), temp.end());
}

inline ValueTable copy_shared_table_to_local(const SharedValueTable& shared_table) {
    auto transient = ValueTable{}.transient();
    for (const auto& entry : shared_table) {
        transient.insert(TableEntry{
            entry.id.to_string(),
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
            shared_memory::SharedString(entry.id),
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
            return Value{data.to_string()};
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
        // Math types - trivially copyable, direct copy
        else if constexpr (std::is_same_v<T, Vec2>) {
            return Value{data};
        }
        else if constexpr (std::is_same_v<T, Vec3>) {
            return Value{data};
        }
        else if constexpr (std::is_same_v<T, Vec4>) {
            return Value{data};
        }
        else if constexpr (std::is_same_v<T, Mat3>) {
            return Value{data};
        }
        else if constexpr (std::is_same_v<T, Mat4x3>) {
            return Value{data};
        }
        else if constexpr (std::is_same_v<T, Mat4>) {
            return Value{data};
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
        // Math types - trivially copyable, direct copy
        else if constexpr (std::is_same_v<T, Vec2>) {
            return SharedValue{data};
        }
        else if constexpr (std::is_same_v<T, Vec3>) {
            return SharedValue{data};
        }
        else if constexpr (std::is_same_v<T, Vec4>) {
            return SharedValue{data};
        }
        else if constexpr (std::is_same_v<T, Mat3>) {
            return SharedValue{data};
        }
        else if constexpr (std::is_same_v<T, Mat4x3>) {
            return SharedValue{data};
        }
        else if constexpr (std::is_same_v<T, Mat4>) {
            return SharedValue{data};
        }
        else {
            return SharedValue{};
        }
    }, local.data);
}

//==============================================================================
// SharedValueHandle - Handle for shared Value
//
// Encapsulates shared memory region and the SharedValue stored in it.
// Provides convenient creation and access interface.
//
// Important: SharedValue data is stored at a fixed position after the
// shared memory header, ensuring both processes can access it correctly.
//==============================================================================

// Ensure SharedValue alignment is compatible with shared_heap
static_assert(alignof(SharedValue) <= shared_memory::shared_heap::ALIGNMENT,
    "SharedValue alignment must not exceed shared_heap::ALIGNMENT");

class SharedValueHandle {
public:
    SharedValueHandle() = default;
    ~SharedValueHandle() = default;

    // Non-copyable
    SharedValueHandle(const SharedValueHandle&) = delete;
    SharedValueHandle& operator=(const SharedValueHandle&) = delete;

    // Movable
    SharedValueHandle(SharedValueHandle&&) = default;
    SharedValueHandle& operator=(SharedValueHandle&&) = default;

    /// @brief Create shared memory and write Value (called by process B)
    ///
    /// @param name Shared memory region name (unique identifier)
    /// @param value The Value to copy to shared memory
    /// @param max_size Maximum size of shared memory region (default 100MB)
    /// @return true on success, false on failure
    ///
    /// On failure, the region is cleaned up automatically.
    /// Use last_error() to get the last error message (if any).
    bool create(const char* name, const Value& value, size_t max_size = 100 * 1024 * 1024) {
        last_error_.clear();

        if (!region_.create(name, max_size)) {
            last_error_ = "Failed to create shared memory region";
            return false;
        }

        // RAII guard for cleanup on exception
        struct RegionGuard {
            shared_memory::SharedMemoryRegion& region;
            bool success = false;
            ~RegionGuard() {
                shared_memory::set_current_shared_region(nullptr);
                if (!success) region.close();
            }
        } guard{region_};

        shared_memory::set_current_shared_region(&region_);

        try {
            void* value_storage = region_.allocate(sizeof(SharedValue), alignof(SharedValue));
            if (!value_storage) {
                last_error_ = "Failed to allocate storage for SharedValue";
                return false;
            }

            auto* header = region_.header();
            size_t offset = static_cast<char*>(value_storage) - static_cast<char*>(region_.base());
            header->value_offset = offset;

            new (value_storage) SharedValue(deep_copy_to_shared(value));

            // Sync local cursor to shared header (required for single-threaded allocator)
            region_.sync_allocation_cursor();

            guard.success = true;
            return true;
        }
        catch (const shared_memory::shared_memory_error& e) {
            last_error_ = e.what();
            return false;
        }
        catch (const std::exception& e) {
            last_error_ = e.what();
            return false;
        }
    }

    // Open shared memory (called by process A)
    bool open(const char* name) {
        return region_.open(name);
    }

    // Get shared Value (true zero-copy read-only access!)
    // Note: Must be called after successful open()
    const SharedValue* shared_value() const noexcept {
        if (!region_.is_valid()) {
            return nullptr;
        }
        auto* header = region_.header();
        size_t offset = header->value_offset;
        if (offset == 0) {
            return nullptr;
        }
        return reinterpret_cast<const SharedValue*>(
            static_cast<char*>(region_.base()) + offset);
    }

    // Deep copy to local Value
    Value copy_to_local() const {
        const SharedValue* sv = shared_value();
        if (!sv) {
            return Value{};
        }
        return deep_copy_to_local(*sv);
    }

    bool is_valid() const noexcept { return region_.is_valid(); }

    // Check if Value has been initialized
    bool is_value_ready() const noexcept {
        if (!region_.is_valid()) return false;
        return region_.header()->value_offset != 0;
    }

    shared_memory::SharedMemoryRegion& region() noexcept { return region_; }
    const shared_memory::SharedMemoryRegion& region() const noexcept { return region_; }

    /// @brief Get the last error message (if any)
    /// @return Last error message, or empty string if no error
    const std::string& last_error() const noexcept { return last_error_; }

private:
    shared_memory::SharedMemoryRegion region_;
    std::string last_error_;
};

} // namespace lager_ext
