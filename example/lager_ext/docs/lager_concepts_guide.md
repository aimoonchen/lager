
# Lager 核心概念完整指南

本文档介绍 lager 库中的所有核心概念：store、cursor、sensor、state、lens、effects、deps、context，以及它们如何协同工作。

## 架构概览

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              Lager Architecture                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                            STORE                                     │   │
│  │  ┌─────────┐    dispatch()    ┌─────────┐                           │   │
│  │  │ Action  │ ───────────────► │ Reducer │                           │   │
│  │  └─────────┘                  └────┬────┘                           │   │
│  │                                    │                                 │   │
│  │                    ┌───────────────┼───────────────┐                │   │
│  │                    │               │               │                │   │
│  │                    ▼               ▼               ▼                │   │
│  │              ┌─────────┐    ┌───────────┐    ┌─────────┐           │   │
│  │              │  Model  │    │  Effect   │    │  Deps   │           │   │
│  │              │ (State) │    │(Side-Eff) │    │(Service)│           │   │
│  │              └────┬────┘    └─────┬─────┘    └────┬────┘           │   │
│  │                   │               │               │                 │   │
│  │                   │               └───────┬───────┘                 │   │
│  │                   │                       │                         │   │
│  │                   │               ┌───────▼───────┐                 │   │
│  │                   │               │    Context    │                 │   │
│  │                   │               │ (dispatch+deps)│                 │   │
│  │                   │               └───────────────┘                 │   │
│  └───────────────────┼─────────────────────────────────────────────────┘   │
│                      │                                                      │
│        ┌─────────────┼─────────────┐                                       │
│        │             │             │                                        │
│        ▼             ▼             ▼                                        │
│   ┌─────────┐   ┌─────────┐   ┌─────────┐                                  │
│   │ Cursor  │   │ Reader  │   │  Lens   │  ← 访问/修改 State 的方式        │
│   │(R/W)    │   │(R only) │   │(Focus)  │                                  │
│   └─────────┘   └─────────┘   └─────────┘                                  │
│                                                                             │
│   ┌─────────────────────────────────────────────────────────────────────┐  │
│   │  Independent State Containers (outside store)                        │  │
│   │  ┌─────────┐        ┌─────────┐                                     │  │
│   │  │  State  │        │ Sensor  │                                     │  │
│   │  │ (R/W)   │        │(R only) │                                     │  │
│   │  └─────────┘        └─────────┘                                     │  │
│   └─────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 概念详解

| 概念 | 类型 | 作用 |
|------|------|------|
| **Store** | 核心容器 | 包含 Model + Reducer + EventLoop，管理整个应用状态 |
| **Model** | 数据 | 应用的不可变状态 |
| **Action** | 数据 | 描述"发生了什么"的事件 |
| **Reducer** | 函数 | `(Model, Action) -> Model` 或 `-> pair<Model, Effect>` |
| **Effect** | 函数 | 副作用，如 IO、网络请求、定时器 |
| **Deps** | 依赖 | 注入到 Effect 中的服务（如 HTTP 客户端） |
| **Context** | 上下文 | 包含 `dispatch()` 和 `deps`，传给 Effect |
| **Reader** | 只读视图 | 可以读取和监听状态变化 |
| **Cursor** | 读写视图 | Reader + 写入能力 |
| **Lens** | 聚焦器 | 聚焦到状态的某个部分 |
| **State** | 独立状态 | Store 之外的可观察状态容器（可读写） |
| **Sensor** | 独立只读 | Store 之外的只读值来源（如系统时间） |

## 完整示例代码

```cpp
// lager_complete_example.cpp
// 展示 lager 所有核心概念的最小示例

#include <lager/store.hpp>
#include <lager/state.hpp>
#include <lager/sensor.hpp>
#include <lager/cursor.hpp>
#include <lager/lens.hpp>
#include <lager/lenses/attr.hpp>
#include <lager/watch.hpp>
#include <lager/effect.hpp>
#include <lager/deps.hpp>
#include <lager/context.hpp>
#include <lager/event_loop/manual.hpp>

#include <iostream>
#include <string>
#include <variant>
#include <chrono>

// ============================================================
// 1. MODEL - 应用状态
// ============================================================
struct AppModel {
    std::string username;
    int counter = 0;
    bool logged_in = false;
};

// 用于 lens 访问的属性宏（或手动定义）
LAGER_STRUCT(AppModel, username, counter, logged_in);

// ============================================================
// 2. ACTIONS - 描述事件
// ============================================================
struct Increment { int amount = 1; };
struct Decrement { int amount = 1; };
struct Login { std::string user; };
struct Logout {};
struct FetchDataComplete { std::string data; };

using AppAction = std::variant<
    Increment, 
    Decrement, 
    Login, 
    Logout, 
    FetchDataComplete
>;

// ============================================================
// 3. DEPS - 依赖注入的服务
// ============================================================
struct Logger {
    void log(const std::string& msg) const {
        std::cout << "[LOG] " << msg << std::endl;
    }
};

struct DataService {
    std::string fetch_data() const {
        return "Data from server";
    }
};

// ============================================================
// 4. EFFECT - 副作用
// ============================================================
// Effect 签名: (Context&) -> void 或 -> future
// Context 提供 dispatch() 和 deps

using AppDeps = lager::deps<Logger&, DataService&>;
using AppEffect = lager::effect<AppAction, AppDeps>;

// ============================================================
// 5. REDUCER - 纯函数处理状态转换
// ============================================================
// 返回类型: Model 或 pair<Model, Effect>

std::pair<AppModel, AppEffect> update(AppModel model, AppAction action) {
    return std::visit(lager::visitor{
        [&](Increment a) -> std::pair<AppModel, AppEffect> {
            model.counter += a.amount;
            // 返回无副作用
            return {model, lager::noop};
        },
        
        [&](Decrement a) -> std::pair<AppModel, AppEffect> {
            model.counter -= a.amount;
            return {model, lager::noop};
        },
        
        [&](Login a) -> std::pair<AppModel, AppEffect> {
            model.username = a.user;
            model.logged_in = true;
            
            // 返回副作用：登录后获取数据
            return {model, [](auto& ctx) {
                // 从 context 获取 deps
                auto& logger = ctx.template get<Logger&>();
                auto& service = ctx.template get<DataService&>();
                
                logger.log("User logged in, fetching data...");
                std::string data = service.fetch_data();
                
                // 通过 context dispatch 新 action
                ctx.dispatch(FetchDataComplete{data});
            }};
        },
        
        [&](Logout a) -> std::pair<AppModel, AppEffect> {
            model.username = "";
            model.logged_in = false;
            
            return {model, [](auto& ctx) {
                ctx.template get<Logger&>().log("User logged out");
            }};
        },
        
        [&](FetchDataComplete a) -> std::pair<AppModel, AppEffect> {
            // 处理异步获取的数据
            return {model, [data = a.data](auto& ctx) {
                ctx.template get<Logger&>().log("Received: " + data);
            }};
        },
    }, action);
}

// ============================================================
// 6. MAIN - 组装所有组件
// ============================================================
int main() {
    // --- 创建依赖服务 ---
    Logger logger;
    DataService data_service;
    
    // --- 创建事件循环 ---
    auto loop = lager::with_manual_event_loop{};
    
    // --- 创建 STORE ---
    auto store = lager::make_store<AppAction>(
        AppModel{"", 0, false},              // 初始状态
        loop,                                 // 事件循环
        lager::with_reducer(update),          // reducer
        lager::with_deps(                     // 依赖注入
            std::ref(logger),
            std::ref(data_service)
        )
    );
    
    // ============================================================
    // 7. READER & WATCH - 监听状态变化
    // ============================================================
    lager::reader<AppModel> reader = store;  // store 可以转为 reader
    
    lager::watch(reader, [](const AppModel& m) {
        std::cout << "[WATCH] State changed: "
                  << "user=" << m.username 
                  << ", counter=" << m.counter 
                  << ", logged_in=" << m.logged_in << std::endl;
    });
    
    // ============================================================
    // 8. LENS - 聚焦到状态的某个部分
    // ============================================================
    // 创建聚焦到 counter 的 lens
    auto counter_lens = lager::lenses::attr(&AppModel::counter);
    
    // 使用 lens 读取
    int current_counter = lager::view(counter_lens, store.get());
    std::cout << "Counter via lens: " << current_counter << std::endl;
    
    // 使用 zoom 创建聚焦的 cursor
    auto counter_cursor = store.zoom(counter_lens);
    std::cout << "Counter via cursor: " << counter_cursor.get() << std::endl;
    
    // ============================================================
    // 9. CURSOR - 读写视图
    // ============================================================
    lager::cursor<AppModel> cursor = store;  // store 也是 cursor
    
    // cursor 可以直接设置值（绕过 reducer，慎用）
    // cursor.set(AppModel{"direct", 100, true});
    
    // ============================================================
    // 10. STATE - 独立的可观察状态容器
    // ============================================================
    // state 是 store 之外的独立状态，不通过 reducer
    lager::state<int> external_counter{42};
    
    lager::watch(external_counter, [](int v) {
        std::cout << "[EXTERNAL STATE] Changed to: " << v << std::endl;
    });
    
    // 直接修改 state
    external_counter.set(100);
    lager::commit(external_counter);  // 触发通知
    
    // ============================================================
    // 11. SENSOR - 只读的外部值来源
    // ============================================================
    // sensor 从外部获取值（如系统时间、配置等）
    auto time_sensor = lager::make_sensor([]() {
        return std::chrono::system_clock::now();
    });
    
    // 可以 watch sensor
    lager::watch(time_sensor, [](auto time) {
        auto t = std::chrono::system_clock::to_time_t(time);
        std::cout << "[SENSOR] Time updated: " << std::ctime(&t);
    });
    
    // ============================================================
    // 12. 运行：Dispatch Actions
    // ============================================================
    std::cout << "\n=== Dispatching Actions ===" << std::endl;
    
    // dispatch action 到 store
    store.dispatch(Increment{5});
    loop.step();  // manual event loop 需要手动 step
    
    store.dispatch(Login{"Alice"});
    loop.step();  // 处理 Login
    loop.step();  // 处理 Login 产生的 Effect (FetchDataComplete)
    
    store.dispatch(Decrement{2});
    loop.step();
    
    store.dispatch(Logout{});
    loop.step();
    
    // ============================================================
    // 13. CONTEXT - 在 Effect 中使用
    // ============================================================
    // Context 在 Effect 内部使用，提供:
    // - ctx.dispatch(action): 派发新 action
    // - ctx.get<T>(): 获取依赖
    // (见上面 update 函数中的 Effect 实现)
    
    std::cout << "\n=== Final State ===" << std::endl;
    std::cout << "Username: " << store->username << std::endl;
    std::cout << "Counter: " << store->counter << std::endl;
    std::cout << "Logged in: " << store->logged_in << std::endl;
    
    return 0;
}
```

## 概念总结表

| 概念 | 创建方式 | 主要用途 |
|------|---------|---------|
| **Store** | `lager::make_store<Action>(model, loop, ...)` | 应用状态的核心容器 |
| **Action** | `std::variant<A1, A2, ...>` | 描述事件 |
| **Reducer** | `update(Model, Action) -> Model/pair` | 状态转换逻辑 |
| **Effect** | `lager::effect<Action, Deps>` | 副作用函数 |
| **Deps** | `lager::deps<Service1&, Service2&>` | 依赖类型列表 |
| **Context** | Effect 参数 | 提供 dispatch + deps |
| **Reader** | `lager::reader<T> = store` | 只读状态视图 |
| **Cursor** | `lager::cursor<T> = store` | 读写状态视图 |
| **Lens** | `lager::lenses::attr(&T::member)` | 聚焦到状态的某部分 |
| **State** | `lager::state<T>{initial}` | 独立可观察状态 |
| **Sensor** | `lager::make_sensor(fn)` | 只读外部值来源 |
| **Watch** | `lager::watch(observable, callback)` | 监听变化 |
| **Commit** | `lager::commit(state)` | 触发 state 通知 |

## 数据流图

```
User Action
    │
    ▼
store.dispatch(action)
    │
    ▼
┌───────────────────┐
│  Event Loop       │  ← 确保单线程处理
└─────────┬─────────┘
          │
          ▼
┌───────────────────┐
│  Reducer          │  ← 纯函数，可测试
│  (model, action)  │
│        │          │
│        ▼          │
│  new_model        │
│  + effect         │
└─────────┬─────────┘
          │
    ┌─────┴─────┐
    │           │
    ▼           ▼
new_model    effect(ctx)
    │           │
    ▼           ▼
watchers     side-effects
notified     executed
    │           │
    ▼           ▼
UI update    ctx.dispatch()
             (new actions)
```

## Reader vs Cursor vs State vs Sensor

```
                    ┌─────────────┐
                    │   Store     │
                    │  (核心)     │
                    └──────┬──────┘
                           │
           ┌───────────────┼───────────────┐
           │               │               │
           ▼               ▼               ▼
    ┌─────────────┐ ┌─────────────┐ ┌─────────────┐
    │   Reader    │ │   Cursor    │ │    Lens     │
    │  (只读)     │ │  (读写)     │ │  (聚焦)     │
    │             │ │             │ │             │
    │  .get()     │ │  .get()     │ │  view()     │
    │  watch()    │ │  .set()     │ │  set()      │
    │             │ │  watch()    │ │  over()     │
    └─────────────┘ └─────────────┘ └─────────────┘


    ┌─────────────────────────────────────────────┐
    │         独立状态容器 (Store 之外)            │
    │                                             │
    │  ┌─────────────┐       ┌─────────────┐     │
    │  │   State     │       │   Sensor    │     │
    │  │  (读写)     │       │  (只读)     │     │
    │  │             │       │             │     │
    │  │  .get()     │       │  .get()     │     │
    │  │  .set()     │       │  watch()    │     │
    │  │  watch()    │       │  recompute()│     │
    │  │  commit()   │       │             │     │
    │  └─────────────┘       └─────────────┘     │
    │                                             │
    └─────────────────────────────────────────────┘
```

## 使用场景对比

| 场景 | 推荐使用 |
|------|---------|
| 应用主状态管理 | **Store** |
| UI 组件只需读取状态 | **Reader** |
| UI 组件需要修改状态 | **Cursor** (通过 dispatch) 或 **Lens** (聚焦后 set) |
| 聚焦到嵌套状态的某部分 | **Lens** + `store.zoom(lens)` |
| 独立于 Store 的临时状态 | **State** |
| 外部只读数据源（时间、配置等） | **Sensor** |
| 监听任何状态变化 | **watch()** |
| 批量修改后统一通知 | **commit()** |

## 注意事项

1. **Store 是单向数据流的核心**，所有状态变更应该通过 `dispatch(action)` → `reducer` 的流程

2. **Cursor.set() 绕过 Reducer**，应谨慎使用，可能破坏状态一致性

3. **Effect 是处理副作用的唯一方式**，保持 Reducer 纯净

4. **Deps 通过依赖注入**，使 Effect 可测试（可以注入 mock 服务）

5. **State 和 Sensor 是独立于 Store 的**，适用于不需要 Redux 模式的简单场景

6. **Lens 是函数式的聚焦工具**，可以组合使用访问深层嵌套结构
