
# 🎓 核心依赖库全面学习总结

> 本文档是对 `pathlens` 项目核心依赖库的系统性学习总结，涵盖 **lager**、**immer**、**zug** 和 **Boost.Interprocess** 四大库。

---

## 目录

1. [Lager 库 (ELM 架构状态管理)](#一lager-库-elm-架构状态管理)
2. [Immer 库 (不可变数据结构)](#二immer-库-不可变数据结构)
3. [Zug 库 (Transducers/转换器)](#三zug-库-transducers转换器)
4. [Boost.Interprocess 库 (进程间通信)](#四boostinterprocess-库-进程间通信)
5. [四库协作关系](#五四库协作关系图)

---

## 一、Lager 库 (ELM 架构状态管理)

### 1. 核心理念

**Lager** 是一个将 **Elm Architecture** 引入 C++ 的状态管理库。其核心思想是：

- **单向数据流 (Unidirectional Data Flow):** 数据只能沿一个方向流动：Action → Reducer → State → View
- **不可变状态 (Immutable State):** 状态永远不会被原地修改，每次更新都产生新状态
- **纯函数更新 (Pure Reducer):** 状态更新逻辑是纯函数，便于测试和推理
- **值语义 (Value Semantics):** 一切皆值，避免共享可变状态带来的复杂性

### 2. ELM 架构核心组件

```
┌─────────────────────────────────────────────────────────────┐
│                      ELM Architecture                        │
│                                                              │
│    ┌─────────┐      ┌──────────┐      ┌─────────────┐       │
│    │  View   │─────▶│  Action  │─────▶│   Reducer   │       │
│    └─────────┘      └──────────┘      └──────────┬──┘       │
│         ▲                                        │           │
│         │              ┌─────────┐               │           │
│         └──────────────│  State  │◀──────────────┘           │
│                        └─────────┘                           │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

| 组件 | 职责 | C++ 实现 |
|------|------|----------|
| **Model (State)** | 应用程序的完整状态 | 不可变数据结构（通常使用 `immer` 容器） |
| **Action** | 描述"发生了什么"的消息 | `std::variant` 或带标签的结构体 |
| **Reducer** | 根据 Action 计算新状态 | 纯函数 `(State, Action) -> State` |
| **Effect** | 副作用（如 I/O、网络请求） | 返回 `effect<Action>` 或 `result<State, Action>` |
| **View** | 状态的可视化表示 | 接收状态的函数（可选） |

### 3. Store - 核心容器

`store` 是 Lager 的核心，它组合了所有组件：

```cpp
#include <lager/store.hpp>
#include <lager/event_loop/manual.hpp>

// 定义状态
struct app_state {
    int counter = 0;
    std::string message;
};

// 定义动作
struct increment {};
struct decrement {};
struct set_message { std::string text; };
using action = std::variant<increment, decrement, set_message>;

// 定义 reducer（纯函数）
app_state update(app_state state, action a) {
    return std::visit(lager::visitor{
        [&](increment) { 
            state.counter++; 
            return state; 
        },
        [&](decrement) { 
            state.counter--; 
            return state; 
        },
        [&](set_message msg) { 
            state.message = std::move(msg.text); 
            return state; 
        }
    }, a);
}

// 创建 store
auto store = lager::make_store<action>(
    app_state{},           // 初始状态
    update,                // reducer
    lager::with_manual_event_loop{}  // 事件循环
);

// 使用 store
store.dispatch(increment{});          // 发送动作
const app_state& current = store.get();  // 获取当前状态
```

### 4. Effects - 处理副作用

Effects 是处理副作用的机制。Reducer 保持纯净，副作用通过返回 `effect` 对象来延迟执行：

```cpp
#include <lager/effect.hpp>

// 使用 result 返回状态和副作用
using app_result = lager::result<app_state, action>;

app_result update_with_effects(app_state state, action a) {
    return std::visit(lager::visitor{
        [&](increment) -> app_result { 
            state.counter++;
            if (state.counter >= 10) {
                // 返回状态 + 副作用
                return {state, lager::effect<action>{
                    [](auto&& ctx) {
                        // 异步操作，完成后 dispatch 新动作
                        ctx.dispatch(set_message{"Counter reached 10!"});
                    }
                }};
            }
            return state;  // 只返回状态，无副作用
        },
        [&](decrement) -> app_result {
            state.counter--;
            return state;
        },
        [&](set_message msg) -> app_result {
            state.message = std::move(msg.text);
            return state;
        }
    }, a);
}
```

### 5. Cursors - 状态的透镜

**Cursor** 是 Lager 最强大的抽象之一，它提供了对状态子部分的"视图"：

```cpp
#include <lager/cursor.hpp>
#include <lager/lenses.hpp>

// 使用 lens 聚焦于状态的一部分
auto counter_cursor = store.zoom(lager::lenses::attr(&app_state::counter));

// 读取子状态
int count = counter_cursor.get();

// 通过 cursor 更新（需要配合 setter actions）
// counter_cursor.set(42);  // 如果定义了相应的 setter 逻辑
```

**Lens 的核心操作：**

| 操作 | 说明 |
|------|------|
| `view(lens, whole)` | 从整体中提取部分 |
| `set(lens, whole, part)` | 设置整体中的部分，返回新整体 |
| `over(lens, whole, fn)` | 对部分应用函数，返回新整体 |

**常用 Lens 组合器：**

```cpp
using namespace lager::lenses;

// 成员属性 lens
auto name_lens = attr(&Person::name);

// 组合 lens（从左到右聚焦）
auto street_lens = attr(&Person::address) | attr(&Address::street);

// 容器元素 lens
auto first_lens = at(0);  // 访问第一个元素
auto key_lens = at_key("name");  // 访问 map 中的键

// 可选值 lens
auto value_or_default = value_or(42);  // 处理 optional
```

### 6. Reader/Writer 分离

Lager 将读写能力分离为不同的类型：

| 类型 | 能力 | 用途 |
|------|------|------|
| `reader<T>` | 只读 | 组件只需要读取状态 |
| `writer<T, A>` | 只写 | 组件只需要发送动作 |
| `cursor<T, A>` | 读写 | 组件需要读写状态 |
| `store<A, M>` | 完整 | 应用程序根级别 |

```cpp
// reader：只读视图
void display_counter(lager::reader<int> counter) {
    std::cout << "Counter: " << counter.get() << std::endl;
}

// writer：只发送动作
void button_click(lager::writer<action> dispatcher) {
    dispatcher.dispatch(increment{});
}

// cursor：读写都可以
void counter_widget(lager::cursor<int, action> counter) {
    std::cout << counter.get() << std::endl;
    // 可以通过某种方式更新...
}
```

### 7. Context 和依赖注入

`context` 用于在 effects 中访问外部依赖：

```cpp
// 定义依赖
struct app_deps {
    std::function<void(std::string)> logger;
    http_client& http;
};

// effect 中使用依赖
lager::effect<action> log_effect(std::string msg) {
    return [msg](auto&& ctx) {
        auto& deps = ctx.get<app_deps>();
        deps.logger(msg);
    };
}

// 创建带依赖的 store
auto store = lager::make_store<action>(
    app_state{},
    update,
    lager::with_deps(app_deps{my_logger, my_http}),
    lager::with_manual_event_loop{}
);
```

### 8. Event Loops 集成

Lager 支持多种事件循环：

| Event Loop | 用途 |
|------------|------|
| `with_manual_event_loop` | 手动控制，用于测试 |
| `with_boost_asio_event_loop` | Boost.Asio 集成 |
| `with_qt_event_loop` | Qt 框架集成 |
| `with_sdl_event_loop` | SDL 游戏开发集成 |

```cpp
// Qt 集成示例
#include <lager/event_loop/qt.hpp>

auto store = lager::make_store<action>(
    app_state{},
    update,
    lager::with_qt_event_loop{*qApp}
);
```

### 9. 调试支持

**Time-Travel Debugging:**

```cpp
#include <lager/debug/debugger.hpp>

// 使用 debugger 包装 store
auto store = lager::make_store<action>(
    app_state{},
    update,
    lager::with_debugger
);

// 可以回溯到之前的状态
store.undo();
store.redo();
```

**HTTP Debugger（浏览器可视化）:**

```cpp
#include <lager/debug/http_server.hpp>

// 启动 HTTP 调试服务器
lager::debug::http_server server{store, 8080};
// 访问 http://localhost:8080 查看状态变化
```

### 10. 与 pathlens 项目的关系

`pathlens` 扩展了 `lager` 的核心概念，使其能够：

1. **跨进程状态共享:** 使用 Boost.Interprocess 将状态存储在共享内存中
2. **自定义内存策略:** 通过 `immer` 的 `memory_policy` 让容器使用共享内存
3. **Value 抽象:** 创建类似 JSON 的动态值类型，用于灵活的状态表示

```cpp
// pathlens 中的 SharedValue 概念
using SharedValue = BasicValue<shared_memory_policy>;

// 可以跨进程共享的 lager store
auto shared_store = make_shared_store<SharedAction>(
    SharedValue{},
    shared_reducer,
    shared_memory_segment
);
```

---

## 二、Immer 库 (不可变数据结构)

### 1. 核心理念

**Immer** 是一个 C++ 持久化（persistent）和不可变（immutable）数据结构库。其核心特点是：

- **不可变性 (Immutability):** 所有容器方法都是 `const` 的。操作不会修改原始数据，而是返回包含变更的新值。
- **持久性 (Persistence):** 旧值在修改后仍然存在且有效。
- **结构共享 (Structural Sharing):** 新值与旧值在内部共享未修改的部分，使得"复制"操作非常高效（通常是 O(log n) 或 O(1)）。

### 2. 核心容器类型

| 容器类型 | 说明 | 内部数据结构 |
|---------|------|-------------|
| `immer::vector<T>` | 不可变顺序容器，支持随机访问 | **RRB-Tree** (Relaxed Radix Balanced Tree) |
| `immer::flex_vector<T>` | 支持高效拼接的vector | RRB-Tree with size tables |
| `immer::map<K, V>` | 不可变哈希映射 | **CHAMP** (Compressed Hash-Array Mapped Prefix-tree) |
| `immer::set<T>` | 不可变哈希集合 | CHAMP |
| `immer::table<T>` | 类似map，但使用对象ID作为键 | CHAMP |
| `immer::array<T>` | 小型不可变数组（简单堆分配） | 简单堆数组 |
| `immer::box<T>` | 单值的不可变包装器 | 引用计数的堆分配值 |

### 3. 关键操作示例

```cpp
#include <immer/vector.hpp>
#include <immer/map.hpp>

// Vector 操作
const auto v0 = immer::vector<int>{};
const auto v1 = v0.push_back(13);      // v0 仍然是空的！
const auto v2 = v1.set(0, 42);          // v1[0] 仍然是 13

// Map 操作
auto m = immer::map<std::string, int>{};
m = m.set("hello", 1);
m = m.update("hello", [](int x){ return x + 1; });
const int* val = m.find("hello");  // 返回指针，找不到返回 nullptr
```

### 4. Transients (临时可变视图)

当需要进行批量操作时，**transient** 提供了一种高效的方式：

```cpp
immer::vector<int> myiota(immer::vector<int> v, int first, int last)
{
    auto t = v.transient();       // O(1) 转换为可变视图
    for (auto i = first; i < last; ++i)
        t.push_back(i);           // 原地修改
    return t.persistent();        // O(1) 转换回不可变
}
```

**transient 的工作原理：**
- 内部节点使用 "owned" 标志来跟踪当前 transient 是否独占该节点
- 修改时，如果节点被独占，则原地修改；否则创建副本
- 调用 `.persistent()` 时清除所有 "owned" 标志

### 5. Memory Policy (内存策略)

Immer 使用策略模式来定制内存管理行为：

```cpp
template <typename HeapPolicy,
          typename RefcountPolicy,
          typename LockPolicy,
          typename TransiencePolicy = ...,
          bool PreferFewerBiggerObjects = ...,
          bool UseTransientRValues = ...>
struct memory_policy;
```

| 策略组件 | 默认值 | 可选值 |
|---------|--------|--------|
| **Heap** | `free_list_heap_policy<cpp_heap>` | `gc_heap`, 自定义堆 |
| **Refcount** | `refcount_policy` (线程安全) | `unsafe_refcount_policy`, `no_refcount_policy` |
| **Lock** | `spinlock_policy` | `no_lock_policy` |

这种设计使得 `pathlens` 项目能够创建使用共享内存堆的自定义内存策略！

### 6. `immer::atom<T>` - 线程安全的状态容器

```cpp
immer::atom<immer::map<std::string, int>> state;

// 线程安全的原子更新
state.update([](auto m) {
    return m.set("counter", m["counter"] + 1);
});

// 读取当前状态
auto current = state.load();
```

`atom` 的实现根据内存策略自动选择：
- **无引用计数策略** (如 GC 堆): 使用 `std::atomic` 进行无锁原子操作
- **引用计数策略**: 使用互斥锁保护

### 7. `immer::box<T>` - 递归数据结构的基础

```cpp
struct tree_node {
    int value;
    immer::vector<immer::box<tree_node>> children;  // 递归！
};
```

`box` 是一个轻量级的堆分配、引用计数的智能指针，使递归数据结构成为可能。

---

## 三、Zug 库 (Transducers/转换器)

### 1. 核心理念

**Zug** 提供 **Transducers** (转换器) —— 一种可组合的、独立于序列源的顺序转换抽象。

**核心洞察:** 最通用的序列算法是 `reduce`/`fold`/`accumulate`。Transducer 就是对 reducing function 的转换。

```
Transducer = (ReducingFunction) -> ReducingFunction
```

### 2. 为什么需要 Transducers？

| 传统方法 | 问题 |
|---------|-----|
| STL 算法 | 只能用于迭代器/范围 |
| Range Views | 只能用于拉取式 (pull-based) 序列 |
| RxCpp 风格 | 需要为每种序列类型重写所有算法 |

**Transducers 的解决方案:** 算法变换与序列源完全解耦！

### 3. 核心概念

```cpp
// 一个简单的 transducer: map
template <typename MappingT>
auto map(MappingT&& mapping) {
    return comp([=](auto&& step) {        // 接收下一个 reducing function
        return [=](auto&& s, auto&&... is) {  // 返回新的 reducing function
            return step(s, mapping(is...));   // 转换输入，传递给下一个
        };
    });
}
```

### 4. Transducer 组合

```cpp
// 使用 | 操作符组合 (从左到右读取！)
auto xf = zug::filter([](int x) { return x > 0; })
        | zug::map([](int x) { return std::to_string(x); })
        | zug::take(10);

// 组合顺序说明：
// 数据流向: filter -> map -> take
// 函数组合: take(map(filter(step)))  (右到左包装)
```

**组合的数学表示：**
```
comp(f, g, h) = h ∘ g ∘ f
(f | g | h)(step) = h(g(f(step)))
```

### 5. 核心函数

| 函数 | 用途 | 示例 |
|------|-----|------|
| `zug::reduce(step, state, ranges...)` | 对范围应用 reducing function | `reduce(std::plus<>{}, 0, v)` |
| `zug::transduce(xf, step, state, ranges...)` | 应用 transducer 后 reduce | `transduce(map(f), std::plus<>{}, 0, v)` |
| `zug::into(collection, xf, ranges...)` | 将转换结果收集到容器 | `into(std::vector<int>{}, map(f), v)` |
| `zug::into_vector(xf, ranges...)` | 收集到 vector | `into_vector(filter(pred), v)` |
| `zug::sequence(xf, ranges...)` | 创建惰性迭代器 | `for (auto x : sequence(xf, v))` |
| `zug::run(xf)` | 执行有副作用的管道 | `run(each([](int x) { print(x); }))` |

### 6. 常用 Transducers

| Transducer | 功能 | 示例 |
|-----------|------|------|
| `map(f)` | 对每个输入应用 f | `map([](int x) { return x * 2; })` |
| `filter(pred)` | 保留满足谓词的元素 | `filter([](int x) { return x > 0; })` |
| `take(n)` | 只取前 n 个元素 | `take(5)` |
| `drop(n)` | 跳过前 n 个元素 | `drop(3)` |
| `cat` | 展平嵌套序列 | `map(get_children) \| cat` |
| `mapcat(f)` | map + cat 的组合 | `mapcat([](int x) { return range(x); })` |
| `enumerate` | 添加索引 | 输出 `(0, elem0), (1, elem1), ...` |
| `zip` | 将多个输入组合为 tuple | 多输入范围 |
| `dedupe` | 去除连续重复 | `[1,1,2,2,3] -> [1,2,3]` |
| `partition(n)` | 按 n 个一组分区 | `[1,2,3,4,5,6] -> [[1,2,3], [4,5,6]]` |
| `each(f)` | 对每个元素执行副作用 | `each([](int x) { log(x); })` |

### 7. 状态管理

Transducers 可以携带状态（如 `enumerate` 需要计数器）：

```cpp
// 状态包装函数
state_wrap(s, data);      // 将数据附加到状态
state_unwrap(s);          // 获取底层状态  
state_data(s, init_fn);   // 获取附加数据，或用 init_fn 初始化

// 状态完成检查
state_complete(s);        // 调用完成回调，获取最终状态
state_is_reduced(s);      // 检查是否应该提前终止
```

### 8. Skip 机制

当 transducer 可能跳过调用下一个 step 时（如 `filter`），需要特殊处理以保持类型一致：

```cpp
auto filter(Predicate pred) {
    return comp([=](auto step) {
        return [=](auto s, auto... is) {
            return pred(is...)
                ? call(step, s, is...)    // 调用，返回可能包装的状态
                : skip(step, s, is...);   // 跳过，返回类型兼容的状态
        };
    });
}
```

**`skip` 的工作原理：**
- `skip(step, s, is...)` 返回一个 `skip_state`，它是 `std::variant<ActualState, SkippedState>` 的包装
- 这确保了无论是否调用 step，返回类型都是一致的

### 9. 实际使用示例

```cpp
#include <zug/into.hpp>
#include <zug/transducer/filter.hpp>
#include <zug/transducer/map.hpp>

std::vector<int> input = {1, -2, 3, -4, 5};

// 过滤正数，然后翻倍
auto result = zug::into(
    std::vector<int>{},
    zug::filter([](int x) { return x > 0; })
        | zug::map([](int x) { return x * 2; }),
    input
);
// result = {2, 6, 10}
```

---

## 四、Boost.Interprocess 库 (进程间通信)

### 1. 核心功能

| 类别 | 功能 |
|------|------|
| **共享内存** | `shared_memory_object`, `windows_shared_memory`, `xsi_shared_memory` |
| **内存映射文件** | `file_mapping`, `mapped_region` |
| **同步原语** | 互斥锁、条件变量、信号量（可放置于共享内存） |
| **命名对象** | 支持在共享内存中创建命名对象 |
| **容器/分配器** | STL 兼容的容器和分配器 |

### 2. 基本使用模式

**创建和映射共享内存：**

```cpp
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>

using namespace boost::interprocess;

// 1. 创建共享内存对象
shared_memory_object shm(create_only, "MySharedMemory", read_write);

// 2. 设置大小
shm.truncate(1000);

// 3. 映射到进程地址空间
mapped_region region(shm, read_write);

// 4. 使用内存
void* addr = region.get_address();
std::size_t size = region.get_size();
std::memset(addr, 0, size);
```

### 3. 资源生命周期

**关键概念：命名资源需要显式删除！**

| 操作 | 类比 | POSIX 对应 |
|------|------|-----------|
| 构造函数 | `fstream` 构造 | `open`/`shm_open` |
| 析构函数 | `fstream` 析构 | `close` |
| `remove()` | `std::remove` | `unlink`/`shm_unlink` |

**RAII 模式删除资源：**

```cpp
struct shm_remove {
    shm_remove()  { shared_memory_object::remove("MyShm"); }
    ~shm_remove() { shared_memory_object::remove("MyShm"); }
} remover;
```

### 4. Windows 共享内存特性

`windows_shared_memory` 与 POSIX 共享内存有重要区别：

| 特性 | `shared_memory_object` (POSIX) | `windows_shared_memory` |
|------|--------------------------------|------------------------|
| **底层实现** | 内存映射文件模拟 | 原生 Windows 共享内存 |
| **生命周期** | 内核/文件系统持久性 | 进程持久性（最后一个进程退出时销毁） |
| **创建时大小** | 创建后可 `truncate` | 必须在创建时指定 |
| **跨会话共享** | 需要路径配置 | 需要 `"Global\\"` 前缀 |

```cpp
// Windows 原生共享内存
windows_shared_memory shm(create_only, "MyShm", read_write, 1000);

// 注意：没有 remove() 方法，因为它随进程自动销毁
```

### 5. `offset_ptr` - 共享内存指针

**问题:** 普通指针在共享内存中无效，因为不同进程将共享内存映射到不同的虚拟地址！

**解决方案:** `offset_ptr` 存储的是相对于自身的偏移量，而非绝对地址。

```cpp
#include <boost/interprocess/offset_ptr.hpp>

struct list_node {
    offset_ptr<list_node> next;  // 存储相对于 this 的偏移
    int value;
};

// 使用方式与普通指针相同
list_node* raw_ptr = node.next.get();  // 转换为原始指针
node.next = another_node;              // 自动计算偏移
```

**工作原理：**
```
offset = target_address - this_pointer_address

当访问时:
target_address = this_pointer_address + offset
```

### 6. `managed_shared_memory` - 高级 API

```cpp
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/containers/vector.hpp>
#include <boost/interprocess/allocators/allocator.hpp>

using namespace boost::interprocess;

// 创建托管共享内存段
managed_shared_memory segment(create_only, "MySegment", 65536);

// 在共享内存中构造命名对象
MyType* instance = segment.construct<MyType>
    ("MyInstance")     // 对象名称
    (arg1, arg2);      // 构造函数参数

// 构造数组
int* arr = segment.construct<int>("MyArray")[10](99);  // 10个元素，初始化为99

// 在另一个进程中查找
auto res = segment.find<MyType>("MyInstance");
if (res.first) {
    MyType* ptr = res.first;
    std::size_t count = res.second;  // 对象数量（数组时有用）
}

// 匿名对象（无名称）
MyType* anon = segment.construct<MyType>(anonymous_instance)(args...);

// 销毁对象
segment.destroy<MyType>("MyInstance");
segment.destroy_ptr(anon);
```

### 7. 共享内存中的 STL 容器

```cpp
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/containers/vector.hpp>
#include <boost/interprocess/containers/map.hpp>
#include <boost/interprocess/containers/string.hpp>
#include <boost/interprocess/allocators/allocator.hpp>

using namespace boost::interprocess;

// 定义使用共享内存分配器的类型
typedef allocator<int, managed_shared_memory::segment_manager> ShmemAllocator;
typedef vector<int, ShmemAllocator> ShmemVector;

managed_shared_memory segment(create_only, "MySegment", 65536);

// 获取分配器
ShmemAllocator alloc(segment.get_segment_manager());

// 在共享内存中构造 vector
ShmemVector* vec = segment.construct<ShmemVector>("MyVector")(alloc);
vec->push_back(1);
vec->push_back(2);
vec->push_back(3);
```

### 8. 同步原语

```cpp
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>

struct shared_data {
    interprocess_mutex mutex;
    interprocess_condition cond;
    int value;
};

// 在共享内存中创建
shared_data* data = segment.construct<shared_data>("SharedData")();

// 使用
{
    scoped_lock<interprocess_mutex> lock(data->mutex);
    data->value = 42;
    data->cond.notify_one();
}

// 等待
{
    scoped_lock<interprocess_mutex> lock(data->mutex);
    data->cond.wait(lock, [&]{ return data->value != 0; });
}
```

### 9. 与 `pathlens` 项目的关联

`pathlens` 项目使用 Boost.Interprocess 的方式非常巧妙：

```cpp
// shared_value.h 中的实现思路
struct shared_heap {
    static void* allocate(size_t size) {
        // 从共享内存区域进行 bump allocation
        auto ptr = current_ptr;
        current_ptr += size;
        return ptr;
    }
    static void deallocate(size_t, void*) {} // no-op，整体释放
};

using shared_memory_policy = immer::memory_policy<
    immer::heap_policy<shared_heap>,
    immer::no_refcount_policy,    // 共享内存中不使用引用计数
    immer::no_lock_policy,
    immer::no_transience_policy,
    false>;

using SharedValue = BasicValue<shared_memory_policy>;
```

**关键设计决策：**

1. **固定地址映射:** 使用固定的基地址映射共享内存，确保指针跨进程有效
2. **自定义 immer 内存策略:** 将 immer 容器的堆分配重定向到共享内存
3. **Bump Allocator:** 只分配不释放，批量使用后整体释放整个共享内存段
4. **无引用计数:** 共享内存中的对象生命周期由外部管理

---

## 五、四库协作关系图

```
┌─────────────────────────────────────────────────────────────────┐
│                       lager (应用层)                             │
│  ┌──────────┐    ┌──────────┐    ┌──────────────────────────┐   │
│  │  store   │────│ reducer  │────│     cursors/lenses       │   │
│  └──────────┘    └──────────┘    └──────────────────────────┘   │
│       │               │                      │                   │
│       │          使用不可变            使用 zug 进行             │
│       │          状态更新              cursor 变换               │
└───────┼───────────────┼──────────────────────┼───────────────────┘
        │               │                      │
        ▼               ▼                      ▼
┌─────────────────┐ ┌─────────────┐ ┌──────────────────────┐
│     immer       │ │    zug      │ │ Boost.Interprocess   │
│  (不可变数据)    │ │ (转换管道)   │ │   (共享内存)          │
│                 │ │             │ │                      │
│ • vector/map    │ │ • map/filter│ │ • shared_memory      │
│ • memory_policy │ │ • comp      │ │ • mapped_region      │
│ • structural    │ │ • transduce │ │ • offset_ptr         │
│   sharing       │ │             │ │ • managed_segment    │
└────────┬────────┘ └─────────────┘ └──────────┬───────────┘
         │                                     │
         │         自定义内存策略               │
         └──────────────┬──────────────────────┘
                        ▼
           ┌────────────────────────┐
           │    pathlens 项目       │
           │                        │
           │  SharedValue = immer   │
           │    容器 + Boost.IPC    │
           │    共享内存策略         │
           │                        │
           │  实现跨进程共享的       │
           │  不可变数据结构         │
           └────────────────────────┘
```

---

## 六、关键要点总结

### Lager

| 概念 | 关键点 |
|------|--------|
| 单向数据流 | Action → Reducer → State → View |
| Store | 组合状态、reducer、事件循环的核心容器 |
| Effect | 副作用的延迟执行机制 |
| Cursor | 状态子部分的透镜视图 |
| Reader/Writer | 读写能力的分离抽象 |
| Context | 依赖注入机制 |

### Immer

| 概念 | 关键点 |
|------|--------|
| 不可变性 | 所有方法返回新值，原值不变 |
| 结构共享 | 通过共享内部节点避免深拷贝 |
| Transient | 批量更新时的临时可变视图 |
| Memory Policy | 可定制的内存管理策略 |
| Box | 实现递归数据结构的关键 |
| Atom | 线程安全的状态容器 |

### Zug

| 概念 | 关键点 |
|------|--------|
| Transducer | 对 reducing function 的高阶变换 |
| 组合 | 使用 `\|` 从左到右组合 |
| Skip | 条件跳过时保持类型一致的机制 |
| State | 有状态 transducer 的状态管理 |
| 源无关 | 同一 transducer 适用于任何序列类型 |

### Boost.Interprocess

| 概念 | 关键点 |
|------|--------|
| 资源生命周期 | 析构只关闭句柄，需显式 `remove` |
| offset_ptr | 跨进程有效的相对指针 |
| managed_segment | 高级 API，支持命名对象 |
| 分配器 | 使 STL 容器可用于共享内存 |
| Windows 差异 | 原生共享内存有进程生命周期 |

---

## 七、参考资源

- [Immer 官方文档](https://sinusoid.es/immer/)
- [Zug 官方文档](https://sinusoid.es/zug/)
- [Boost.Interprocess 文档](https://www.boost.org/doc/libs/release/doc/html/interprocess.html)
- [Lager 官方文档](https://sinusoid.es/lager/)

---

*文档生成日期: 2024年12月*
