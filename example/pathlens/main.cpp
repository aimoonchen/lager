#include <lager/lenses.hpp>
#include <lager/store.hpp>
#include <lager/util.hpp>
#include <immer/box.hpp>
#include <immer/map.hpp>
#include <immer/vector.hpp>
#include <immer/algorithm.hpp>  // for immer::diff
#include <lager/event_loop/manual.hpp>

#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <variant>

namespace immer_lens {

struct Value;
using ValueMap    = immer::map<std::string, immer::box<Value>>;
using ValueVector = immer::vector<immer::box<Value>>;

struct Value
{
    std::variant<int,
                 double,
                 bool,
                 std::string,
                 ValueMap,
                 ValueVector,
                 std::monostate>
        data;

    Value()
        : data(std::monostate{})
    {
    }
    Value(int v)
        : data(v)
    {
    }
    Value(double v)
        : data(v)
    {
    }
    Value(bool v)
        : data(v)
    {
    }
    Value(const std::string& v)
        : data(v)
    {
    }
    Value(const char* v)  // 新增：支持字符串字面量
        : data(std::string{v})
    {
    }
    Value(ValueMap v)
        : data(std::move(v))
    {
    }
    Value(ValueVector v)
        : data(std::move(v))
    {
    }

    template <typename T>
    const T* get_if() const
    {
        return std::get_if<T>(&data);
    }

    template <typename T>
    bool is() const
    {
        return std::holds_alternative<T>(data);
    }
    
    // 获取 variant 类型索引（用于快速类型比较）
    size_t type_index() const noexcept { return data.index(); }
    
    // 判断是否为 null
    bool is_null() const noexcept { return std::holds_alternative<std::monostate>(data); }
};

// Value 相等比较（利用 variant 的 index 快速短路）
inline bool operator==(const Value& a, const Value& b) {
    return a.data == b.data;
}
inline bool operator!=(const Value& a, const Value& b) {
    return !(a == b);
}

using PathElement = std::variant<std::string, size_t>;
using Path        = std::vector<PathElement>;

auto key_lens(const std::string& key)
{
    return lager::lenses::getset(
        [key](const Value& obj) -> Value {
            if (auto* map = obj.get_if<ValueMap>()) {
                if (auto found = map->find(key); found != nullptr) {
                    return **found;
                }
            }
            return Value{};
        },
        [key](Value obj, Value value) -> Value {
            if (auto* map = obj.get_if<ValueMap>()) {
                auto new_map =
                    map->set(key, immer::box<Value>{std::move(value)});
                return Value{std::move(new_map)};
            }
            ValueMap new_map;
            new_map = new_map.set(key, immer::box<Value>{std::move(value)});
            return Value{std::move(new_map)};
        });
}

auto index_lens(size_t index)
{
    return lager::lenses::getset(
        [index](const Value& obj) -> Value {
            if (auto* vec = obj.get_if<ValueVector>()) {
                if (index < vec->size()) {
                    return *(*vec)[index];
                }
            }
            return Value{};
        },
        [index](Value obj, Value value) -> Value {
            if (auto* vec = obj.get_if<ValueVector>()) {
                if (index < vec->size()) {
                    auto new_vec = vec->update(index, [&](auto&& box) {
                        return immer::box<Value>{std::move(value)};
                    });
                    return Value{std::move(new_vec)};
                } else {
                    auto new_vec = *vec;
                    for (size_t i = vec->size(); i <= index; ++i) {
                        new_vec = new_vec.push_back(
                            immer::box<Value>{Value{}});
                    }
                    new_vec = new_vec.update(index, [&](auto&&) {
                        return immer::box<Value>{std::move(value)};
                    });
                    return Value{std::move(new_vec)};
                }
            }
            ValueVector new_vec;
            for (size_t i = 0; i <= index; ++i) {
                new_vec = new_vec.push_back(immer::box<Value>{Value{}});
            }
            new_vec = new_vec.update(index, [&](auto&&) {
                return immer::box<Value>{std::move(value)};
            });
            return Value{std::move(new_vec)};
        });
}

// Helper to convert element to appropriate lens
template <typename T>
auto element_to_lens(T&& elem)
{
    if constexpr (std::is_convertible_v<T, std::string> ||
                  std::is_same_v<std::decay_t<T>, const char*>) {
        return key_lens(std::string{elem});
    } else if constexpr (std::is_integral_v<std::decay_t<T>>) {
        return index_lens(static_cast<size_t>(elem));
    }
}

// Static path lens using fold expression
template <typename... Elements>
auto static_path_lens(Elements&&... elements)
{
    return (zug::identity | ... |
            element_to_lens(std::forward<Elements>(elements)));
}
// auto user_name_lens = static_path_lens("users", 0, "name");
// auto value = lager::view(lens, root);

// ============================================================
// Type-erased lens class
// 支持动态路径组合的类型擦除 Lens
// 
// 设计要点:
// 1. 使用 std::function 实现类型擦除
// 2. 支持 | 运算符进行组合（类似 zug::comp）
// 3. 支持 over() 操作用于函数式更新
// ============================================================
class ErasedLens
{
public:
    using Getter = std::function<Value(const Value&)>;
    using Setter = std::function<Value(Value, Value)>;

private:
    Getter getter_;
    Setter setter_;

public:
    // Identity lens（默认构造为恒等 lens）
    ErasedLens()
        : getter_([](const Value& v) { return v; })
        , setter_([](Value, Value v) { return v; })
    {
    }

    ErasedLens(Getter g, Setter s)
        : getter_(std::move(g))
        , setter_(std::move(s))
    {
    }

    // 获取聚焦的值
    Value get(const Value& v) const { return getter_(v); }
    
    // 设置聚焦的值，返回更新后的整体
    Value set(Value whole, Value part) const
    {
        return setter_(std::move(whole), std::move(part));
    }
    
    // 使用函数更新聚焦的值（over 操作）
    template<typename Fn>
    Value over(Value whole, Fn&& fn) const
    {
        auto current = getter_(whole);
        auto updated = std::forward<Fn>(fn)(std::move(current));
        return setter_(std::move(whole), std::move(updated));
    }

    // Compose with inner lens (this -> inner)
    // 即：先用 this 聚焦，再用 inner 进一步聚焦
    ErasedLens compose(const ErasedLens& inner) const
    {
        auto outer_get = getter_;
        auto outer_set = setter_;
        auto inner_get = inner.getter_;
        auto inner_set = inner.setter_;

        return ErasedLens{
            [=](const Value& v) { return inner_get(outer_get(v)); },
            [=](Value whole, Value new_val) {
                auto outer_part = outer_get(whole);
                auto new_outer =
                    inner_set(std::move(outer_part), std::move(new_val));
                return outer_set(std::move(whole), std::move(new_outer));
            }};
    }
    
    // 支持 | 运算符组合（类似 zug::comp 的风格）
    friend ErasedLens operator|(const ErasedLens& lhs, const ErasedLens& rhs)
    {
        return lhs.compose(rhs);
    }
};

// Helper functions to create ErasedLens
ErasedLens make_key_lens(const std::string& key)
{
    return ErasedLens{
        [key](const Value& obj) -> Value {
            if (auto* map = obj.get_if<ValueMap>()) {
                if (auto found = map->find(key))
                    return **found;
            }
            return Value{};
        },
        [key](Value obj, Value value) -> Value {
            ValueMap map;
            if (auto* m = obj.get_if<ValueMap>())
                map = *m;
            return Value{map.set(key, immer::box<Value>{std::move(value)})};
        }};
}

ErasedLens make_index_lens(size_t index)
{
    return ErasedLens{
        [index](const Value& obj) -> Value {
            if (auto* vec = obj.get_if<ValueVector>()) {
                if (index < vec->size())
                    return *(*vec)[index];
            }
            return Value{};
        },
        [index](Value obj, Value value) -> Value {
            ValueVector vec;
            if (auto* v = obj.get_if<ValueVector>())
                vec = *v;
            while (vec.size() <= index) {
                vec = vec.push_back(immer::box<Value>{Value{}});
            }
            return Value{vec.set(index, immer::box<Value>{std::move(value)})};
        }};
}

// Build lens from path
ErasedLens path_lens(const Path& path)
{
    ErasedLens result; // Identity lens

    for (const auto& elem : path) {
        ErasedLens current = std::visit(
            [](const auto& value) -> ErasedLens {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    return make_key_lens(value);
                } else {
                    return make_index_lens(value);
                }
            },
            elem);

        result = result.compose(current);
    }

    return result;
}

struct AddItem
{
    std::string text;
};
struct UpdateItem
{
    Path path;
    std::string new_value;
};
struct Undo
{};
struct Redo
{};

using Action = std::variant<AddItem, UpdateItem, Undo, Redo>;

struct AppState
{
    Value data;
    immer::vector<Value> history;
    immer::vector<Value> future;
};

auto create_initial_state()
{
    auto item1 = Value{
        ValueMap{{"title", immer::box<Value>{Value{std::string{"Task 1"}}}},
                 {"done", immer::box<Value>{Value{false}}}}};

    auto items = Value{ValueVector{immer::box<Value>{std::move(item1)}}};

    auto root = Value{ValueMap{{"items", immer::box<Value>{std::move(items)}}}};

    return AppState{.data    = std::move(root),
                    .history = immer::vector<Value>{},
                    .future  = immer::vector<Value>{}};
}

AppState reducer(AppState state, Action action)
{
    return std::visit(
        [&](auto&& act) -> AppState {
            using T = std::decay_t<decltype(act)>;

            if constexpr (std::is_same_v<T, Undo>) {
                if (state.history.empty())
                    return state;

                auto new_state = state;
                auto previous  = new_state.history.back();

                new_state.future = new_state.future.push_back(new_state.data);
                new_state.data   = previous;
                new_state.history =
                    new_state.history.take(new_state.history.size() - 1);

                return new_state;

            } else if constexpr (std::is_same_v<T, Redo>) {
                if (state.future.empty())
                    return state;

                auto new_state = state;
                auto next      = new_state.future.back();

                new_state.history = new_state.history.push_back(new_state.data);
                new_state.data    = next;
                new_state.future =
                    new_state.future.take(new_state.future.size() - 1);

                return new_state;

            } else if constexpr (std::is_same_v<T, AddItem>) {
                auto new_state    = state;
                new_state.history = new_state.history.push_back(state.data);
                new_state.future  = immer::vector<Value>{};

                // Get current items using ErasedLens
                Path items_path = {std::string{"items"}};
                auto items_lens = path_lens(items_path);
                auto current_items = items_lens.get(new_state.data);

                if (auto* vec = current_items.get_if<ValueVector>()) {
                    // Create new item
                    auto new_item = Value{
                        ValueMap{{"title", immer::box<Value>{Value{act.text}}},
                                 {"done", immer::box<Value>{Value{false}}}}};

                    auto new_vec =
                        vec->push_back(immer::box<Value>{std::move(new_item)});
                    new_state.data = items_lens.set(new_state.data,
                                                    Value{std::move(new_vec)});
                }

                return new_state;

            } else if constexpr (std::is_same_v<T, UpdateItem>) {
                auto new_state    = state;
                new_state.history = new_state.history.push_back(state.data);
                new_state.future  = immer::vector<Value>{};

                auto lens      = immer_lens::path_lens(act.path);
                auto new_value = Value{act.new_value};
                new_state.data = lens.set(new_state.data, std::move(new_value));

                return new_state;
            }

            return state;
        },
        action);
}

void print_value(const Value& val,
                 const std::string& prefix = "",
                 size_t depth                 = 0)
{
    std::visit(
        [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;

            if constexpr (std::is_same_v<T, std::string>) {
                std::cout << std::string(depth * 2, ' ') << prefix << arg
                          << "\n";
            } else if constexpr (std::is_same_v<T, bool>) {
                std::cout << std::string(depth * 2, ' ') << prefix
                          << (arg ? "true" : "false") << "\n";
            } else if constexpr (std::is_same_v<T, int> ||
                                 std::is_same_v<T, double>) {
                std::cout << std::string(depth * 2, ' ') << prefix << arg
                          << "\n";
            } else if constexpr (std::is_same_v<T, ValueMap>) {
                for (const auto& [k, v] : arg) {
                    std::cout << std::string(depth * 2, ' ') << prefix << k
                              << ":\n";
                    print_value(*v, "", depth + 1);
                }
            } else if constexpr (std::is_same_v<T, ValueVector>) {
                for (size_t i = 0; i < arg.size(); ++i) {
                    std::cout << std::string(depth * 2, ' ') << prefix << "["
                              << i << "]:\n";
                    print_value(*arg[i], "", depth + 1);
                }
            } else if constexpr (std::is_same_v<T, std::monostate>) {
                std::cout << std::string(depth * 2, ' ') << prefix << "null\n";
            }
        },
        val.data);
}

// ============================================================
// DiffEntry 结构 - 记录单个变化
// ============================================================
struct DiffEntry {
    enum class Type { Add, Remove, Change };
    
    Type type;
    Path path;              // 变化的路径
    std::string old_value;  // 旧值的字符串表示
    std::string new_value;  // 新值的字符串表示
    
    std::string path_to_string() const {
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
};

// ============================================================
// 将 Value 转换为可读字符串
// ============================================================
std::string value_to_string(const Value& val) {
    return std::visit([](const auto& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return "\"" + arg + "\"";
        } else if constexpr (std::is_same_v<T, bool>) {
            return arg ? "true" : "false";
        } else if constexpr (std::is_same_v<T, int>) {
            return std::to_string(arg);
        } else if constexpr (std::is_same_v<T, double>) {
            return std::to_string(arg);
        } else if constexpr (std::is_same_v<T, ValueMap>) {
            return "{map:" + std::to_string(arg.size()) + "}";
        } else if constexpr (std::is_same_v<T, ValueVector>) {
            return "[vector:" + std::to_string(arg.size()) + "]";
        } else {
            return "null";
        }
    }, val.data);
}

// ============================================================
// RecursiveDiffCollector - 递归收集所有 diff (优化版)
// 
// 优化点:
// 1. 利用 immer::box 的指针比较实现 O(1) 快速跳过未变化节点
// 2. 递归遍历到叶子节点，收集完整的变化路径
// 3. 支持 ValueMap 和 ValueVector 的嵌套结构
// 4. 正确处理 immer::diff 回调签名
// 5. 使用双指针法同时遍历 old/new vector 以正确获取索引
// ============================================================
class RecursiveDiffCollector {
private:
    std::vector<DiffEntry> diffs_;

    // 递归比较两个 Value，收集差异
    void diff_value(const Value& old_val, const Value& new_val, Path current_path) {
        // 快速路径：如果类型不同，直接记录为 Change
        if (old_val.data.index() != new_val.data.index()) {
            diffs_.push_back({DiffEntry::Type::Change, current_path, 
                              value_to_string(old_val), value_to_string(new_val)});
            return;
        }

        std::visit([&](const auto& old_arg) {
            using T = std::decay_t<decltype(old_arg)>;
            
            if constexpr (std::is_same_v<T, ValueMap>) {
                const auto& new_map = *new_val.get_if<ValueMap>();
                diff_map(old_arg, new_map, current_path);
            }
            else if constexpr (std::is_same_v<T, ValueVector>) {
                const auto& new_vec = *new_val.get_if<ValueVector>();
                diff_vector(old_arg, new_vec, current_path);
            }
            else if constexpr (std::is_same_v<T, std::monostate>) {
                // 两个 null，无变化
            }
            else {
                // 基本类型：直接比较
                const auto& new_arg = std::get<T>(new_val.data);
                if (old_arg != new_arg) {
                    diffs_.push_back({DiffEntry::Type::Change, current_path,
                                      value_to_string(old_val), value_to_string(new_val)});
                }
            }
        }, old_val.data);
    }

    // 对 ValueMap 进行 diff
    // ★ 使用 immer::make_differ 创建 differ 对象，代码更清晰
    void diff_map(const ValueMap& old_map, const ValueMap& new_map, Path current_path) {
        // 创建 differ 对象，封装三个回调
        auto map_differ = immer::make_differ(
            // added: 新增的 key-value pair
            [&](const std::pair<const std::string, immer::box<Value>>& added_kv) {
                Path child_path = current_path;
                child_path.push_back(added_kv.first);
                collect_added(*added_kv.second, child_path);
            },
            // removed: 删除的 key-value pair
            [&](const std::pair<const std::string, immer::box<Value>>& removed_kv) {
                Path child_path = current_path;
                child_path.push_back(removed_kv.first);
                collect_removed(*removed_kv.second, child_path);
            },
            // changed: 保留的 key（可能值变了）- 两个参数，old 和 new
            [&](const std::pair<const std::string, immer::box<Value>>& old_kv,
                const std::pair<const std::string, immer::box<Value>>& new_kv) {
                // ★ 核心优化：利用 immer::box 的指针比较 - O(1)
                if (old_kv.second.get() == new_kv.second.get()) {
                    return; // 指针相同，完全未变化，跳过！
                }
                // 指针不同，递归比较
                Path child_path = current_path;
                child_path.push_back(old_kv.first);
                diff_value(*old_kv.second, *new_kv.second, child_path);
            }
        );

        // 使用 differ 对象进行 diff
        immer::diff(old_map, new_map, map_differ);
    }

    // 对 ValueVector 进行 diff
    // ★ 关键：immer::diff 只支持 map/set，不支持 vector！
    //   （参见 immer/algorithm.hpp 第 299-305 行的注释）
    // 我们使用索引遍历的方法
    void diff_vector(const ValueVector& old_vec, const ValueVector& new_vec, Path current_path) {
        const size_t old_size = old_vec.size();
        const size_t new_size = new_vec.size();
        const size_t common_size = std::min(old_size, new_size);
        
        // 1. 遍历共有索引范围，比较每个位置
        for (size_t i = 0; i < common_size; ++i) {
            const auto& old_box = old_vec[i];
            const auto& new_box = new_vec[i];
            
            // ★ 核心优化：利用 immer::box 的 operator== 
            //   immer/box.hpp 第 172-175 行：先比较指针，再比较值
            //   如果指针相同，直接返回 true（O(1)）
            if (old_box == new_box) {
                continue; // 完全相同（指针或值），跳过
            }
            
            // 不同，递归比较
            Path child_path = current_path;
            child_path.push_back(i);
            diff_value(*old_box, *new_box, child_path);
        }
        
        // 2. 处理被删除的尾部元素
        for (size_t i = common_size; i < old_size; ++i) {
            Path child_path = current_path;
            child_path.push_back(i);
            collect_removed(*old_vec[i], child_path);
        }
        
        // 3. 处理新增的尾部元素
        for (size_t i = common_size; i < new_size; ++i) {
            Path child_path = current_path;
            child_path.push_back(i);
            collect_added(*new_vec[i], child_path);
        }
    }

    // 收集被删除节点的所有叶子
    void collect_removed(const Value& val, Path current_path) {
        std::visit([&](const auto& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, ValueMap>) {
                for (const auto& [k, v] : arg) {
                    Path child_path = current_path;
                    child_path.push_back(k);
                    collect_removed(*v, child_path);
                }
            }
            else if constexpr (std::is_same_v<T, ValueVector>) {
                for (size_t i = 0; i < arg.size(); ++i) {
                    Path child_path = current_path;
                    child_path.push_back(i);
                    collect_removed(*arg[i], child_path);
                }
            }
            else if constexpr (!std::is_same_v<T, std::monostate>) {
                // 叶子节点（非 null）
                diffs_.push_back({DiffEntry::Type::Remove, current_path,
                                  value_to_string(val), ""});
            }
        }, val.data);
    }

    // 收集新增节点的所有叶子
    void collect_added(const Value& val, Path current_path) {
        std::visit([&](const auto& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, ValueMap>) {
                for (const auto& [k, v] : arg) {
                    Path child_path = current_path;
                    child_path.push_back(k);
                    collect_added(*v, child_path);
                }
            }
            else if constexpr (std::is_same_v<T, ValueVector>) {
                for (size_t i = 0; i < arg.size(); ++i) {
                    Path child_path = current_path;
                    child_path.push_back(i);
                    collect_added(*arg[i], child_path);
                }
            }
            else if constexpr (!std::is_same_v<T, std::monostate>) {
                // 叶子节点（非 null）
                diffs_.push_back({DiffEntry::Type::Add, current_path,
                                  "", value_to_string(val)});
            }
        }, val.data);
    }

public:
    // 主入口：比较两个 Value
    void diff(const Value& old_val, const Value& new_val) {
        diffs_.clear();
        diff_value(old_val, new_val, {});
    }

    const std::vector<DiffEntry>& get_diffs() const { return diffs_; }
    
    void clear() { diffs_.clear(); }
    
    bool has_changes() const { return !diffs_.empty(); }

    void print_diffs() const {
        if (diffs_.empty()) {
            std::cout << "  (无变化)\n";
            return;
        }
        for (const auto& d : diffs_) {
            std::string type_str;
            switch (d.type) {
                case DiffEntry::Type::Add:    type_str = "ADD   "; break;
                case DiffEntry::Type::Remove: type_str = "REMOVE"; break;
                case DiffEntry::Type::Change: type_str = "CHANGE"; break;
            }
            std::cout << "  " << type_str << " " << d.path_to_string();
            if (d.type == DiffEntry::Type::Change) {
                std::cout << ": " << d.old_value << " -> " << d.new_value;
            } else if (d.type == DiffEntry::Type::Add) {
                std::cout << ": " << d.new_value;
            } else {
                std::cout << ": " << d.old_value;
            }
            std::cout << "\n";
        }
    }
};

// ============================================================
// 完整 Diff 收集演示
// ============================================================
void demo_recursive_diff_collector()
{
    std::cout << "\n=== RecursiveDiffCollector 演示 ===\n\n";

    // 创建旧状态
    ValueMap user1;
    user1 = user1.set("name", immer::box<Value>{Value{std::string{"Alice"}}});
    user1 = user1.set("age", immer::box<Value>{Value{25}});

    ValueMap user2;
    user2 = user2.set("name", immer::box<Value>{Value{std::string{"Bob"}}});
    user2 = user2.set("age", immer::box<Value>{Value{30}});

    ValueVector users_old;
    users_old = users_old.push_back(immer::box<Value>{Value{user1}});
    users_old = users_old.push_back(immer::box<Value>{Value{user2}});

    ValueMap old_root;
    old_root = old_root.set("users", immer::box<Value>{Value{users_old}});
    old_root = old_root.set("version", immer::box<Value>{Value{1}});

    Value old_state{old_root};

    // 创建新状态（修改了一些内容）
    ValueMap user1_new;
    user1_new = user1_new.set("name", immer::box<Value>{Value{std::string{"Alice"}}});
    user1_new = user1_new.set("age", immer::box<Value>{Value{26}});  // 修改了 age
    user1_new = user1_new.set("email", immer::box<Value>{Value{std::string{"alice@x.com"}}}); // 新增

    ValueMap user3;
    user3 = user3.set("name", immer::box<Value>{Value{std::string{"Charlie"}}});
    user3 = user3.set("age", immer::box<Value>{Value{35}});

    ValueVector users_new;
    users_new = users_new.push_back(immer::box<Value>{Value{user1_new}});
    users_new = users_new.push_back(immer::box<Value>{Value{user2}});  // Bob 未变
    users_new = users_new.push_back(immer::box<Value>{Value{user3}});  // 新增 Charlie

    ValueMap new_root;
    new_root = new_root.set("users", immer::box<Value>{Value{users_new}});
    new_root = new_root.set("version", immer::box<Value>{Value{2}});  // 修改

    Value new_state{new_root};

    // 打印状态
    std::cout << "--- 旧状态 ---\n";
    print_value(old_state, "", 1);
    
    std::cout << "\n--- 新状态 ---\n";
    print_value(new_state, "", 1);

    // 使用 RecursiveDiffCollector 收集所有变化
    std::cout << "\n--- Diff 结果 ---\n";
    RecursiveDiffCollector collector;
    collector.diff(old_state, new_state);
    collector.print_diffs();

    std::cout << "\n共检测到 " << collector.get_diffs().size() << " 处变化\n";
    std::cout << "\n=== 演示结束 ===\n\n";
}

// ============================================================
// immer::diff 演示函数
// 展示如何使用 immer 原生的 diff 机制来检测变化
// 
// ★ 重要：immer::diff 只支持 map 和 set，不支持 vector！
//   （参见 immer/algorithm.hpp 第 299-305 行的注释）
// ============================================================
void demo_immer_diff()
{
    std::cout << "\n=== immer::diff 演示 ===\n\n";

    // --- immer::vector 的比较 ---
    // 注意：immer::diff 不支持 vector，需要手动比较
    std::cout << "--- immer::vector 比较 (手动方式) ---\n";

    ValueVector old_vec;
    old_vec = old_vec.push_back(immer::box<Value>{Value{std::string{"Alice"}}});
    old_vec = old_vec.push_back(immer::box<Value>{Value{std::string{"Bob"}}});
    old_vec = old_vec.push_back(immer::box<Value>{Value{std::string{"Charlie"}}});

    ValueVector new_vec;
    new_vec = new_vec.push_back(immer::box<Value>{Value{std::string{"Alice"}}});     // 未变
    new_vec = new_vec.push_back(immer::box<Value>{Value{std::string{"Bobby"}}});     // 修改
    new_vec = new_vec.push_back(immer::box<Value>{Value{std::string{"Charlie"}}});   // 未变
    new_vec = new_vec.push_back(immer::box<Value>{Value{std::string{"David"}}});     // 新增

    std::cout << "旧列表: [Alice, Bob, Charlie]\n";
    std::cout << "新列表: [Alice, Bobby, Charlie, David]\n\n";

    // 手动比较 vector（immer::diff 不支持 vector）
    std::cout << "手动比较结果:\n";
    
    size_t old_size = old_vec.size();
    size_t new_size = new_vec.size();
    size_t common_size = std::min(old_size, new_size);
    
    // 比较共有索引
    for (size_t i = 0; i < common_size; ++i) {
        const auto& old_box = old_vec[i];
        const auto& new_box = new_vec[i];
        
        auto* old_str = old_box->get_if<std::string>();
        auto* new_str = new_box->get_if<std::string>();
        
        if (old_str && new_str) {
            // 利用 immer::box 的 operator==（内部先比较指针）
            if (old_box == new_box) {
                std::cout << "  [" << i << "] 保留: " << *old_str << " (指针相同)\n";
            } else if (*old_str == *new_str) {
                std::cout << "  [" << i << "] 保留: " << *old_str << " (值相同)\n";
            } else {
                std::cout << "  [" << i << "] 修改: " << *old_str << " -> " << *new_str << "\n";
            }
        }
    }
    
    // 删除的尾部元素
    for (size_t i = common_size; i < old_size; ++i) {
        if (auto* str = old_vec[i]->get_if<std::string>()) {
            std::cout << "  [" << i << "] 删除: " << *str << "\n";
        }
    }
    
    // 新增的尾部元素
    for (size_t i = common_size; i < new_size; ++i) {
        if (auto* str = new_vec[i]->get_if<std::string>()) {
            std::cout << "  [" << i << "] 新增: " << *str << "\n";
        }
    }

    // --- immer::map 的 diff ---
    std::cout << "\n--- immer::map diff (使用 immer::diff) ---\n";

    ValueMap old_map;
    old_map = old_map.set("name", immer::box<Value>{Value{std::string{"Tom"}}});
    old_map = old_map.set("age", immer::box<Value>{Value{25}});
    old_map = old_map.set("city", immer::box<Value>{Value{std::string{"Beijing"}}});

    ValueMap new_map;
    new_map = new_map.set("name", immer::box<Value>{Value{std::string{"Tom"}}});      // 未变
    new_map = new_map.set("age", immer::box<Value>{Value{26}});                        // 修改
    new_map = new_map.set("email", immer::box<Value>{Value{std::string{"tom@x.com"}}}); // 新增
    // city 被删除

    std::cout << "旧 map: {name: Tom, age: 25, city: Beijing}\n";
    std::cout << "新 map: {name: Tom, age: 26, email: tom@x.com}\n\n";

    std::cout << "immer::diff 结果:\n";

    immer::diff(
        old_map,
        new_map,
        // 删除的 key-value
        [](const auto& removed) {
            std::cout << "  [删除] key=" << removed.first << "\n";
        },
        // 新增的 key-value
        [](const auto& added) {
            std::cout << "  [新增] key=" << added.first << "\n";
        },
        // 保留的 key-value (key 相同，value 可能不同)
        [](const auto& old_kv, const auto& new_kv) {
            // 利用 immer::box 的指针比较（结构共享特性）
            // 如果指针相同，说明值完全没变
            if (old_kv.second.get() == new_kv.second.get()) {
                std::cout << "  [保留] key=" << old_kv.first << " (指针相同，未变化)\n";
            } else {
                std::cout << "  [修改] key=" << old_kv.first << " (值已变化)\n";
            }
        }
    );

    std::cout << "\n=== diff 演示结束 ===\n\n";
}

} // namespace immer_lens


int main()
{
    using namespace immer_lens;
    auto loop  = lager::with_manual_event_loop{};
    auto store = lager::make_store<Action>(create_initial_state(), loop, lager::with_reducer(reducer));

    std::cout << "=== Simple Editor (Manual Event Loop) ===\n";

    while (true) {

        std::cout << "Current data:\n";
        print_value(store.get().data, "", 1);

        std::cout << "\nOperations:\n";
        std::cout << "1. Add item\n";
        std::cout << "2. Update item\n";
        std::cout << "U. Undo\n";
        std::cout << "R. Redo\n";
        std::cout << "D. Demo immer::diff (基础)\n";
        std::cout << "C. Demo RecursiveDiffCollector (完整)\n";
        std::cout << "Q. Quit\n";
        std::cout << "Choice: ";

        char choice;
        std::cin >> choice;
        std::cin.ignore();

        switch (choice) {
        case '1': {
            std::cout << "Enter item title: ";
            std::string title;
            std::getline(std::cin, title);
            store.dispatch(AddItem{title});
            break;
        }
        case '2': {
            std::cout << "Enter item index: ";
            size_t index;
            std::cin >> index;
            std::cin.ignore();

            std::cout << "Enter new title: ";
            std::string new_title;
            std::getline(std::cin, new_title);

            Path path = {std::string{"items"}, index, std::string{"title"}};
            store.dispatch(UpdateItem{path, new_title});
            break;
        }
        case 'U':
        case 'u':
            store.dispatch(Undo{});
            break;
        case 'R':
        case 'r':
            store.dispatch(Redo{});
            break;
        case 'D':
        case 'd':
            demo_immer_diff();
            break;
        case 'C':
        case 'c':
            demo_recursive_diff_collector();
            break;
        case 'Q':
        case 'q':
            return 0;
        default:
            std::cout << "Invalid choice!\n";
        }
    }
}