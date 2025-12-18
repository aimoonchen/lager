// value.cpp
// Implementation of Value type utilities and serialization
//
// Note: BasicValue<MemoryPolicy> is a template class, so its member
// functions are implemented in value.h. This file only contains
// implementations for non-template utility functions and serialization.

#include "value.h"

#include <cstring>  // for std::memcpy
#include <stdexcept>  // for std::runtime_error

namespace immer_lens {

// ============================================================
// Utility functions (for default Value type)
// ============================================================

std::string value_to_string(const Value& val)
{
    return std::visit([](const auto& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return "\"" + arg + "\"";
        } else if constexpr (std::is_same_v<T, bool>) {
            return arg ? "true" : "false";
        } else if constexpr (std::is_same_v<T, int>) {
            return std::to_string(arg);
        } else if constexpr (std::is_same_v<T, float>) {
            return std::to_string(arg) + "f";
        } else if constexpr (std::is_same_v<T, double>) {
            return std::to_string(arg);
        } else if constexpr (std::is_same_v<T, ValueMap>) {
            return "{map:" + std::to_string(arg.size()) + "}";
        } else if constexpr (std::is_same_v<T, ValueVector>) {
            return "[vector:" + std::to_string(arg.size()) + "]";
        } else if constexpr (std::is_same_v<T, ValueArray>) {
            return "[array:" + std::to_string(arg.size()) + "]";
        } else if constexpr (std::is_same_v<T, ValueTable>) {
            return "<table:" + std::to_string(arg.size()) + ">";
        } else {
            return "null";
        }
    }, val.data);
}

void print_value(const Value& val, const std::string& prefix, std::size_t depth)
{
    std::visit(
        [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;

            if constexpr (std::is_same_v<T, std::string>) {
                std::cout << std::string(depth * 2, ' ') << prefix << arg << "\n";
            } else if constexpr (std::is_same_v<T, bool>) {
                std::cout << std::string(depth * 2, ' ') << prefix
                          << (arg ? "true" : "false") << "\n";
            } else if constexpr (std::is_same_v<T, int> ||
                                 std::is_same_v<T, float> ||
                                 std::is_same_v<T, double>) {
                std::cout << std::string(depth * 2, ' ') << prefix << arg << "\n";
            } else if constexpr (std::is_same_v<T, ValueMap>) {
                for (const auto& [k, v] : arg) {
                    std::cout << std::string(depth * 2, ' ') << prefix << k << ":\n";
                    print_value(*v, "", depth + 1);
                }
            } else if constexpr (std::is_same_v<T, ValueVector>) {
                for (std::size_t i = 0; i < arg.size(); ++i) {
                    std::cout << std::string(depth * 2, ' ') << prefix << "["
                              << i << "]:\n";
                    print_value(*arg[i], "", depth + 1);
                }
            } else if constexpr (std::is_same_v<T, ValueArray>) {
                for (std::size_t i = 0; i < arg.size(); ++i) {
                    std::cout << std::string(depth * 2, ' ') << prefix << "("
                              << i << "):\n";
                    print_value(*arg[i], "", depth + 1);
                }
            } else if constexpr (std::is_same_v<T, ValueTable>) {
                for (const auto& entry : arg) {
                    std::cout << std::string(depth * 2, ' ') << prefix << "<"
                              << entry.id << ">:\n";
                    print_value(*entry.value, "", depth + 1);
                }
            } else if constexpr (std::is_same_v<T, std::monostate>) {
                std::cout << std::string(depth * 2, ' ') << prefix << "null\n";
            }
        },
        val.data);
}

std::string path_to_string(const Path& path)
{
    std::string result;
    for (const auto& elem : path) {
        std::visit([&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                result += "." + v;
            } else {
                result += "[" + std::to_string(v) + "]";
            }
        }, elem);
    }
    return result.empty() ? "/" : result;
}

// ============================================================
// Common test data factory
// ============================================================

Value create_sample_data()
{
    // Create user 1
    ValueMap user1;
    user1 = user1.set("name", ValueBox{Value{std::string{"Alice"}}});
    user1 = user1.set("age", ValueBox{Value{25}});
    
    // Create user 2
    ValueMap user2;
    user2 = user2.set("name", ValueBox{Value{std::string{"Bob"}}});
    user2 = user2.set("age", ValueBox{Value{30}});
    
    // Create users array
    ValueVector users;
    users = users.push_back(ValueBox{Value{user1}});
    users = users.push_back(ValueBox{Value{user2}});
    
    // Create config
    ValueMap config;
    config = config.set("version", ValueBox{Value{1}});
    config = config.set("theme", ValueBox{Value{std::string{"dark"}}});
    
    // Create root
    ValueMap root;
    root = root.set("users", ValueBox{Value{users}});
    root = root.set("config", ValueBox{Value{config}});
    
    return Value{root};
}

// ============================================================
// Serialization / Deserialization Implementation
// ============================================================

namespace {

// Type tags for binary format
enum class TypeTag : uint8_t {
    Null   = 0x00,
    Int    = 0x01,
    Float  = 0x02,
    Double = 0x03,
    Bool   = 0x04,
    String = 0x05,
    Map    = 0x06,
    Vector = 0x07,
    Array  = 0x08,
    Table  = 0x09,
};

// Helper: write bytes to buffer
class ByteWriter {
public:
    ByteBuffer buffer;
    
    void write_u8(uint8_t v) {
        buffer.push_back(v);
    }
    
    void write_u32(uint32_t v) {
        // Little-endian
        buffer.push_back(static_cast<uint8_t>(v & 0xFF));
        buffer.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buffer.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buffer.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    }
    
    void write_i32(int32_t v) {
        write_u32(static_cast<uint32_t>(v));
    }
    
    void write_f32(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        write_u32(bits);
    }
    
    void write_f64(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        // Little-endian
        for (int i = 0; i < 8; ++i) {
            buffer.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
        }
    }
    
    void write_string(const std::string& s) {
        write_u32(static_cast<uint32_t>(s.size()));
        for (char c : s) {
            buffer.push_back(static_cast<uint8_t>(c));
        }
    }
};

// Helper: read bytes from buffer
class ByteReader {
public:
    const uint8_t* data;
    std::size_t size;
    std::size_t pos = 0;
    
    ByteReader(const uint8_t* d, std::size_t s) : data(d), size(s) {}
    
    bool has_bytes(std::size_t n) const {
        return pos + n <= size;
    }
    
    uint8_t read_u8() {
        if (!has_bytes(1)) throw std::runtime_error("Unexpected end of buffer");
        return data[pos++];
    }
    
    uint32_t read_u32() {
        if (!has_bytes(4)) throw std::runtime_error("Unexpected end of buffer");
        uint32_t v = 0;
        v |= static_cast<uint32_t>(data[pos++]);
        v |= static_cast<uint32_t>(data[pos++]) << 8;
        v |= static_cast<uint32_t>(data[pos++]) << 16;
        v |= static_cast<uint32_t>(data[pos++]) << 24;
        return v;
    }
    
    int32_t read_i32() {
        return static_cast<int32_t>(read_u32());
    }
    
    float read_f32() {
        uint32_t bits = read_u32();
        float v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    
    double read_f64() {
        if (!has_bytes(8)) throw std::runtime_error("Unexpected end of buffer");
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) {
            bits |= static_cast<uint64_t>(data[pos++]) << (i * 8);
        }
        double v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    
    std::string read_string() {
        uint32_t len = read_u32();
        if (!has_bytes(len)) throw std::runtime_error("Unexpected end of buffer");
        std::string s(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
        return s;
    }
};

// Forward declarations
void serialize_value(ByteWriter& w, const Value& val);
Value deserialize_value(ByteReader& r);

void serialize_value(ByteWriter& w, const Value& val) {
    std::visit([&w](const auto& arg) {
        using T = std::decay_t<decltype(arg)>;
        
        if constexpr (std::is_same_v<T, std::monostate>) {
            w.write_u8(static_cast<uint8_t>(TypeTag::Null));
        } else if constexpr (std::is_same_v<T, int>) {
            w.write_u8(static_cast<uint8_t>(TypeTag::Int));
            w.write_i32(arg);
        } else if constexpr (std::is_same_v<T, float>) {
            w.write_u8(static_cast<uint8_t>(TypeTag::Float));
            w.write_f32(arg);
        } else if constexpr (std::is_same_v<T, double>) {
            w.write_u8(static_cast<uint8_t>(TypeTag::Double));
            w.write_f64(arg);
        } else if constexpr (std::is_same_v<T, bool>) {
            w.write_u8(static_cast<uint8_t>(TypeTag::Bool));
            w.write_u8(arg ? 0x01 : 0x00);
        } else if constexpr (std::is_same_v<T, std::string>) {
            w.write_u8(static_cast<uint8_t>(TypeTag::String));
            w.write_string(arg);
        } else if constexpr (std::is_same_v<T, ValueMap>) {
            w.write_u8(static_cast<uint8_t>(TypeTag::Map));
            w.write_u32(static_cast<uint32_t>(arg.size()));
            for (const auto& [k, v] : arg) {
                w.write_string(k);
                serialize_value(w, *v);
            }
        } else if constexpr (std::is_same_v<T, ValueVector>) {
            w.write_u8(static_cast<uint8_t>(TypeTag::Vector));
            w.write_u32(static_cast<uint32_t>(arg.size()));
            for (const auto& v : arg) {
                serialize_value(w, *v);
            }
        } else if constexpr (std::is_same_v<T, ValueArray>) {
            w.write_u8(static_cast<uint8_t>(TypeTag::Array));
            w.write_u32(static_cast<uint32_t>(arg.size()));
            for (std::size_t i = 0; i < arg.size(); ++i) {
                serialize_value(w, *arg[i]);
            }
        } else if constexpr (std::is_same_v<T, ValueTable>) {
            w.write_u8(static_cast<uint8_t>(TypeTag::Table));
            w.write_u32(static_cast<uint32_t>(arg.size()));
            for (const auto& entry : arg) {
                w.write_string(entry.id);
                serialize_value(w, *entry.value);
            }
        }
    }, val.data);
}

Value deserialize_value(ByteReader& r) {
    TypeTag tag = static_cast<TypeTag>(r.read_u8());
    
    switch (tag) {
        case TypeTag::Null:
            return Value{};
            
        case TypeTag::Int:
            return Value{r.read_i32()};
            
        case TypeTag::Float:
            return Value{r.read_f32()};
            
        case TypeTag::Double:
            return Value{r.read_f64()};
            
        case TypeTag::Bool:
            return Value{r.read_u8() != 0};
            
        case TypeTag::String:
            return Value{r.read_string()};
            
        case TypeTag::Map: {
            uint32_t count = r.read_u32();
            ValueMap map;
            for (uint32_t i = 0; i < count; ++i) {
                std::string key = r.read_string();
                Value val = deserialize_value(r);
                map = map.set(key, ValueBox{std::move(val)});
            }
            return Value{std::move(map)};
        }
        
        case TypeTag::Vector: {
            uint32_t count = r.read_u32();
            ValueVector vec;
            for (uint32_t i = 0; i < count; ++i) {
                Value val = deserialize_value(r);
                vec = vec.push_back(ValueBox{std::move(val)});
            }
            return Value{std::move(vec)};
        }
        
        case TypeTag::Array: {
            uint32_t count = r.read_u32();
            // Build vector first, then convert to array
            std::vector<ValueBox> temp;
            temp.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                Value val = deserialize_value(r);
                temp.push_back(ValueBox{std::move(val)});
            }
            ValueArray arr{temp.begin(), temp.end()};
            return Value{std::move(arr)};
        }
        
        case TypeTag::Table: {
            uint32_t count = r.read_u32();
            ValueTable table;
            for (uint32_t i = 0; i < count; ++i) {
                std::string id = r.read_string();
                Value val = deserialize_value(r);
                table = table.insert(TableEntry{std::move(id), ValueBox{std::move(val)}});
            }
            return Value{std::move(table)};
        }
        
        default:
            throw std::runtime_error("Unknown type tag: " + std::to_string(static_cast<int>(tag)));
    }
}

std::size_t calc_serialized_size(const Value& val) {
    std::size_t size = 1; // type tag
    
    std::visit([&size](const auto& arg) {
        using T = std::decay_t<decltype(arg)>;
        
        if constexpr (std::is_same_v<T, std::monostate>) {
            // no extra data
        } else if constexpr (std::is_same_v<T, int>) {
            size += 4;
        } else if constexpr (std::is_same_v<T, float>) {
            size += 4;
        } else if constexpr (std::is_same_v<T, double>) {
            size += 8;
        } else if constexpr (std::is_same_v<T, bool>) {
            size += 1;
        } else if constexpr (std::is_same_v<T, std::string>) {
            size += 4 + arg.size();
        } else if constexpr (std::is_same_v<T, ValueMap>) {
            size += 4; // count
            for (const auto& [k, v] : arg) {
                size += 4 + k.size(); // key string
                size += calc_serialized_size(*v);
            }
        } else if constexpr (std::is_same_v<T, ValueVector>) {
            size += 4; // count
            for (const auto& v : arg) {
                size += calc_serialized_size(*v);
            }
        } else if constexpr (std::is_same_v<T, ValueArray>) {
            size += 4; // count
            for (std::size_t i = 0; i < arg.size(); ++i) {
                size += calc_serialized_size(*arg[i]);
            }
        } else if constexpr (std::is_same_v<T, ValueTable>) {
            size += 4; // count
            for (const auto& entry : arg) {
                size += 4 + entry.id.size(); // id string
                size += calc_serialized_size(*entry.value);
            }
        }
    }, val.data);
    
    return size;
}

} // anonymous namespace

ByteBuffer serialize(const Value& val) {
    ByteWriter w;
    w.buffer.reserve(calc_serialized_size(val));
    serialize_value(w, val);
    return std::move(w.buffer);
}

Value deserialize(const ByteBuffer& buffer) {
    return deserialize(buffer.data(), buffer.size());
}

Value deserialize(const uint8_t* data, std::size_t size) {
    if (size == 0) {
        return Value{};
    }
    ByteReader r(data, size);
    return deserialize_value(r);
}

std::size_t serialized_size(const Value& val) {
    return calc_serialized_size(val);
}

std::size_t serialize_to(const Value& val, uint8_t* buffer, std::size_t buffer_size) {
    ByteBuffer temp = serialize(val);
    if (temp.size() > buffer_size) {
        throw std::runtime_error("Buffer too small: need " + std::to_string(temp.size()) + 
                                 " bytes, got " + std::to_string(buffer_size));
    }
    std::memcpy(buffer, temp.data(), temp.size());
    return temp.size();
}

} // namespace immer_lens
