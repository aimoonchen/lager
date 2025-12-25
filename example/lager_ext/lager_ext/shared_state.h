// Copyright (c) 2024 chenmou. All rights reserved.
// Licensed under the MIT License. See LICENSE file in the project root.

/// @file shared_state.h
/// @brief Cross-process state sharing for lager/immer applications.
///
/// This module provides StatePublisher and StateSubscriber for sharing
/// immer-based state across process boundaries using shared memory.
///
/// Architecture:
/// - Main process owns the lager store and maintains full immer structure sharing
/// - Child processes receive serialized state updates via shared memory
/// - Supports both full state and incremental (diff) updates
///
/// Thread Safety:
/// - StatePublisher is NOT thread-safe (use from single thread)
/// - StateSubscriber is thread-safe for reading
/// - Cross-process synchronization uses platform-specific primitives

#pragma once

#include <lager_ext/value.h>
#include <lager_ext/diff_collector.h>

#include <functional>
#include <memory>
#include <optional>
#include <atomic>
#include <string>
#include <chrono>

namespace lager_ext {

// ============================================================
// StateUpdate - Represents a state change notification
// ============================================================
struct StateUpdate {
    enum class Type : uint8_t {
        Full = 0,      // Complete state snapshot
        Diff = 1       // Incremental changes only
    };

    Type type;
    ByteBuffer data;      // Serialized state or diff data
    uint64_t version;     // Monotonically increasing version number
    uint64_t timestamp;   // Unix timestamp in milliseconds
};

// ============================================================
// SharedMemoryConfig - Configuration for shared memory
// ============================================================
struct SharedMemoryConfig {
    std::string name;              // Shared memory name (e.g., "my_app_state")
    std::size_t size = 64 * 1024;  // Size in bytes (default: 64KB)
    bool create = true;            // Create if not exists (for publisher)

    // Performance tuning
    std::chrono::milliseconds poll_interval{10};  // For subscriber polling
    std::size_t max_history = 100;                // Max diff history to keep
};

// ============================================================
// StatePublisher - Main process state broadcaster
//
// Usage:
//   StatePublisher publisher({"my_app_state", 1024*1024});
//   publisher.publish(current_state);
//   // or for incremental updates:
//   publisher.publish_diff(old_state, new_state);
// ============================================================
class StatePublisher {
public:
    explicit StatePublisher(const SharedMemoryConfig& config);
    ~StatePublisher();

    // Non-copyable, movable
    StatePublisher(const StatePublisher&) = delete;
    StatePublisher& operator=(const StatePublisher&) = delete;
    StatePublisher(StatePublisher&&) noexcept;
    StatePublisher& operator=(StatePublisher&&) noexcept;

    // Publish complete state (recommended for initial state)
    void publish(const Value& state);

    // Publish incremental diff (recommended for updates)
    // Returns true if diff was published, false if full state was published
    // (full state is published when diff would be larger)
    bool publish_diff(const Value& old_state, const Value& new_state);

    // Force publish full state even if diff might be smaller
    void publish_full(const Value& state);

    // Get current version number
    [[nodiscard]] uint64_t version() const noexcept;

    // Get statistics
    struct Stats {
        uint64_t total_publishes = 0;
        uint64_t full_publishes = 0;
        uint64_t diff_publishes = 0;
        std::size_t total_bytes_written = 0;
        std::size_t last_update_size = 0;
    };
    [[nodiscard]] Stats stats() const noexcept;

    // Check if shared memory is valid
    [[nodiscard]] bool is_valid() const noexcept;

    // Explicitly release shared memory resources
    // Useful for graceful shutdown before process exit
    void close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================
// StateSubscriber - Child process state receiver
//
// Usage:
//   StateSubscriber subscriber({"my_app_state"});
//
//   // Blocking wait:
//   Value state = subscriber.wait_for_update();
//
//   // Or with callback:
//   subscriber.on_update([](const Value& state) {
//       // Handle new state
//   });
// ============================================================
class StateSubscriber {
public:
    explicit StateSubscriber(const SharedMemoryConfig& config);
    ~StateSubscriber();

    // Non-copyable, movable
    StateSubscriber(const StateSubscriber&) = delete;
    StateSubscriber& operator=(const StateSubscriber&) = delete;
    StateSubscriber(StateSubscriber&&) noexcept;
    StateSubscriber& operator=(StateSubscriber&&) noexcept;

    // Get current state (returns cached state, does not wait)
    [[nodiscard]] const Value& current() const noexcept;

    // Get current version
    [[nodiscard]] uint64_t version() const noexcept;

    // Check for updates (non-blocking)
    // Returns true if state was updated
    bool poll();

    // Try to get update without blocking
    // Returns null Value if no update available
    [[nodiscard]] Value try_get_update();

    // Wait for next update (blocking)
    // Returns the new state
    // timeout: max time to wait (0 = infinite)
    [[nodiscard]] Value wait_for_update(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    // Register callback for state updates
    // Callback is invoked when poll() detects an update
    using UpdateCallback = std::function<void(const Value& new_state, uint64_t version)>;
    void on_update(UpdateCallback callback);

    // Start background polling thread
    // Callbacks will be invoked from the background thread
    void start_polling();

    // Stop background polling thread
    void stop_polling();

    // Check if polling is active
    [[nodiscard]] bool is_polling() const noexcept;

    // Get statistics
    struct Stats {
        uint64_t total_updates = 0;
        uint64_t full_updates = 0;
        uint64_t diff_updates = 0;
        std::size_t total_bytes_read = 0;
        uint64_t missed_updates = 0;  // Updates that were overwritten before reading
    };
    [[nodiscard]] Stats stats() const noexcept;

    // Check if shared memory is valid
    [[nodiscard]] bool is_valid() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ============================================================
// DiffResult - Structured diff for cross-process transfer
//
// Unlike DiffEntry (human-readable strings), DiffResult stores
// actual Value objects for efficient binary serialization.
// ============================================================
struct ModifiedEntry {
    Path path;
    Value old_value;
    Value new_value;
};

struct DiffResult {
    std::vector<std::pair<Path, Value>> added;    // path -> new value
    std::vector<std::pair<Path, Value>> removed;  // path -> old value (optional)
    std::vector<ModifiedEntry> modified;           // path + old/new values

    [[nodiscard]] bool empty() const {
        return added.empty() && removed.empty() && modified.empty();
    }
};

// ============================================================
// Utility functions
// ============================================================

// Collect diff between two Values (returns structured diff)
DiffResult collect_diff(const Value& old_val, const Value& new_val);

// Encode diff changes to binary format for transmission
ByteBuffer encode_diff(const DiffResult& diff);

// Decode diff changes from binary format
DiffResult decode_diff(const ByteBuffer& data);

// Apply diff to a Value, returning the new Value
Value apply_diff(const Value& base, const DiffResult& diff);

// ============================================================
// Demo function
// ============================================================

// Demonstrates cross-process state sharing (single-process simulation)
void demo_shared_state();

} // namespace lager_ext
