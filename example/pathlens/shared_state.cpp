// shared_state.cpp
// Implementation of cross-process state sharing
//
// This implementation uses Windows shared memory (CreateFileMapping/MapViewOfFile)
// For cross-platform support, consider using Boost.Interprocess

#include "shared_state.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <semaphore.h>
#endif

#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <iostream>

namespace immer_lens {

// ============================================================
// Shared Memory Header Layout
// ============================================================
// Offset  Size    Field
// 0       8       magic (0x494D4D4552535453 = "IMMERSST")
// 8       8       version (monotonically increasing)
// 16      8       timestamp (unix ms)
// 24      1       update_type (0=full, 1=diff)
// 25      3       reserved
// 28      4       data_size
// 32      N       data (serialized Value or diff)
// ============================================================

static constexpr uint64_t SHARED_MEMORY_MAGIC = 0x494D4D4552535453ULL; // "IMMERSST"
static constexpr std::size_t HEADER_SIZE = 32;

struct SharedMemoryHeader {
    uint64_t magic;
    uint64_t version;
    uint64_t timestamp;
    uint8_t update_type;
    uint8_t reserved[3];
    uint32_t data_size;
};

static_assert(sizeof(SharedMemoryHeader) == HEADER_SIZE, "Header size mismatch");

// ============================================================
// Platform-specific shared memory implementation
// ============================================================

#ifdef _WIN32

class SharedMemoryRegion {
public:
    SharedMemoryRegion(const std::string& name, std::size_t size, bool create)
        : size_(size)
        , is_creator_(create)
    {
        std::wstring wide_name(name.begin(), name.end());
        wide_name = L"Local\\" + wide_name;
        
        if (create) {
            handle_ = CreateFileMappingW(
                INVALID_HANDLE_VALUE,
                nullptr,
                PAGE_READWRITE,
                static_cast<DWORD>(size >> 32),
                static_cast<DWORD>(size & 0xFFFFFFFF),
                wide_name.c_str()
            );
        } else {
            handle_ = OpenFileMappingW(
                FILE_MAP_READ,
                FALSE,
                wide_name.c_str()
            );
        }
        
        if (!handle_) {
            std::cerr << "[SharedMemory] Failed to " << (create ? "create" : "open") 
                      << " shared memory: " << GetLastError() << "\n";
            return;
        }
        
        data_ = MapViewOfFile(
            handle_,
            create ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ,
            0, 0, size
        );
        
        if (!data_) {
            std::cerr << "[SharedMemory] Failed to map view: " << GetLastError() << "\n";
            CloseHandle(handle_);
            handle_ = nullptr;
            return;
        }
        
        // Initialize header if creator
        if (create) {
            auto* header = reinterpret_cast<SharedMemoryHeader*>(data_);
            header->magic = SHARED_MEMORY_MAGIC;
            header->version = 0;
            header->timestamp = 0;
            header->update_type = 0;
            header->data_size = 0;
        }
    }
    
    ~SharedMemoryRegion() {
        if (data_) {
            UnmapViewOfFile(data_);
        }
        if (handle_) {
            CloseHandle(handle_);
        }
    }
    
    bool is_valid() const noexcept { return data_ != nullptr; }
    void* data() noexcept { return data_; }
    const void* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    
private:
    HANDLE handle_ = nullptr;
    void* data_ = nullptr;
    std::size_t size_;
    bool is_creator_;
};

#else // POSIX

class SharedMemoryRegion {
public:
    SharedMemoryRegion(const std::string& name, std::size_t size, bool create)
        : name_("/" + name)
        , size_(size)
        , is_creator_(create)
    {
        int flags = create ? (O_CREAT | O_RDWR) : O_RDONLY;
        fd_ = shm_open(name_.c_str(), flags, 0666);
        
        if (fd_ < 0) {
            std::cerr << "[SharedMemory] Failed to open: " << strerror(errno) << "\n";
            return;
        }
        
        if (create) {
            if (ftruncate(fd_, size) < 0) {
                std::cerr << "[SharedMemory] Failed to resize: " << strerror(errno) << "\n";
                close(fd_);
                fd_ = -1;
                return;
            }
        }
        
        int prot = create ? (PROT_READ | PROT_WRITE) : PROT_READ;
        data_ = mmap(nullptr, size, prot, MAP_SHARED, fd_, 0);
        
        if (data_ == MAP_FAILED) {
            std::cerr << "[SharedMemory] Failed to mmap: " << strerror(errno) << "\n";
            close(fd_);
            fd_ = -1;
            data_ = nullptr;
            return;
        }
        
        // Initialize header if creator
        if (create) {
            auto* header = reinterpret_cast<SharedMemoryHeader*>(data_);
            header->magic = SHARED_MEMORY_MAGIC;
            header->version = 0;
            header->timestamp = 0;
            header->update_type = 0;
            header->data_size = 0;
        }
    }
    
    ~SharedMemoryRegion() {
        if (data_) {
            munmap(data_, size_);
        }
        if (fd_ >= 0) {
            close(fd_);
        }
        if (is_creator_) {
            shm_unlink(name_.c_str());
        }
    }
    
    bool is_valid() const noexcept { return data_ != nullptr; }
    void* data() noexcept { return data_; }
    const void* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    
private:
    std::string name_;
    int fd_ = -1;
    void* data_ = nullptr;
    std::size_t size_;
    bool is_creator_;
};

#endif

// ============================================================
// Helper: Get current timestamp in milliseconds
// ============================================================
static uint64_t current_timestamp_ms() {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

// ============================================================
// StatePublisher Implementation
// ============================================================

struct StatePublisher::Impl {
    SharedMemoryRegion shm;
    SharedMemoryConfig config;
    Stats stats;
    Value last_state;
    
    Impl(const SharedMemoryConfig& cfg)
        : shm(cfg.name, cfg.size, true)
        , config(cfg)
    {}
    
    void write_update(StateUpdate::Type type, const ByteBuffer& data) {
        if (!shm.is_valid()) return;
        
        auto* header = reinterpret_cast<SharedMemoryHeader*>(shm.data());
        
        // Check size
        if (HEADER_SIZE + data.size() > shm.size()) {
            std::cerr << "[StatePublisher] Data too large for shared memory: "
                      << data.size() << " > " << (shm.size() - HEADER_SIZE) << "\n";
            return;
        }
        
        // Write data first (before updating header)
        uint8_t* data_ptr = reinterpret_cast<uint8_t*>(shm.data()) + HEADER_SIZE;
        std::memcpy(data_ptr, data.data(), data.size());
        
        // Memory barrier to ensure data is written before header update
#ifdef _WIN32
        MemoryBarrier();
#else
        __sync_synchronize();
#endif
        
        // Update header atomically
        header->data_size = static_cast<uint32_t>(data.size());
        header->update_type = static_cast<uint8_t>(type);
        header->timestamp = current_timestamp_ms();
        header->version++;
        
        // Update stats
        stats.total_publishes++;
        if (type == StateUpdate::Type::Full) {
            stats.full_publishes++;
        } else {
            stats.diff_publishes++;
        }
        stats.total_bytes_written += data.size();
        stats.last_update_size = data.size();
    }
};

StatePublisher::StatePublisher(const SharedMemoryConfig& config)
    : impl_(std::make_unique<Impl>(config))
{}

StatePublisher::~StatePublisher() = default;

StatePublisher::StatePublisher(StatePublisher&&) noexcept = default;
StatePublisher& StatePublisher::operator=(StatePublisher&&) noexcept = default;

void StatePublisher::publish(const Value& state) {
    publish_full(state);
}

bool StatePublisher::publish_diff(const Value& old_state, const Value& new_state) {
    if (!impl_->shm.is_valid()) return false;
    
    // Collect diff
    DiffResult diff = collect_diff(old_state, new_state);
    
    // If no changes, don't publish
    if (diff.added.empty() && diff.removed.empty() && diff.modified.empty()) {
        return true;  // No update needed
    }
    
    // Encode diff
    ByteBuffer diff_data = encode_diff(diff);
    
    // Serialize full state for comparison
    ByteBuffer full_data = serialize(new_state);
    
    // Use diff if it's smaller than full state
    if (diff_data.size() < full_data.size()) {
        impl_->write_update(StateUpdate::Type::Diff, diff_data);
        impl_->last_state = new_state;
        return true;
    } else {
        impl_->write_update(StateUpdate::Type::Full, full_data);
        impl_->last_state = new_state;
        return false;
    }
}

void StatePublisher::publish_full(const Value& state) {
    if (!impl_->shm.is_valid()) return;
    
    ByteBuffer data = serialize(state);
    impl_->write_update(StateUpdate::Type::Full, data);
    impl_->last_state = state;
}

uint64_t StatePublisher::version() const noexcept {
    if (!impl_->shm.is_valid()) return 0;
    auto* header = reinterpret_cast<const SharedMemoryHeader*>(impl_->shm.data());
    return header->version;
}

StatePublisher::Stats StatePublisher::stats() const noexcept {
    return impl_->stats;
}

bool StatePublisher::is_valid() const noexcept {
    return impl_->shm.is_valid();
}

// ============================================================
// StateSubscriber Implementation
// ============================================================

struct StateSubscriber::Impl {
    SharedMemoryRegion shm;
    SharedMemoryConfig config;
    Stats stats;
    Value current_state;
    uint64_t current_version = 0;
    
    std::vector<UpdateCallback> callbacks;
    std::mutex callback_mutex;
    
    std::atomic<bool> polling{false};
    std::thread poll_thread;
    
    Impl(const SharedMemoryConfig& cfg)
        : shm(cfg.name, cfg.size, false)  // Open existing, don't create
        , config(cfg)
    {}
    
    ~Impl() {
        stop_polling();
    }
    
    void stop_polling() {
        polling = false;
        if (poll_thread.joinable()) {
            poll_thread.join();
        }
    }
    
    bool check_and_read() {
        if (!shm.is_valid()) return false;
        
        auto* header = reinterpret_cast<const SharedMemoryHeader*>(shm.data());
        
        // Check magic
        if (header->magic != SHARED_MEMORY_MAGIC) {
            return false;
        }
        
        // Check version
        if (header->version == current_version) {
            return false;  // No update
        }
        
        // Check for missed updates
        if (header->version > current_version + 1) {
            stats.missed_updates += (header->version - current_version - 1);
        }
        
        // Read data
        const uint8_t* data_ptr = reinterpret_cast<const uint8_t*>(shm.data()) + HEADER_SIZE;
        std::size_t data_size = header->data_size;
        auto update_type = static_cast<StateUpdate::Type>(header->update_type);
        uint64_t new_version = header->version;
        
        // Memory barrier before reading data
#ifdef _WIN32
        MemoryBarrier();
#else
        __sync_synchronize();
#endif
        
        // Copy data (to avoid race conditions)
        ByteBuffer data(data_ptr, data_ptr + data_size);
        
        // Process update
        try {
            if (update_type == StateUpdate::Type::Full) {
                current_state = deserialize(data);
                stats.full_updates++;
            } else {
                DiffResult diff = decode_diff(data);
                current_state = apply_diff(current_state, diff);
                stats.diff_updates++;
            }
            
            current_version = new_version;
            stats.total_updates++;
            stats.total_bytes_read += data_size;
            
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[StateSubscriber] Failed to process update: " << e.what() << "\n";
            return false;
        }
    }
    
    void invoke_callbacks() {
        std::lock_guard<std::mutex> lock(callback_mutex);
        for (auto& cb : callbacks) {
            if (cb) {
                try {
                    cb(current_state, current_version);
                } catch (const std::exception& e) {
                    std::cerr << "[StateSubscriber] Callback error: " << e.what() << "\n";
                }
            }
        }
    }
};

StateSubscriber::StateSubscriber(const SharedMemoryConfig& config)
    : impl_(std::make_unique<Impl>(config))
{
    // Try to read initial state
    impl_->check_and_read();
}

StateSubscriber::~StateSubscriber() = default;

StateSubscriber::StateSubscriber(StateSubscriber&&) noexcept = default;
StateSubscriber& StateSubscriber::operator=(StateSubscriber&&) noexcept = default;

const Value& StateSubscriber::current() const noexcept {
    return impl_->current_state;
}

uint64_t StateSubscriber::version() const noexcept {
    return impl_->current_version;
}

bool StateSubscriber::poll() {
    if (impl_->check_and_read()) {
        impl_->invoke_callbacks();
        return true;
    }
    return false;
}

std::optional<Value> StateSubscriber::try_get_update() {
    if (poll()) {
        return impl_->current_state;
    }
    return std::nullopt;
}

Value StateSubscriber::wait_for_update(std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();
    
    while (true) {
        if (poll()) {
            return impl_->current_state;
        }
        
        // Check timeout
        if (timeout.count() > 0) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= timeout) {
                return impl_->current_state;  // Return current state on timeout
            }
        }
        
        std::this_thread::sleep_for(impl_->config.poll_interval);
    }
}

void StateSubscriber::on_update(UpdateCallback callback) {
    std::lock_guard<std::mutex> lock(impl_->callback_mutex);
    impl_->callbacks.push_back(std::move(callback));
}

void StateSubscriber::start_polling() {
    if (impl_->polling) return;
    
    impl_->polling = true;
    impl_->poll_thread = std::thread([this]() {
        while (impl_->polling) {
            poll();
            std::this_thread::sleep_for(impl_->config.poll_interval);
        }
    });
}

void StateSubscriber::stop_polling() {
    impl_->stop_polling();
}

bool StateSubscriber::is_polling() const noexcept {
    return impl_->polling;
}

StateSubscriber::Stats StateSubscriber::stats() const noexcept {
    return impl_->stats;
}

bool StateSubscriber::is_valid() const noexcept {
    return impl_->shm.is_valid();
}

// ============================================================
// Collect Diff - Recursive difference collection
// ============================================================

// Helper: Compare two Values and collect differences
static void collect_diff_recursive(const Value& old_val, const Value& new_val, 
                                    Path current_path, DiffResult& result) {
    // Same object (structural sharing) - no diff
    if (&old_val == &new_val) {
        return;
    }
    
    // Different types or both primitives - report as modification
    if (old_val.type_index() != new_val.type_index()) {
        result.modified.push_back({current_path, old_val, new_val});
        return;
    }
    
    // Both are maps
    if (auto* old_map = old_val.get_if<ValueMap>()) {
        auto* new_map = new_val.get_if<ValueMap>();
        
        // Check for added and modified entries
        for (const auto& [key, new_box] : *new_map) {
            Path child_path = current_path;
            child_path.push_back(key);
            
            if (auto* old_box = old_map->find(key)) {
                // Key exists in both - check if same box (structural sharing)
                if (&(*old_box) != &new_box) {
                    collect_diff_recursive(old_box->get(), new_box.get(), child_path, result);
                }
            } else {
                // Key only in new - added
                result.added.emplace_back(child_path, new_box.get());
            }
        }
        
        // Check for removed entries
        for (const auto& [key, old_box] : *old_map) {
            if (!new_map->find(key)) {
                Path child_path = current_path;
                child_path.push_back(key);
                result.removed.emplace_back(child_path, old_box.get());
            }
        }
        return;
    }
    
    // Both are vectors
    if (auto* old_vec = old_val.get_if<ValueVector>()) {
        auto* new_vec = new_val.get_if<ValueVector>();
        
        std::size_t min_size = std::min(old_vec->size(), new_vec->size());
        
        // Compare common elements
        for (std::size_t i = 0; i < min_size; ++i) {
            const auto& old_box = (*old_vec)[i];
            const auto& new_box = (*new_vec)[i];
            
            // Check if same box (structural sharing)
            if (&old_box != &new_box) {
                Path child_path = current_path;
                child_path.push_back(i);
                collect_diff_recursive(old_box.get(), new_box.get(), child_path, result);
            }
        }
        
        // Elements added to new vector
        for (std::size_t i = min_size; i < new_vec->size(); ++i) {
            Path child_path = current_path;
            child_path.push_back(i);
            result.added.emplace_back(child_path, (*new_vec)[i].get());
        }
        
        // Elements removed from old vector
        for (std::size_t i = min_size; i < old_vec->size(); ++i) {
            Path child_path = current_path;
            child_path.push_back(i);
            result.removed.emplace_back(child_path, (*old_vec)[i].get());
        }
        return;
    }
    
    // Both are arrays
    if (auto* old_arr = old_val.get_if<ValueArray>()) {
        auto* new_arr = new_val.get_if<ValueArray>();
        
        std::size_t min_size = std::min(old_arr->size(), new_arr->size());
        
        for (std::size_t i = 0; i < min_size; ++i) {
            const auto& old_box = (*old_arr)[i];
            const auto& new_box = (*new_arr)[i];
            
            if (&old_box != &new_box) {
                Path child_path = current_path;
                child_path.push_back(i);
                collect_diff_recursive(old_box.get(), new_box.get(), child_path, result);
            }
        }
        
        for (std::size_t i = min_size; i < new_arr->size(); ++i) {
            Path child_path = current_path;
            child_path.push_back(i);
            result.added.emplace_back(child_path, (*new_arr)[i].get());
        }
        
        for (std::size_t i = min_size; i < old_arr->size(); ++i) {
            Path child_path = current_path;
            child_path.push_back(i);
            result.removed.emplace_back(child_path, (*old_arr)[i].get());
        }
        return;
    }
    
    // Primitive types - compare by value
    if (old_val.data != new_val.data) {
        result.modified.push_back({current_path, old_val, new_val});
    }
}

DiffResult collect_diff(const Value& old_val, const Value& new_val) {
    DiffResult result;
    collect_diff_recursive(old_val, new_val, {}, result);
    return result;
}

// ============================================================
// Diff Encoding/Decoding
// ============================================================
// Format:
// [4 bytes] added_count
// [entries] added entries: [path_len][path_data][value_data]
// [4 bytes] removed_count
// [entries] removed entries: [path_len][path_data]
// [4 bytes] modified_count
// [entries] modified entries: [path_len][path_data][old_value_data][new_value_data]
// ============================================================

static void write_path(ByteBuffer& buf, const Path& path) {
    // Write path element count
    uint32_t count = static_cast<uint32_t>(path.size());
    buf.push_back(count & 0xFF);
    buf.push_back((count >> 8) & 0xFF);
    buf.push_back((count >> 16) & 0xFF);
    buf.push_back((count >> 24) & 0xFF);
    
    for (const auto& elem : path) {
        if (auto* s = std::get_if<std::string>(&elem)) {
            buf.push_back(0);  // String type
            uint32_t len = static_cast<uint32_t>(s->size());
            buf.push_back(len & 0xFF);
            buf.push_back((len >> 8) & 0xFF);
            buf.push_back((len >> 16) & 0xFF);
            buf.push_back((len >> 24) & 0xFF);
            buf.insert(buf.end(), s->begin(), s->end());
        } else if (auto* idx = std::get_if<std::size_t>(&elem)) {
            buf.push_back(1);  // Index type
            uint64_t val = *idx;
            for (int i = 0; i < 8; ++i) {
                buf.push_back((val >> (i * 8)) & 0xFF);
            }
        }
    }
}

static Path read_path(const uint8_t*& ptr, const uint8_t* end) {
    if (ptr + 4 > end) throw std::runtime_error("Invalid path data");
    
    uint32_t count = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
    ptr += 4;
    
    Path path;
    path.reserve(count);
    
    for (uint32_t i = 0; i < count; ++i) {
        if (ptr >= end) throw std::runtime_error("Invalid path element");
        
        uint8_t type = *ptr++;
        if (type == 0) {  // String
            if (ptr + 4 > end) throw std::runtime_error("Invalid string length");
            uint32_t len = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
            ptr += 4;
            if (ptr + len > end) throw std::runtime_error("Invalid string data");
            path.push_back(std::string(reinterpret_cast<const char*>(ptr), len));
            ptr += len;
        } else {  // Index
            if (ptr + 8 > end) throw std::runtime_error("Invalid index data");
            uint64_t val = 0;
            for (int j = 0; j < 8; ++j) {
                val |= static_cast<uint64_t>(ptr[j]) << (j * 8);
            }
            ptr += 8;
            path.push_back(static_cast<std::size_t>(val));
        }
    }
    
    return path;
}

static void write_uint32(ByteBuffer& buf, uint32_t val) {
    buf.push_back(val & 0xFF);
    buf.push_back((val >> 8) & 0xFF);
    buf.push_back((val >> 16) & 0xFF);
    buf.push_back((val >> 24) & 0xFF);
}

static uint32_t read_uint32(const uint8_t*& ptr, const uint8_t* end) {
    if (ptr + 4 > end) throw std::runtime_error("Invalid uint32 data");
    uint32_t val = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
    ptr += 4;
    return val;
}

ByteBuffer encode_diff(const DiffResult& diff) {
    ByteBuffer buf;
    buf.reserve(1024);  // Pre-allocate
    
    // Write added entries
    write_uint32(buf, static_cast<uint32_t>(diff.added.size()));
    for (const auto& [path, value] : diff.added) {
        write_path(buf, path);
        ByteBuffer value_data = serialize(value);
        write_uint32(buf, static_cast<uint32_t>(value_data.size()));
        buf.insert(buf.end(), value_data.begin(), value_data.end());
    }
    
    // Write removed entries
    write_uint32(buf, static_cast<uint32_t>(diff.removed.size()));
    for (const auto& [path, value] : diff.removed) {
        write_path(buf, path);
        // We don't need to send the old value for removed entries
    }
    
    // Write modified entries
    write_uint32(buf, static_cast<uint32_t>(diff.modified.size()));
    for (const auto& mod : diff.modified) {
        write_path(buf, mod.path);
        ByteBuffer new_value_data = serialize(mod.new_value);
        write_uint32(buf, static_cast<uint32_t>(new_value_data.size()));
        buf.insert(buf.end(), new_value_data.begin(), new_value_data.end());
    }
    
    return buf;
}

DiffResult decode_diff(const ByteBuffer& data) {
    DiffResult diff;
    const uint8_t* ptr = data.data();
    const uint8_t* end = ptr + data.size();
    
    // Read added entries
    uint32_t added_count = read_uint32(ptr, end);
    diff.added.reserve(added_count);
    for (uint32_t i = 0; i < added_count; ++i) {
        Path path = read_path(ptr, end);
        uint32_t value_size = read_uint32(ptr, end);
        if (ptr + value_size > end) throw std::runtime_error("Invalid value data");
        Value value = deserialize(ptr, value_size);
        ptr += value_size;
        diff.added.emplace_back(std::move(path), std::move(value));
    }
    
    // Read removed entries
    uint32_t removed_count = read_uint32(ptr, end);
    diff.removed.reserve(removed_count);
    for (uint32_t i = 0; i < removed_count; ++i) {
        Path path = read_path(ptr, end);
        diff.removed.emplace_back(std::move(path), Value{});  // Empty value for removed
    }
    
    // Read modified entries
    uint32_t modified_count = read_uint32(ptr, end);
    diff.modified.reserve(modified_count);
    for (uint32_t i = 0; i < modified_count; ++i) {
        Path path = read_path(ptr, end);
        uint32_t value_size = read_uint32(ptr, end);
        if (ptr + value_size > end) throw std::runtime_error("Invalid value data");
        Value new_value = deserialize(ptr, value_size);
        ptr += value_size;
        diff.modified.push_back({std::move(path), Value{}, std::move(new_value)});
    }
    
    return diff;
}

// ============================================================
// Apply Diff
// ============================================================

// Helper to set value at path
static Value set_at_path(const Value& root, const Path& path, const Value& value) {
    if (path.empty()) {
        return value;
    }
    
    // Use recursive approach
    const auto& first = path[0];
    Path rest(path.begin() + 1, path.end());
    
    if (auto* key = std::get_if<std::string>(&first)) {
        Value child = root.at(*key);
        Value new_child = set_at_path(child, rest, value);
        return root.set(*key, new_child);
    } else if (auto* idx = std::get_if<std::size_t>(&first)) {
        Value child = root.at(*idx);
        Value new_child = set_at_path(child, rest, value);
        return root.set(*idx, new_child);
    }
    
    return root;
}

// Helper to remove value at path (set to null)
static Value remove_at_path(const Value& root, const Path& path) {
    // For simplicity, we set to null instead of actually removing
    // A more sophisticated implementation could shrink containers
    return set_at_path(root, path, Value{});
}

Value apply_diff(const Value& base, const DiffResult& diff) {
    Value result = base;
    
    // Apply removals first (set to null)
    for (const auto& [path, _] : diff.removed) {
        result = remove_at_path(result, path);
    }
    
    // Apply modifications
    for (const auto& mod : diff.modified) {
        result = set_at_path(result, mod.path, mod.new_value);
    }
    
    // Apply additions
    for (const auto& [path, value] : diff.added) {
        result = set_at_path(result, path, value);
    }
    
    return result;
}

// ============================================================
// Demo Function
// ============================================================

void demo_shared_state() {
    std::cout << "\n=== Shared State Demo ===\n\n";
    std::cout << "This demo simulates cross-process state sharing within a single process.\n";
    std::cout << "In real use, Publisher and Subscriber would be in different processes.\n\n";
    
    const std::string shm_name = "immer_lens_demo";
    const std::size_t shm_size = 1024 * 1024;  // 1MB
    
    // Create publisher (main process)
    std::cout << "Creating StatePublisher...\n";
    StatePublisher publisher({shm_name, shm_size, true});
    
    if (!publisher.is_valid()) {
        std::cout << "Failed to create publisher!\n";
        return;
    }
    
    // Publish initial state
    Value initial_state = create_sample_data();
    std::cout << "\nPublishing initial state:\n";
    print_value(initial_state, "  ");
    publisher.publish(initial_state);
    std::cout << "Published version: " << publisher.version() << "\n";
    
    // Create subscriber (child process)
    std::cout << "\nCreating StateSubscriber...\n";
    StateSubscriber subscriber({shm_name, shm_size, false});
    
    if (!subscriber.is_valid()) {
        std::cout << "Failed to create subscriber!\n";
        return;
    }
    
    // Read initial state
    std::cout << "\nSubscriber reading initial state:\n";
    print_value(subscriber.current(), "  ");
    std::cout << "Subscriber version: " << subscriber.version() << "\n";
    
    // Make a change and publish diff
    std::cout << "\n--- Modifying state (changing Alice's age to 26) ---\n";
    
    Value modified_state = initial_state;
    // Navigate: users[0].age
    if (auto* users_vec = modified_state.at("users").get_if<ValueVector>()) {
        if (users_vec->size() > 0) {
            Value alice = (*users_vec)[0].get();
            alice = alice.set("age", Value{26});
            auto new_vec = users_vec->set(0, ValueBox{alice});
            modified_state = modified_state.set("users", Value{new_vec});
        }
    }
    
    std::cout << "\nPublishing diff...\n";
    bool used_diff = publisher.publish_diff(initial_state, modified_state);
    std::cout << "Used diff: " << (used_diff ? "yes" : "no (full state was smaller)") << "\n";
    std::cout << "Published version: " << publisher.version() << "\n";
    
    // Subscriber polls for update
    std::cout << "\nSubscriber polling for update...\n";
    if (subscriber.poll()) {
        std::cout << "Received update! New state:\n";
        print_value(subscriber.current(), "  ");
        std::cout << "Subscriber version: " << subscriber.version() << "\n";
    } else {
        std::cout << "No update available.\n";
    }
    
    // Show statistics
    std::cout << "\n--- Statistics ---\n";
    auto pub_stats = publisher.stats();
    std::cout << "Publisher:\n";
    std::cout << "  Total publishes: " << pub_stats.total_publishes << "\n";
    std::cout << "  Full publishes: " << pub_stats.full_publishes << "\n";
    std::cout << "  Diff publishes: " << pub_stats.diff_publishes << "\n";
    std::cout << "  Total bytes written: " << pub_stats.total_bytes_written << "\n";
    
    auto sub_stats = subscriber.stats();
    std::cout << "Subscriber:\n";
    std::cout << "  Total updates: " << sub_stats.total_updates << "\n";
    std::cout << "  Full updates: " << sub_stats.full_updates << "\n";
    std::cout << "  Diff updates: " << sub_stats.diff_updates << "\n";
    std::cout << "  Total bytes read: " << sub_stats.total_bytes_read << "\n";
    std::cout << "  Missed updates: " << sub_stats.missed_updates << "\n";
    
    std::cout << "\n=== Demo Complete ===\n";
}

} // namespace immer_lens
