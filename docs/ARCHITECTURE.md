# cppcoro 技术架构文档（教学版）

> 基于 `simake2017/cppcoro`（fork 自 Lewis Baker 原版，tag 未变动）
> 编译环境：GCC 10 + `-fcoroutines`（已附兼容 shim，见第 7 节）
> 阅读对象：已实现过迷你协程框架（`TaskT` + `MkEventLoop`）的开发者

---

## 1. 项目定位

cppcoro 是 C++20 协程标准的**参考级实现**，作者 Lewis Baker 是协程标准的核心设计者。它提供：

- 协程任务抽象（`task` / `shared_task` / `generator`）
- 协程同步原语（`async_mutex` / `event` / `sequence_barrier` …）
- 调度器（`io_service` / `static_thread_pool` / `inline_scheduler`）
- 组合子（`when_all` / `fmap` / `sync_wait` / `schedule_on`）
- 取消机制（`cancellation_source` / `cancellation_token`）
- IO 集成（socket、Linux AIO 文件）

**核心哲学**：协程本身不知道自己在哪个线程跑、被谁调度——调度完全由 awaitable 的实现和外部调度器决定。这与 deploy 项目里 `TaskT` 与 `MkEventLoop` 的分工完全一致。

---

## 2. 整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        使用者 (demo.cpp)                         │
│        co_await / co_return / sync_wait / when_all              │
├──────────────────────────────┬──────────────────────────────────┤
│        组合层 (全头文件)      │        任务抽象 (全头文件)        │
│  when_all  when_all_ready    │  task<T>      shared_task<T>     │
│  fmap      sync_wait         │  generator<T> async_generator<T> │
│  schedule_on  resume_on      │  make_task                       │
├──────────────────────────────┴──────────────────────────────────┤
│                     同步原语 (头文件 + lib/*.cpp)                 │
│  async_mutex   async_manual_reset_event  async_auto_reset_event │
│  async_latch   single_consumer_event     sequence_barrier       │
│  single_producer_sequencer  multi_producer_sequencer            │
├─────────────────────────────────────────────────────────────────┤
│                        调度层 (核心!)                            │
│  io_service (epoll/IOCP 事件循环)   static_thread_pool (线程池)  │
│  inline_scheduler (同线程立即执行)  round_robin_scheduler        │
│  统一概念: schedule() 返回 awaitable → co_await 即"被调度"        │
├─────────────────────────────────────────────────────────────────┤
│                        取消机制 (lib/*.cpp)                      │
│  cancellation_source → cancellation_token → cancellation_registration │
├─────────────────────────────────────────────────────────────────┤
│                      IO 集成 (lib/*.cpp, 平台相关)               │
│  net/socket (非阻塞 fd + io_service 唤醒)                        │
│  file (Linux: 内核 AIO io_setup/io_getevents; Windows: IOCP)    │
└─────────────────────────────────────────────────────────────────┘
```

**依赖方向**：上层只依赖下层；任务抽象不依赖任何调度器（`task` 纯粹是"可等待的计算"），调度器通过 awaitable 接口介入。

---

## 3. 核心机制：task 的一生

### 3.1 协程三件套

任何 C++20 协程都由三个角色构成，`task.hpp` 是标准范本：

| 角色 | 在 cppcoro 中 | 职责 |
|---|---|---|
| 返回对象 | `task<T>` | 协程函数的返回类型；协程帧句柄的 RAII 包装（move-only，析构销毁帧） |
| promise | `task_promise<T>` | 协程状态机控制器：存结果/异常，决定启动与结束行为 |
| awaiter | `awaitable_base`（`operator co_await()` 返回） | 决定父协程如何挂起、子协程如何启动、结果如何取出 |

### 3.2 生命周期时间线

```
调用 task 函数                     编译器自动行为                    cppcoro 代码
──────────────────────────────────────────────────────────────────────────────
auto t = step1();     →  堆上创建协程帧 (参数捕获进帧)
                         → 构造 promise (task_promise)
                         → get_return_object()          → 返回 task<T>{handle}
                         → co_await initial_suspend()   → suspend_always: 停!
                         ← 调用返回, 函数体一行未执行        (这就是 "lazy")

co_await t (父协程中)  →  t.operator co_await() 产生 awaiter
                         → await_ready(): 已完成?        → 否 → 挂起父协程
                         → await_suspend(父handle)       → set_continuation(父)
                                                         → return 子handle (对称转移)
                         → 编译器尾调用 resume 子协程     → 子协程体开跑

子协程体执行...         → 遇 co_await xxx: 同样的三问 (ready/suspend/resume)
                         → co_return v                  → promise.return_value(v)
                                                        (值 placement-new 进 union)

子协程结束              → co_await final_suspend()       → final_awaitable
                         → await_suspend() 返回父handle  → 尾调用唤醒父协程
                         → 父的 await_resume()          → promise.result() 取值
                                                         (有异常则 rethrow)

task 析构 (t 离开作用域) → handle.destroy()              → 析构 promise + 释放帧
```

### 3.3 lazy vs eager

| | cppcoro `task`（lazy） | deploy `TaskT`（eager） |
|---|---|---|
| `initial_suspend` | `suspend_always`：造出来先停 | `suspend_never`（或立即跑）：造出来就执行到第一个挂起点 |
| 调用语义 | 调用 = 仅构造描述，不花钱 | 调用 = 立即干活 |
| 典型问题 | 忘了 `co_await` 就白造（故标 `[[nodiscard]]`） | 构造即产生副作用，需要背压控制 |
| 组合友好度 | 高：可先建一组 task 再 `when_all` 统一启动 | 需显式 `coroRun` 接管驱动 |

### 3.4 对称转移（symmetric transfer）——最重要的机制

**问题**：子协程结束时直接 `continuation.resume()` 是普通函数调用——
循环 `co_await` 一万个同步完成的 task，或深度嵌套，调用栈线性增长 → 爆栈。

**解法**：`await_suspend()` 返回 `coroutine_handle`，编译器保证**尾调用**：

```cpp
// final_awaitable (子协程临终)
coroutine_handle<> await_suspend(coroutine_handle<PROMISE> coro) {
    return coro.promise().m_continuation;   // 返回父协程 → 编译器 jmp 过去
}

// awaitable_base (父协程启动子协程)
coroutine_handle<> await_suspend(coroutine_handle<> awaiting) {
    m_coroutine.promise().set_continuation(awaiting);
    return m_coroutine;                     // 返回子协程 → 编译器 jmp 过去
}
```

效果：唤醒链变成 `jmp` 链，栈深度恒定为 O(1)。

**GCC 10 不支持**（需 GCC 11+），cppcoro 走 fallback：

```cpp
// fallback: 先启动子协程, 再用 atomic 闸门竞争 resume 权
bool await_suspend(coroutine_handle<> awaiting) {
    m_coroutine.resume();                                    // ① 先跑子协程
    return m_coroutine.promise().try_set_continuation(awaiting); // ② 抢注册
}
// try_set_continuation: m_state.exchange(true) —— 谁先把 false→true 谁负责唤醒
```

这个 `atomic<bool> m_state` 仲裁的竞态（"注册 continuation" vs "协程已跑完"），与 deploy 项目 `coroutine_handoff_test` 场景 2 要验证的问题是同一个。

### 3.5 结果存储：union + 三态判别器

```cpp
enum class result_type { empty, value, exception };
union { T m_value; std::exception_ptr m_exception; };
```

- `co_return v` → placement new 进 `m_value`，标记 `value`
- 抛异常 → `unhandled_exception()` 存 `current_exception()`，标记 `exception`
- `result()` → 按标记取值或 `rethrow_exception`

三个特化：`task_promise<T>`（存值）、`task_promise<void>`（只传异常）、`task_promise<T&>`（存指针，零拷贝返回引用）。

---

## 4. 模块详解

### 4.1 任务抽象（纯头文件）

| 头文件 | 机制 | 要点 |
|---|---|---|
| `task.hpp` | lazy 单次消费任务 | 见第 3 节全文详解 |
| `shared_task.hpp` | 可多方等待的任务 | 引用计数；`await_suspend` 用 atomic 计数，最后一个等待者/完成方负责唤醒全体（`when_all_ready` 的基础） |
| `generator.hpp` | 同步生成器 | `co_yield` 产出序列，拉取式迭代 |
| `async_generator.hpp` | 异步生成器 | 生产者/消费者双向协程交接，双缓冲支持并发推进 |
| `awaitable_traits.hpp` | SFINAE 探测 | 推导 `co_await T` 的结果类型（`when_all`/`make_task` 的基建） |

### 4.2 调度层（核心）

cppcoro 没有"全局调度器"——**调度器 = 提供 `schedule()` awaitable 的对象**，协程 `co_await` 它即"被调度"。

| 组件 | 实现 | 对照 deploy |
|---|---|---|
| `io_service` | **本版仅有 Windows IOCP 实现，Linux 下是空壳（上游 WIP，无 epoll）**。接口语义：`schedule()` / `schedule_after(dur)` 返回 awaitable，`co_await` 即"被调度" | `MkEventLoop` + `TimerAwaiter`（deploy 这部分比 cppcoro 完整） |
| `static_thread_pool` | 固定 N 线程，每线程一个任务队列，`schedule()` 入队随机线程 | output worker 线程池（round-robin 投递 FlushRequest） |
| `inline_scheduler` | `co_await` 时不挂起，同线程立即继续 | 无对应（你的管道总是经循环） |
| `round_robin_scheduler` | 在 N 个内部调度器间轮转 | worker 投递的 round-robin 策略 |
| `schedule_on(sched, awaitable)` | 把 awaitable 的执行搬到 sched 上，完成后回原调度器 | worker 执行完经 result pipe 回引擎线程 |
| `resume_on(sched, awaitable)` | 只改"恢复方"所在线程 | 结果统一回引擎线程处理 |

**io_service 内部时序**（Windows IOCP 路径；Linux 无实现，仅看设计）：

```
协程线程                     io_service 循环/定时器线程
co_await schedule_after(150ms)
  → 构造 timed_schedule_operation
  → await_suspend: 把 {到期时刻, 协程handle} 插入 timer_queue(堆), 唤醒定时器线程
  → 挂起 ──────────────────→  timer_thread_state::run(): 等待到点
                               出堆 → schedule_impl → IOCP 投递
                               循环线程 GetQueuedCompletionStatus 收到
                               handle.resume() ──→ 协程在循环线程恢复!
```

与 deploy `TimerAwaiter` 完全同构：挂起不是阻塞，是把"唤醒源"注册进事件循环。
Linux 上 cppcoro 这一层缺失，而 deploy 的 `MkEventLoop`（mk_event 抽象）已自行实现。

### 4.3 同步原语

| 原语 | 语义 | 实现要点 |
|---|---|---|
| `async_mutex` | 协程互斥锁，lock 不上就挂起排队 | 等待者链表挂在 mutex 上，unlock 时 resume 队首 |
| `async_manual_reset_event` | 手动复位信号，多等待者 | 置位后所有等待者被唤醒 |
| `async_auto_reset_event` | 自动复位，一次唤醒一个 | 唤醒一个等待者后自动复位 |
| `async_latch` | 倒计数门闩 | count_down 到 0 唤醒全体 |
| `sequence_barrier` / `*_sequencer` | 无锁环形队列的序号协调 | SPSC/MPSC 背压，LMAX Disruptor 风格 |

### 4.4 取消机制

```
cancellation_source  ──发出──→  cancellation_token  ──注册回调──→  cancellation_registration
     (发起方持有)                (传给协程链路)                    (取消时执行回调)
```

token 内部是共享的 `cancellation_state`（引用计数 + 原子标志）。协程在 awaitable 内部检查 token 或注册回调，被取消时抛 `operation_cancelled`。对应 fluent-bit 没有直接等价物（flb 靠 engine shutdown 全局退出）。

### 4.5 组合子

| 组合子 | 行为 |
|---|---|
| `when_all(tasks...)` | 全部完成才继续；任一抛异常 → 取消其余，重抛首个异常 |
| `when_all_ready(tasks...)` | 全部完成才继续；不传播异常，逐个 `result()` 自查（容错批量） |
| `fmap(fn, awaitable)` | 对结果做变换：`co_return fn(co_await awaitable)` |
| `sync_wait(awaitable)` | 同步外壳：当前线程跑一个 io_service 直到协程完成，桥接非协程世界 |

---

## 5. 与 deploy 项目的逐项对照

| cppcoro | deploy 项目 | 差异/备注 |
|---|---|---|
| `task<T>`（lazy） | `TaskT<T>`（eager） | 启动策略相反；cppcoro 组合更灵活，deploy 采集即干 |
| `promise::m_continuation` | `TaskT` 的 continuation | 同款；deploy 已实现 `final_suspend` 对称转移 |
| `awaitable_base` fallback 的 `atomic m_state` | `coroutine_handoff_test` 场景 2 | 同一个竞态：注册 continuation vs 协程已完成 |
| `io_service`（epoll + 定时器堆） | `MkEventLoop`（mk_event 抽象） | cppcoro 封装在库内；deploy 对齐 fluent-bit mk_event |
| `schedule_after` | `TimerAwaiter` | 同构 |
| socket awaitable（非阻塞 fd + io_service 唤醒） | `FdAwaiter` + `HttpClient` | 同构；deploy 多了 HTTP 协议定界 |
| `static_thread_pool` + `schedule_on` | output worker 池 + pipe round-robin + result pipe 回引擎 | deploy 用 pipe 跨线程，cppcoro 用队列 + 原子；`schedule_on`/`resume_on` 即"执行线程/恢复线程"分离的标准化写法 |
| `when_all_ready` | （未实现） | ES 批量请求"等齐再逐个查成败"可直接借鉴 |
| `cancellation_token` | （未实现，靠 engine exit） | 若做请求级超时取消可引入 |
| `sync_wait` | `coroRun(loop, task)` | 同款"同步驱动壳"，coroRun 额外持续驱动事件循环排空 |

**迁移建议**：deploy 若新增并发组合，直接抄 `when_all_ready` 的共享计数设计；`schedule_on`/`resume_on` 可作为 worker 池语义的文档化参照。

---

## 6. 代码导读路线（推荐阅读顺序）

1. **`include/cppcoro/task.hpp`**（已加教学注释）——协程三件套、lazy、对称转移、union 结果存储
2. **`include/cppcoro/awaitable_traits.hpp`** + **`detail/get_awaiter.hpp`**（已加注释）——awaitable 探测
3. **`include/cppcoro/sync_wait.hpp`** + **`detail/sync_wait_task.hpp`**——同步外壳如何用 io_service 驱动协程
4. **`include/cppcoro/when_all.hpp`** + **`detail/when_all_*`**——并发组合与共享计数
5. **`lib/io_service.cpp`**（Linux 段）——epoll 循环 + 定时器堆，对照你的 MkEventLoop
6. **`include/cppcoro/static_thread_pool.hpp`** + **`lib/static_thread_pool.cpp`**——线程池调度器
7. **`include/cppcoro/shared_task.hpp`**——引用计数多方等待

---

## 7. 构建说明（GCC 10 适配）

原版只有 cake 构建系统（Python，老旧）。本 fork 增加了：

| 新增文件 | 作用 |
|---|---|
| `CMakeLists.txt` | `libcppcoro` 静态库 + `demo` 示例；剔除 Linux 不可编译的平台源码 |
| `include/experimental/coroutine` | shim：`std::experimental::*` 协程 TS 名字 → 标准 `std::*`（GCC 10 无 TS 兼容头） |
| `include/experimental/filesystem` | shim：`std::experimental::filesystem` → `std::filesystem` |
| `demo.cpp` | 5 个教学场景示例（已实测通过） |

**Linux 剔除清单**（上游 WIP，仅 Windows 实现）：`io_service.cpp`、`file*.cpp` / `*_file.cpp`（8 个）、`socket*.cpp`（9 个）、`win32.cpp`。
保留的 task / when_all / 同步原语 / 线程池 / 取消机制全平台可用——协程机制的学习价值也集中在这里。

```bash
cd cppcoro
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
./build/demo
```

注意：GCC 10 无对称转移，`task.hpp` 自动走 atomic fallback 分支（demo 场景 4 即展示该路径下的跨线程唤醒）；想看对称转移路径需 GCC 11+/clang 7+。

---

## 8. 一图总结

```
           lazy 的世界 (cppcoro)              eager 的世界 (deploy/fluent-bit)
      ┌──────────────────────────┐      ┌──────────────────────────┐
 调用  │ 造帧 → 停在入口 → 返回    │  调用 │ 造帧 → 立即跑 → 挂起点    │
 await │ 注册continuation→跳进去   │ await │ coroRun 驱动事件循环      │
 结束  │ final_suspend 尾调用父    │ 结束  │ final_suspend 对称转移    │
 调度  │ awaitable 里注册到调度器   │ 调度  │ Awaiter 注册到 MkEventLoop│
      └──────────────────────────┘      └──────────────────────────┘
          共同骨架: promise 三回调 + awaiter 三问 + 事件循环唤醒
```

**结论**：协程框架的难点不在语法，而在三处——①结束时的控制权移交（对称转移/竞态仲裁）②"恢复发生在哪个线程"（调度器抽象）③批量组合时的完成度协调（共享计数）。cppcoro 把这三处都写成了最小可读实现，这正是它值得逐行读的原因。
