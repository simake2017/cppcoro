// ============================================================================
// demo.cpp —— cppcoro 教学示例 (编译: 见 CMakeLists.txt)
// ----------------------------------------------------------------------------
// 场景1: lazy 启动 —— 调用 task 函数不执行, co_await 才跑 (对照 deploy 的 eager TaskT)
// 场景2: 异常传播 —— 协程里 throw, 经 task 原样抛给 sync_wait
// 场景3: when_all —— 并发组合多个 task (对照 ES 批量请求)
// 场景4: 跨线程唤醒 —— 协程挂起等信号, 另一线程 set() 触发恢复 (对照 coroutine_handoff 场景2)
// 场景5: static_thread_pool —— schedule_on 跨线程恢复 (对照 output worker 线程池)
// ============================================================================
#include <cppcoro/task.hpp>
#include <cppcoro/sync_wait.hpp>
#include <cppcoro/when_all.hpp>
#include <cppcoro/async_manual_reset_event.hpp>
#include <cppcoro/static_thread_pool.hpp>
#include <cppcoro/schedule_on.hpp>

#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

// 打印当前线程 id, 用于观察"恢复发生在哪个线程"
static std::string tid()
{
	std::ostringstream oss;
	oss << "[tid " << std::this_thread::get_id() << "] ";
	return oss.str();
}

// ---------------------------------------------------------------------------
// 场景1: lazy 启动
// 注意: 调用 step1()/step2() 的瞬间, 函数体一行都不会执行 ——
// 编译器只创建协程帧并停在 initial_suspend (suspend_always) 处。
// 直到 sync_wait 把它 co_await, 才从入口开跑。
// (对照: deploy 的 TaskT 是 eager —— 一造出来就跑到第一个 co_await 挂起点)
// ---------------------------------------------------------------------------
cppcoro::task<int> step2(int x)
{
	std::cout << tid() << "step2 开始跑 (此刻才真正执行!)" << std::endl;
	co_return x * 2;
}

cppcoro::task<int> step1()
{
	std::cout << tid() << "step1 开始跑" << std::endl;
	int v = co_await step2(21);   // 此处才启动 step2, 并挂起等它完成
	std::cout << tid() << "step1 拿到 step2 结果: " << v << std::endl;
	co_return v + 1;
}

// ---------------------------------------------------------------------------
// 场景2: 异常传播
// unhandled_exception() 把异常存进 promise, result() 时 rethrow ——
// 异常跨越协程挂起点原样传递, 和普通函数调用栈一致。
// ---------------------------------------------------------------------------
cppcoro::task<int> failing()
{
	std::cout << tid() << "failing 即将抛出" << std::endl;
	throw std::runtime_error("协程内异常");
	co_return 0;   // 仅为让编译器识别为协程, 不可达
}

// ---------------------------------------------------------------------------
// 场景3: when_all 并发组合
// 两个 task 交替执行 (单线程协作式), 全部完成后一次性取回结果元组。
// (对照: deploy 对 ES 发多个 _bulk 请求等待全部返回)
// ---------------------------------------------------------------------------
cppcoro::task<int> job(const char* name, int v)
{
	std::cout << tid() << name << " 运行" << std::endl;
	co_return v;
}

// ---------------------------------------------------------------------------
// 场景4: 另一线程触发唤醒 (对照 coroutine_handoff 场景2: 信号来自其他线程)
// co_await event: 协程把自己挂进 event 的等待者队列后挂起;
// 另一线程 150ms 后 event.set() → 协程在 signaller 线程上恢复执行。
// (注: cppcoro 的 io_service 上游仅有 Windows IOCP 实现, Linux 下不可用,
//  故用 async_manual_reset_event 演示同样的"跨线程唤醒"机制)
// ---------------------------------------------------------------------------
cppcoro::task<> wakeup_demo(cppcoro::async_manual_reset_event& event)
{
	std::cout << tid() << "co_await event → 挂起, 等另一线程发信号" << std::endl;
	co_await event;
	std::cout << tid() << "← 被唤醒! 注意线程号: 恢复发生在发信号的线程上" << std::endl;
}

// ---------------------------------------------------------------------------
// 场景5: static_thread_pool + schedule_on (对照 output worker 线程池)
// schedule_on(pool, task): 被等待时先把 task 调度到线程池执行,
// 完成后再回到等待方所在线程 —— 即"恢复到哪个线程"由调度器决定。
// (对照: deploy 的 FlushRequest 经 pipe round-robin 投递给 worker 线程)
// ---------------------------------------------------------------------------
cppcoro::task<int> pool_work(int id)
{
	std::cout << tid() << "worker 执行任务 " << id << " (线程池线程)" << std::endl;
	co_return id * 10;
}

int main()
{
	std::cout << tid() << "===== main 线程 =====" << std::endl;

	// ---- 场景1 ----
	std::cout << "\n── 场景1: lazy 启动 ──" << std::endl;
	{
		auto t = step1();   // 只造协程帧, step1 打印语句此刻不会输出
		std::cout << tid() << "step1() 已返回, 但函数体还没跑 (lazy)" << std::endl;
		int v = cppcoro::sync_wait(std::move(t));   // 这里才开跑
		std::cout << tid() << "最终结果: " << v << " (期望 43)" << std::endl;
	}

	// ---- 场景2 ----
	std::cout << "\n── 场景2: 异常传播 ──" << std::endl;
	try
	{
		cppcoro::sync_wait(failing());
	}
	catch (const std::runtime_error& e)
	{
		std::cout << tid() << "main 捕获到协程抛出的异常: " << e.what() << std::endl;
	}

	// ---- 场景3 ----
	std::cout << "\n── 场景3: when_all 并发 ──" << std::endl;
	{
		auto a = job("A", 1);
		auto b = job("B", 2);
		auto [ra, rb] = cppcoro::sync_wait(cppcoro::when_all(std::move(a), std::move(b)));
		std::cout << tid() << "全部完成: A=" << ra << " B=" << rb << std::endl;
	}

	// ---- 场景4 ----
	std::cout << "\n── 场景4: 跨线程唤醒 ──" << std::endl;
	{
		cppcoro::async_manual_reset_event event;
		event.reset();   // 初始非信号态: co_await 它的协程会挂起
		// 发信号线程: 模拟"外部世界过一会儿产生事件" (网络回包/其他线程产出)
		std::thread signaller([&event]
		{
			std::this_thread::sleep_for(150ms);
			std::cout << tid() << "signaller: 150ms 到, event.set() 唤醒协程" << std::endl;
			event.set();   // 等待者在本线程被 resume
		});
		cppcoro::sync_wait(wakeup_demo(event));
		signaller.join();
	}

	// ---- 场景5 ----
	std::cout << "\n── 场景5: 线程池跨线程 ──" << std::endl;
	{
		cppcoro::static_thread_pool pool{2};
		int r = cppcoro::sync_wait(
			cppcoro::schedule_on(pool, pool_work(7)));
		std::cout << tid() << "main 拿到线程池结果: " << r << std::endl;
	}

	std::cout << "\n全部场景完成。" << std::endl;
	return 0;
}
