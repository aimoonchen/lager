// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file fast_shared_value.h
/// @brief FastSharedValue - High Performance Shared Memory Value with Fake Transience
///
/// Differences from SharedValue:
/// - SharedValue: uses no_transience_policy, construction complexity O(n log n)
/// - FastSharedValue: uses fake_transience_policy, construction complexity O(n)
///
/// Performance characteristics:
/// - Allocation: O(1) bump allocator (same as SharedValue)
/// - Reference counting: no-op empty implementation (same as SharedValue)
/// - Locks: none (same as SharedValue)
/// - Transient: supported! but checks are empty (this is the key optimization)
///
/// Why is this design safe?
/// 1. No modification after construction -> no locks needed (no concurrent writes)
/// 2. Released as a whole -> no individual destruction (refcount can be no-op)
/// 3. Only need transient's build optimization -> don't need transient's "safety checks"
///
/// Use cases:
/// - Building large SharedValues (10,000+ elements)
/// - High performance requirements for construction

#pragma once

#include <lager_ext/shared_value.h>

//==============================================================================
// Fake Policies for FastSharedValue (Zero-overhead transient support)
//==============================================================================

namespace shared_memory {

// Fake reference counting policy - all operations are no-op
// Fully compatible with immer::no_refcount_policy
//
// All methods are marked noexcept to allow better compiler optimization
struct fake_refcount_policy {
    fake_refcount_policy() noexcept = default;
    fake_refcount_policy(immer::disowned) noexcept {}

    void inc() noexcept {}
    bool dec() noexcept { return false; }
    bool unique() noexcept { return false; }
};

// Fake transience policy - provides transient interface but performs no checks
//
// Core idea:
// - The real purpose of transient is to "mark as modifiable", avoiding unnecessary copies
// - In shared memory scenarios: no modification after construction, so no real "ownership check" needed
// - We only need transient's "in-place modification" semantics, not its "safety guarantees"
//
// This policy follows the immer transience policy interface:
// - Must have template<HeapPolicy> struct apply { struct type { ... }; };
// - type must have: edit, owner, ownee, noone
struct fake_transience_policy {
    template <typename HeapPolicy>
    struct apply {
        struct type {
            // edit type - used to identify editing permissions
            struct edit {
                bool operator==(edit) const noexcept { return true; }
                bool operator!=(edit) const noexcept { return false; }
            };

            // owner type - represents "modification permission owner"
            struct owner {
                owner() noexcept = default;
                owner(const owner&) noexcept = default;
                owner(owner&&) noexcept = default;
                owner& operator=(const owner&) noexcept = default;
                owner& operator=(owner&&) noexcept = default;

                operator edit() const noexcept { return {}; }
            };

            // ownee type - represents "managed object"
            struct ownee {
                ownee() noexcept = default;
                ownee(const ownee&) noexcept = default;
                ownee(ownee&&) noexcept = default;
                ownee& operator=(const ownee&) noexcept = default;
                ownee& operator=(ownee&&) noexcept = default;

                ownee& operator=(edit) noexcept { return *this; }

                // Key: can_mutate() always returns true, meaning "can be modified"
                bool can_mutate(edit) const noexcept { return true; }
                bool owned() const noexcept { return true; }
            };

            // Static noone member (empty owner)
            static owner noone;
        };
    };
};

// Static member definition
template <typename HP>
typename fake_transience_policy::apply<HP>::type::owner
    fake_transience_policy::apply<HP>::type::noone = {};

} // namespace shared_memory

//==============================================================================
// FastSharedValue Type Definitions
//==============================================================================

namespace lager_ext {

// Shared memory policy with transient support (zero-overhead version)
//
// Differences from shared_memory_policy:
// - shared_memory_policy: no_transience_policy -> does not support transient()
// - fast_shared_memory_policy: fake_transience_policy -> supports transient(), but no check overhead
using fast_shared_memory_policy = immer::memory_policy<
    immer::heap_policy<::shared_memory::shared_heap>,
    ::shared_memory::fake_refcount_policy,
    immer::no_lock_policy,
    ::shared_memory::fake_transience_policy,
    false,
    false
>;

//==============================================================================
// FastSharedValue Type Definitions
//==============================================================================

struct FastSharedValue;

using FastSharedValueBox    = immer::box<FastSharedValue, fast_shared_memory_policy>;
using FastSharedValueMap    = immer::map<::shared_memory::SharedString,
                                          FastSharedValueBox,
                                          ::shared_memory::SharedStringHash,
                                          ::shared_memory::SharedStringEqual,
                                          fast_shared_memory_policy>;
using FastSharedValueVector = immer::vector<FastSharedValueBox, fast_shared_memory_policy>;
using FastSharedValueArray  = immer::array<FastSharedValueBox, fast_shared_memory_policy>;

struct FastSharedTableEntry {
    ::shared_memory::SharedString id;
    FastSharedValueBox value;

    bool operator==(const FastSharedTableEntry& other) const {
        return id == other.id && value == other.value;
    }
    bool operator!=(const FastSharedTableEntry& other) const {
        return !(*this == other);
    }
};

struct FastSharedTableKeyFn {
    const ::shared_memory::SharedString& operator()(const FastSharedTableEntry& e) const {
        return e.id;
    }
};

using FastSharedValueTable = immer::table<FastSharedTableEntry,
                                           FastSharedTableKeyFn,
                                           ::shared_memory::SharedStringHash,
                                           ::shared_memory::SharedStringEqual,
                                           fast_shared_memory_policy>;

struct FastSharedValue {
    using string_type   = ::shared_memory::SharedString;
    using value_box     = FastSharedValueBox;
    using value_map     = FastSharedValueMap;
    using value_vector  = FastSharedValueVector;
    using value_array   = FastSharedValueArray;
    using value_table   = FastSharedValueTable;
    using table_entry   = FastSharedTableEntry;

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

    FastSharedValue() : data(std::monostate{}) {}
    FastSharedValue(int v) : data(v) {}
    FastSharedValue(int64_t v) : data(v) {}
    FastSharedValue(float v) : data(v) {}
    FastSharedValue(double v) : data(v) {}
    FastSharedValue(bool v) : data(v) {}
    FastSharedValue(const ::shared_memory::SharedString& v) : data(v) {}
    FastSharedValue(::shared_memory::SharedString&& v) : data(std::move(v)) {}
    FastSharedValue(const std::string& v) : data(::shared_memory::SharedString(v)) {}
    FastSharedValue(const char* v) : data(::shared_memory::SharedString(v)) {}
    FastSharedValue(value_map v) : data(std::move(v)) {}
    FastSharedValue(value_vector v) : data(std::move(v)) {}
    FastSharedValue(value_array v) : data(std::move(v)) {}
    FastSharedValue(value_table v) : data(std::move(v)) {}
    // Math type constructors
    FastSharedValue(Vec2 v) : data(v) {}
    FastSharedValue(Vec3 v) : data(v) {}
    FastSharedValue(Vec4 v) : data(v) {}
    FastSharedValue(Mat3 v) : data(v) {}
    FastSharedValue(Mat4x3 v) : data(v) {}
    FastSharedValue(Mat4 v) : data(v) {}

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

inline bool operator==(const FastSharedValue& a, const FastSharedValue& b) {
    return a.data == b.data;
}
inline bool operator!=(const FastSharedValue& a, const FastSharedValue& b) {
    return !(a == b);
}

//==============================================================================
// Deep Copy Functions: FastSharedValue <-> Value
//
// Provides two sets of interfaces:
// 1. Explicit naming: fast_deep_copy_to_local / fast_deep_copy_to_shared
// 2. Overloaded naming: deep_copy_to_local / deep_copy_to_shared (consistent with SharedValue interface)
//==============================================================================

Value fast_deep_copy_to_local(const FastSharedValue& shared);
FastSharedValue fast_deep_copy_to_shared(const Value& local);

// Overloaded version - consistent with SharedValue interface
inline Value deep_copy_to_local(const FastSharedValue& shared);
inline FastSharedValue deep_copy_to_shared_fast(const Value& local);

namespace detail {

// FastSharedValue -> Value (using transient optimization)
inline ValueBox copy_fast_shared_box_to_local(const FastSharedValueBox& shared_box) {
    return ValueBox{fast_deep_copy_to_local(shared_box.get())};
}

inline ValueMap copy_fast_shared_map_to_local(const FastSharedValueMap& shared_map) {
    auto transient = ValueMap{}.transient();
    for (const auto& [key, value_box] : shared_map) {
        transient.set(key.to_string(), copy_fast_shared_box_to_local(value_box));
    }
    return transient.persistent();
}

inline ValueVector copy_fast_shared_vector_to_local(const FastSharedValueVector& shared_vec) {
    auto transient = ValueVector{}.transient();
    for (const auto& value_box : shared_vec) {
        transient.push_back(copy_fast_shared_box_to_local(value_box));
    }
    return transient.persistent();
}

inline ValueArray copy_fast_shared_array_to_local(const FastSharedValueArray& shared_arr) {
    // Reserve exact capacity, avoiding reallocations
    std::vector<ValueBox> temp;
    temp.reserve(shared_arr.size());
    for (const auto& value_box : shared_arr) {
        temp.push_back(copy_fast_shared_box_to_local(value_box));
    }
    return ValueArray(temp.begin(), temp.end());
}

inline ValueTable copy_fast_shared_table_to_local(const FastSharedValueTable& shared_table) {
    auto transient = ValueTable{}.transient();
    for (const auto& entry : shared_table) {
        transient.insert(TableEntry{
            entry.id.to_string(),
            copy_fast_shared_box_to_local(entry.value)
        });
    }
    return transient.persistent();
}

// Value -> FastSharedValue (using transient optimization - this is the key optimization!)
inline FastSharedValueBox copy_local_box_to_fast_shared(const ValueBox& local_box) {
    return FastSharedValueBox{fast_deep_copy_to_shared(local_box.get())};
}

inline FastSharedValueMap copy_local_map_to_fast_shared(const ValueMap& local_map) {
    // Key optimization: using transient, O(n) complexity!
    auto transient = FastSharedValueMap{}.transient();
    for (const auto& [key, value_box] : local_map) {
        transient.set(
            ::shared_memory::SharedString(key),
            copy_local_box_to_fast_shared(value_box));
    }
    return transient.persistent();
}

inline FastSharedValueVector copy_local_vector_to_fast_shared(const ValueVector& local_vec) {
    // Key optimization: using transient, O(n) complexity!
    auto transient = FastSharedValueVector{}.transient();
    for (const auto& value_box : local_vec) {
        transient.push_back(copy_local_box_to_fast_shared(value_box));
    }
    return transient.persistent();
}

inline FastSharedValueArray copy_local_array_to_fast_shared(const ValueArray& local_arr) {
    // Note: immer::array's transient API returns a different type that cannot
    // be directly converted to FastSharedValueArray. Using move semantics instead.
    // This is still O(n) due to immer's structural sharing with move.
    FastSharedValueArray result;
    for (const auto& value_box : local_arr) {
        result = std::move(result).push_back(copy_local_box_to_fast_shared(value_box));
    }
    return result;
}

inline FastSharedValueTable copy_local_table_to_fast_shared(const ValueTable& local_table) {
    // Key optimization: using transient, O(n) complexity!
    auto transient = FastSharedValueTable{}.transient();
    for (const auto& entry : local_table) {
        transient.insert(FastSharedTableEntry{
            ::shared_memory::SharedString(entry.id),
            copy_local_box_to_fast_shared(entry.value)
        });
    }
    return transient.persistent();
}

} // namespace detail

inline Value fast_deep_copy_to_local(const FastSharedValue& shared) {
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
        else if constexpr (std::is_same_v<T, ::shared_memory::SharedString>) {
            return Value{data.to_string()};
        }
        else if constexpr (std::is_same_v<T, FastSharedValueMap>) {
            return Value{detail::copy_fast_shared_map_to_local(data)};
        }
        else if constexpr (std::is_same_v<T, FastSharedValueVector>) {
            return Value{detail::copy_fast_shared_vector_to_local(data)};
        }
        else if constexpr (std::is_same_v<T, FastSharedValueArray>) {
            return Value{detail::copy_fast_shared_array_to_local(data)};
        }
        else if constexpr (std::is_same_v<T, FastSharedValueTable>) {
            return Value{detail::copy_fast_shared_table_to_local(data)};
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

inline FastSharedValue fast_deep_copy_to_shared(const Value& local) {
    return std::visit([](const auto& data) -> FastSharedValue {
        using T = std::decay_t<decltype(data)>;

        if constexpr (std::is_same_v<T, std::monostate>) {
            return FastSharedValue{};
        }
        else if constexpr (std::is_same_v<T, int>) {
            return FastSharedValue{data};
        }
        else if constexpr (std::is_same_v<T, int64_t>) {
            return FastSharedValue{data};
        }
        else if constexpr (std::is_same_v<T, float>) {
            return FastSharedValue{data};
        }
        else if constexpr (std::is_same_v<T, double>) {
            return FastSharedValue{data};
        }
        else if constexpr (std::is_same_v<T, bool>) {
            return FastSharedValue{data};
        }
        else if constexpr (std::is_same_v<T, std::string>) {
            return FastSharedValue{::shared_memory::SharedString(data)};
        }
        else if constexpr (std::is_same_v<T, ValueMap>) {
            return FastSharedValue{detail::copy_local_map_to_fast_shared(data)};
        }
        else if constexpr (std::is_same_v<T, ValueVector>) {
            return FastSharedValue{detail::copy_local_vector_to_fast_shared(data)};
        }
        else if constexpr (std::is_same_v<T, ValueArray>) {
            return FastSharedValue{detail::copy_local_array_to_fast_shared(data)};
        }
        else if constexpr (std::is_same_v<T, ValueTable>) {
            return FastSharedValue{detail::copy_local_table_to_fast_shared(data)};
        }
        // Math types - trivially copyable, direct copy
        else if constexpr (std::is_same_v<T, Vec2>) {
            return FastSharedValue{data};
        }
        else if constexpr (std::is_same_v<T, Vec3>) {
            return FastSharedValue{data};
        }
        else if constexpr (std::is_same_v<T, Vec4>) {
            return FastSharedValue{data};
        }
        else if constexpr (std::is_same_v<T, Mat3>) {
            return FastSharedValue{data};
        }
        else if constexpr (std::is_same_v<T, Mat4x3>) {
            return FastSharedValue{data};
        }
        else if constexpr (std::is_same_v<T, Mat4>) {
            return FastSharedValue{data};
        }
        else {
            return FastSharedValue{};
        }
    }, local.data);
}

// Overloaded version implementation - consistent with SharedValue interface
inline Value deep_copy_to_local(const FastSharedValue& shared) {
    return fast_deep_copy_to_local(shared);
}

inline FastSharedValue deep_copy_to_shared_fast(const Value& local) {
    return fast_deep_copy_to_shared(local);
}

//==============================================================================
// FastSharedValueHandle - Handle for FastSharedValue
//==============================================================================

// Ensure FastSharedValue alignment is compatible with shared_heap
static_assert(alignof(FastSharedValue) <= ::shared_memory::shared_heap::ALIGNMENT,
    "FastSharedValue alignment must not exceed shared_heap::ALIGNMENT");

class FastSharedValueHandle {
public:
    FastSharedValueHandle() = default;
    ~FastSharedValueHandle() = default;

    // Non-copyable
    FastSharedValueHandle(const FastSharedValueHandle&) = delete;
    FastSharedValueHandle& operator=(const FastSharedValueHandle&) = delete;

    // Movable
    FastSharedValueHandle(FastSharedValueHandle&&) = default;
    FastSharedValueHandle& operator=(FastSharedValueHandle&&) = default;

    /// @brief Create shared memory and write Value (called by process B)
    ///
    /// @param name Shared memory region name (unique identifier)
    /// @param value The Value to copy to shared memory
    /// @param max_size Maximum size of shared memory region (default 100MB)
    /// @return true on success, false on failure
    ///
    /// Uses fast_deep_copy_to_shared for O(n) construction complexity.
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
            ::shared_memory::SharedMemoryRegion& region;
            bool success = false;
            ~RegionGuard() {
                ::shared_memory::set_current_shared_region(nullptr);
                if (!success) region.close();
            }
        } guard{region_};

        ::shared_memory::set_current_shared_region(&region_);

        try {
            void* value_storage = region_.allocate(sizeof(FastSharedValue), alignof(FastSharedValue));
            if (!value_storage) {
                last_error_ = "Failed to allocate storage for FastSharedValue";
                return false;
            }

            auto* header = region_.header();
            size_t offset = static_cast<char*>(value_storage) - static_cast<char*>(region_.base());
            header->value_offset = offset;

            // Using fast_deep_copy_to_shared - O(n) complexity!
            new (value_storage) FastSharedValue(fast_deep_copy_to_shared(value));

            // Sync local cursor to shared header (required for single-threaded allocator)
            region_.sync_allocation_cursor();

            guard.success = true;
            return true;
        }
        catch (const ::shared_memory::shared_memory_error& e) {
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

    // Get FastSharedValue (true zero-copy read-only access!)
    const FastSharedValue* shared_value() const noexcept {
        if (!region_.is_valid()) {
            return nullptr;
        }
        auto* header = region_.header();
        size_t offset = header->value_offset;
        if (offset == 0) {
            return nullptr;
        }
        return reinterpret_cast<const FastSharedValue*>(
            static_cast<char*>(region_.base()) + offset);
    }

    // Deep copy to local Value
    Value copy_to_local() const {
        const FastSharedValue* sv = shared_value();
        if (!sv) {
            return Value{};
        }
        return fast_deep_copy_to_local(*sv);
    }

    bool is_valid() const noexcept { return region_.is_valid(); }

    bool is_value_ready() const noexcept {
        if (!region_.is_valid()) return false;
        return region_.header()->value_offset != 0;
    }

    ::shared_memory::SharedMemoryRegion& region() noexcept { return region_; }
    const ::shared_memory::SharedMemoryRegion& region() const noexcept { return region_; }

    /// @brief Get the last error message (if any)
    /// @return Last error message, or empty string if no error
    const std::string& last_error() const noexcept { return last_error_; }

private:
    ::shared_memory::SharedMemoryRegion region_;
    std::string last_error_;
};

} // namespace lager_ext
