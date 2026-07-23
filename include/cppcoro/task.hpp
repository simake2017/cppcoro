///////////////////////////////////////////////////////////////////////////////
// Copyright (c) Lewis Baker
// Licenced under MIT license. See LICENSE.txt for details.
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
// 【教学导读】task.hpp —— cppcoro 的核心, 也是理解 C++20 协程的最佳样本
// ---------------------------------------------------------------------------
// 一个 task<T> 由三个角色组成, 对应协程机制的三件套:
//
//   1. task<T>            —— "返回对象": 协程函数返回它, 持有协程帧句柄,
//                            是 RAII 包装 (析构时销毁协程帧)。move-only。
//   2. promise_type       —— "状态机控制器": 编译器在协程帧里创建它,
//                            协程的每个关键节点都回调它的方法:
//                              get_return_object()  协程启动时, 造出 task
//                              initial_suspend()    要不要立刻执行函数体?
//                              await_transform()    (此处未用) 拦截 co_await
//                              return_value()       co_return 时存结果
//                              unhandled_exception() 抛异常时捕获
//                              final_suspend()      函数体结束时的收尾行为
//   3. awaitable          —— "被等待行为": operator co_await() 返回它,
//                            决定父协程如何挂起/把子协程启动/如何取结果。
//
// 关键设计:
//   * lazy 启动: initial_suspend() 返回 suspend_always,
//     调用 task 函数只是"造好协程帧, 停在入口", 直到被 co_await 才跑。
//     (对照: deploy 项目的 TaskT 是 eager 启动, 一造出来就跑到第一个挂起点)
//   * 结果存储: union { T m_value; exception_ptr m_exception } + 三态枚举,
//     co_return 的值和异常共用同一块内存, 取结果时 (result()) 按需 rethrow。
//   * 对称转移 (symmetric transfer): final_suspend() 的 await_suspend()
//     返回父协程句柄, 编译器保证以尾调用方式 resume 父协程 —— 深度嵌套
//     或循环 await 同步完成的 task 都不会爆栈。GCC 10 不支持, 走 fallback
//     分支 (atomic<bool> 仲裁 "注册 continuation" 与 "协程已完成" 的竞态)。
//   * 两份 operator co_await: const& 版 (左值 task, 结果按引用/const 取)
//     与 const&& 版 (右值 task, 结果 move 出来, 一次性消费)。
//
// 与 deploy 项目 TaskT 的逐项对照:
//   task_promise_base::m_continuation  ≈  TaskT 里存的父协程 continuation
//   final_awaitable (对称转移版)        ≈  TaskT::final_suspend 同款实现
//   fallback 里的 atomic m_state        ≈  你 coroutine_handoff 场景2 的竞态问题
//   broken_promise                      ≈  co_await 一个空/已析构 task 的保护
///////////////////////////////////////////////////////////////////////////////
#ifndef CPPCORO_TASK_HPP_INCLUDED
#define CPPCORO_TASK_HPP_INCLUDED

#include <cppcoro/config.hpp>
#include <cppcoro/awaitable_traits.hpp>
#include <cppcoro/broken_promise.hpp>

#include <cppcoro/detail/remove_rvalue_reference.hpp>

#include <atomic>
#include <exception>
#include <utility>
#include <type_traits>
#include <cstdint>
#include <cassert>

#include <experimental/coroutine>

namespace cppcoro
{
	template<typename T> class task;

	namespace detail
	{
		// =====================================================================
		// task_promise_base: 所有 task_promise<T> 特化的公共基类
		// 只管一件事: 协程结束时, 如何把控制权交还给等待者 (continuation)
		// =====================================================================
		class task_promise_base
		{
			friend struct final_awaitable;

			// -----------------------------------------------------------------
			// final_awaitable: 协程体跑完 (co_return 或 return_void 之后),
			// 编译器 co_await final_suspend() 的返回值, 即这个对象。
			// 它是"协程临终行为"的剧本。
			// -----------------------------------------------------------------
			struct final_awaitable
			{
				// 永远不 ready —— 必须挂起, 才有机会执行下面的移交逻辑
				bool await_ready() const noexcept { return false; }

#if CPPCORO_COMPILER_SUPPORTS_SYMMETRIC_TRANSFER
				// 【对称转移版】(clang>=7 / gcc>=11)
				// await_suspend 返回一个 coroutine_handle: 编译器不回到调用者,
				// 而是以尾调用直接跳去 resume 那个句柄 —— 即父协程。
				// 效果: 子协程结束 → 父协程恢复, 全程不增加调用栈深度。
				// 这就是为什么循环 co_await 一万个同步 task 不会爆栈。
				template<typename PROMISE>
				std::experimental::coroutine_handle<> await_suspend(
					std::experimental::coroutine_handle<PROMISE> coro) noexcept
				{
					return coro.promise().m_continuation;
				}
#else
				// 【fallback 版】(GCC 10 / 旧 MSVC, 无对称转移)
				// 问题: 子协程可能在父协程"注册 continuation"之前就同步跑完了
				// (因为父 await_suspend 先 resume 了子协程, 见 awaitable_base)。
				// 解法: 一个 atomic<bool> m_state 做"只许一方通行"的闸门:
				//   - 父注册 continuation: try_set_continuation 里 exchange(true)
				//   - 子协程结束: 这里 exchange(true)
				// 谁先把 false→true 谁负责 resume —— 恰好一次, 不重不漏。
				// HACK: Need to add CPPCORO_NOINLINE to await_suspend() method
				// to avoid MSVC 2017.8 from spilling some local variables in
				// await_suspend() onto the coroutine frame in some cases.
				// Without this, some tests in async_auto_reset_event_tests.cpp
				// were crashing under x86 optimised builds.
				template<typename PROMISE>
				CPPCORO_NOINLINE
				void await_suspend(std::experimental::coroutine_handle<PROMISE> coroutine)
				{
					task_promise_base& promise = coroutine.promise();

					// Use 'release' memory semantics in case we finish before the
					// awaiter can suspend so that the awaiting thread sees our
					// writes to the resulting value.
					// Use 'acquire' memory semantics in case the caller registered
					// the continuation before we finished. Ensure we see their write
					// to m_continuation.
					// (release: 保证结果值 m_value 的写入对恢复方可见;
					//  acquire: 保证看到对方写入的 m_continuation)
					if (promise.m_state.exchange(true, std::memory_order_acq_rel))
					{
						// 对方 (注册 continuation 的一方) 已经抢先 exchange 过,
						// 说明 continuation 已就位且对方不会 resume —— 轮到我
						promise.m_continuation.resume();
					}
				}
#endif

				void await_resume() noexcept {}
			};

		public:

			task_promise_base() noexcept
#if !CPPCORO_COMPILER_SUPPORTS_SYMMETRIC_TRANSFER
				: m_state(false)   // fallback 路径: 闸门初始关闭
#endif
			{}

			// lazy 的关键: 协程创建后先挂在入口, 不执行函数体,
			// 等父协程 co_await 它 (awaitable_base::await_suspend) 时才 resume
			auto initial_suspend() noexcept
			{
				return std::experimental::suspend_always{};
			}

			// 协程体结束时的收尾 —— 见上面 final_awaitable 的详解
			auto final_suspend() noexcept
			{
				return final_awaitable{};
			}

#if CPPCORO_COMPILER_SUPPORTS_SYMMETRIC_TRANSFER
			// 对称转移路径: 父协程在 await_suspend 里注册自己, 随后子协程被
			// 启动; 不存在竞态 (子协程一定在注册之后才跑), 无需 atomic
			void set_continuation(std::experimental::coroutine_handle<> continuation) noexcept
			{
				m_continuation = continuation;
			}
#else
			// fallback 路径: 注册 continuation 并争夺 resume 权。
			// 返回 true = 我 (注册方) 抢到了 → 父协程需要挂起等通知;
			// 返回 false = 子协程已跑完并抢先 exchange → 父协程不挂起, 直接往下走
			bool try_set_continuation(std::experimental::coroutine_handle<> continuation)
			{
				m_continuation = continuation;
				return !m_state.exchange(true, std::memory_order_acq_rel);
			}
#endif

		private:

			// 等待我的那个协程 (父协程) 的句柄, 我结束时 resume 它
			std::experimental::coroutine_handle<> m_continuation;

#if !CPPCORO_COMPILER_SUPPORTS_SYMMETRIC_TRANSFER
			// Initially false. Set to true when either a continuation is registered
			// or when the coroutine has run to completion. Whichever operation
			// successfully transitions from false->true got there first.
			// 初值 false。"注册 continuation" 或 "协程跑完" 两方谁先把它
			// 翻成 true, 谁就负责 (或有权) 执行 resume。
			std::atomic<bool> m_state;
#endif

		};

		// =====================================================================
		// task_promise<T>: task<T> 的 promise, 在基类之上增加"结果存储"
		// =====================================================================
		template<typename T>
		class task_promise final : public task_promise_base
		{
		public:

			task_promise() noexcept {}

			// 结果存在 union 里 (手动控制生命周期), 析构时按实际类型析构
			~task_promise()
			{
				switch (m_resultType)
				{
				case result_type::value:
					m_value.~T();
					break;
				case result_type::exception:
					m_exception.~exception_ptr();
					break;
				default:
					break;
				}
			}

			// 协程启动第一步: 编译器调它造出返回给调用者的 task 对象
			// (定义延后到文件底部, 因为那里 task<T> 才已完整定义)
			task<T> get_return_object() noexcept;

			// 协程体内抛出未捕获异常时编译器调这里: 存住, 等取结果时再抛
			void unhandled_exception() noexcept
			{
				::new (static_cast<void*>(std::addressof(m_exception))) std::exception_ptr(
					std::current_exception());
				m_resultType = result_type::exception;
			}

			// co_return value; —— placement new 把值构造进 union
			template<
				typename VALUE,
				typename = std::enable_if_t<std::is_convertible_v<VALUE&&, T>>>
			void return_value(VALUE&& value)
				noexcept(std::is_nothrow_constructible_v<T, VALUE&&>)
			{
				::new (static_cast<void*>(std::addressof(m_value))) T(std::forward<VALUE>(value));
				m_resultType = result_type::value;
			}

			// 取结果 (左值 task): 有异常就重抛, 否则返回值的引用
			T& result() &
			{
				if (m_resultType == result_type::exception)
				{
					std::rethrow_exception(m_exception);
				}

				assert(m_resultType == result_type::value);

				return m_value;
			}

			// HACK: Need to have co_await of task<int> return prvalue rather than
			// rvalue-reference to work around an issue with MSVC where returning
			// rvalue reference of a fundamental type from await_resume() will
			// cause the value to be copied to a temporary. This breaks the
			// sync_wait() implementation.
			// See https://github.com/lewissbaker/cppcoro/issues/40#issuecomment-326864107
			// 右值 task 取结果: 算术/指针类型按值返回 (避开 MSVC 坑),
			// 其余类型以 T&& move 出来 (一次性消费, 避免拷贝)
			using rvalue_type = std::conditional_t<
				std::is_arithmetic_v<T> || std::is_pointer_v<T>,
				T,
				T&&>;

			rvalue_type result() &&
			{
				if (m_resultType == result_type::exception)
				{
					std::rethrow_exception(m_exception);
				}

				assert(m_resultType == result_type::value);

				return std::move(m_value);
			}

		private:

			// 三态: 空 (还没结果) / 有值 / 有异常 —— union 的判别器
			enum class result_type { empty, value, exception };

			result_type m_resultType = result_type::empty;

			// 值与异常共享内存: 正常路径零额外开销
			union
			{
				T m_value;
				std::exception_ptr m_exception;
			};

		};

		// =====================================================================
		// task_promise<void>: 无返回值特化 —— co_return; 对应 return_void()
		// =====================================================================
		template<>
		class task_promise<void> : public task_promise_base
		{
		public:

			task_promise() noexcept = default;

			task<void> get_return_object() noexcept;

			void return_void() noexcept
			{}

			void unhandled_exception() noexcept
			{
				m_exception = std::current_exception();
			}

			// 无值可取, 只负责把异常传播出去
			void result()
			{
				if (m_exception)
				{
					std::rethrow_exception(m_exception);
				}
			}

		private:

			std::exception_ptr m_exception;

		};

		// =====================================================================
		// task_promise<T&>: 引用返回值特化 —— 不拷贝对象, 只存指针
		// =====================================================================
		template<typename T>
		class task_promise<T&> : public task_promise_base
		{
		public:

			task_promise() noexcept = default;

			task<T&> get_return_object() noexcept;

			void unhandled_exception() noexcept
			{
				m_exception = std::current_exception();
			}

			void return_value(T& value) noexcept
			{
				m_value = std::addressof(value);
			}

			T& result()
			{
				if (m_exception)
				{
					std::rethrow_exception(m_exception);
				}

				return *m_value;
			}

		private:

			T* m_value = nullptr;
			std::exception_ptr m_exception;

		};
	}

	/// \brief
	/// A task represents an operation that produces a result both lazily
	/// and asynchronously.
	///
	/// When you call a coroutine that returns a task, the coroutine
	/// simply captures any passed parameters and returns exeuction to the
	/// caller. Execution of the coroutine body does not start until the
	/// coroutine is first co_await'ed.
	// =====================================================================
	// task<T>: 协程返回对象 + 可等待对象 (本身可被 co_await)
	// 本质就是一个协程帧句柄的 RAII 包装:
	//   - 持有 coroutine_handle, 析构时 destroy() 回收协程帧
	//   - move-only (协程帧独享)
	//   - operator co_await() 让它可以被别的协程等待
	// =====================================================================
	template<typename T = void>
	class [[nodiscard]] task   // nodiscard: 造出来不 await 就丢弃是典型 bug
	{
	public:

		// 编译器靠这个别名找到协程的 promise 类型
		using promise_type = detail::task_promise<T>;

		using value_type = T;

	private:

		// -----------------------------------------------------------------
		// awaitable_base: co_await 一个 task 时的"挂起剧本"
		// 两个 operator co_await() 都继承它, 只差 await_resume 的取结果方式
		// -----------------------------------------------------------------
		struct awaitable_base
		{
			std::experimental::coroutine_handle<promise_type> m_coroutine;

			awaitable_base(std::experimental::coroutine_handle<promise_type> coroutine) noexcept
				: m_coroutine(coroutine)
			{}

			// 子协程已经完成 (或空 task) → 不用挂起, 直接取结果
			bool await_ready() const noexcept
			{
				return !m_coroutine || m_coroutine.done();
			}

#if CPPCORO_COMPILER_SUPPORTS_SYMMETRIC_TRANSFER
			// 【对称转移版】
			// 1. 把"我自己 (等待方)"登记为子协程的 continuation
			// 2. 返回子协程句柄 → 编译器尾调用 resume 它, 我原地挂起
			// 子协程结束时 final_awaitable 会返回我这个 continuation, 把我唤醒。
			// 全程零 atomic, 因为顺序是确定的: 先注册, 再启动子协程。
			std::experimental::coroutine_handle<> await_suspend(
				std::experimental::coroutine_handle<> awaitingCoroutine) noexcept
			{
				m_coroutine.promise().set_continuation(awaitingCoroutine);
				return m_coroutine;
			}
#else
			// 【fallback 版】(GCC 10 走这里)
			// 顺序反过来: 先 resume 子协程, 再尝试注册 continuation。
			// 为什么? 若子协程同步跑完, 它结束时 continuation 还没注册,
			// 没法唤醒我 —— 所以先让它跑, 再用 try_set_continuation 的
			// 返回值 (bool 版 await_suspend) 决定我是否真要挂起:
			//   true  = 注册成功, 我挂起等它唤醒 (它跑完时会 resume 我)
			//   false = 它已跑完并抢先, 我不挂起, 直接进 await_resume 取结果
			// NOTE: We are using the bool-returning version of await_suspend() here
			// to work around a potential stack-overflow issue if a coroutine
			// awaits many synchronously-completing tasks in a loop.
			//
			// We first start the task by calling resume() and then conditionally
			// attach the continuation if it has not already completed. This allows us
			// to immediately resume the awaiting coroutine without increasing
			// the stack depth, avoiding the stack-overflow problem. However, it has
			// the down-side of requiring a std::atomic to arbitrate the race between
			// the coroutine potentially completing on another thread concurrently
			// with registering the continuation on this thread.
			//
			// We can eliminate the use of the std::atomic once we have access to
			// coroutine_handle-returning await_suspend() on both MSVC and Clang
			// as this will provide ability to suspend the awaiting coroutine and
			// resume another coroutine with a guaranteed tail-call to resume().
			bool await_suspend(std::experimental::coroutine_handle<> awaitingCoroutine) noexcept
			{
				m_coroutine.resume();
				return m_coroutine.promise().try_set_continuation(awaitingCoroutine);
			}
#endif
		};

	public:

		task() noexcept
			: m_coroutine(nullptr)
		{}

		// 由 promise::get_return_object() 调用: 用协程帧句柄包装成 task
		explicit task(std::experimental::coroutine_handle<promise_type> coroutine)
			: m_coroutine(coroutine)
		{}

		task(task&& t) noexcept
			: m_coroutine(t.m_coroutine)
		{
			t.m_coroutine = nullptr;   // 移交所有权, 源置空防重复销毁
		}

		/// Disable copy construction/assignment.
		// 协程帧独享一份状态, 禁止拷贝
		task(const task&) = delete;
		task& operator=(const task&) = delete;

		/// Frees resources used by this task.
		// RAII: task 销毁 → 协程帧销毁 (含 promise 里存的结果/异常)
		~task()
		{
			if (m_coroutine)
			{
				m_coroutine.destroy();
			}
		}

		task& operator=(task&& other) noexcept
		{
			if (std::addressof(other) != this)
			{
				if (m_coroutine)
				{
					m_coroutine.destroy();
				}

				m_coroutine = other.m_coroutine;
				other.m_coroutine = nullptr;
			}

			return *this;
		}

		/// \brief
		/// Query if the task result is complete.
		///
		/// Awaiting a task that is ready is guaranteed not to block/suspend.
		bool is_ready() const noexcept
		{
			return !m_coroutine || m_coroutine.done();
		}

		// -----------------------------------------------------------------
		// operator co_await() const & —— 等待左值 task
		// await_resume 返回 T& (引用, 协程帧还在, 值还活着)
		// -----------------------------------------------------------------
		auto operator co_await() const & noexcept
		{
			struct awaitable : awaitable_base
			{
				using awaitable_base::awaitable_base;

				decltype(auto) await_resume()
				{
					if (!this->m_coroutine)
					{
						throw broken_promise{};   // 空 task: 没有协程可等
					}

					return this->m_coroutine.promise().result();
				}
			};

			return awaitable{ m_coroutine };
		}

		// -----------------------------------------------------------------
		// operator co_await() const && —— 等待右值 task (临时量)
		// await_resume 用 std::move(promise).result() 把值搬出来:
		// 临时 task 用完即毁, 值必须 move 走, 否则随帧一起消失
		// -----------------------------------------------------------------
		auto operator co_await() const && noexcept
		{
			struct awaitable : awaitable_base
			{
				using awaitable_base::awaitable_base;

				decltype(auto) await_resume()
				{
					if (!this->m_coroutine)
					{
						throw broken_promise{};
					}

					return std::move(this->m_coroutine.promise()).result();
				}
			};

			return awaitable{ m_coroutine };
		}

		/// \brief
		/// Returns an awaitable that will await completion of the task without
		/// attempting to retrieve the result.
		// 只等完成、不取结果的 awaitable —— 适用于 when_all_ready 这类
		// "先等齐, 再逐个检查成败" 的场景
		auto when_ready() const noexcept
		{
			struct awaitable : awaitable_base
			{
				using awaitable_base::awaitable_base;

				void await_resume() const noexcept {}
			};

			return awaitable{ m_coroutine };
		}

	private:

		std::experimental::coroutine_handle<promise_type> m_coroutine;

	};

	namespace detail
	{
		// promise::get_return_object() 的定义:
		// 协程帧创建后, 编译器调它, 用 from_promise 从句柄反查到
		// 刚创建的协程帧, 包装成 task 返回给调用者。(task<T> 至此才完整)
		template<typename T>
		task<T> task_promise<T>::get_return_object() noexcept
		{
			return task<T>{ std::experimental::coroutine_handle<task_promise>::from_promise(*this) };
		}

		inline task<void> task_promise<void>::get_return_object() noexcept
		{
			return task<void>{ std::experimental::coroutine_handle<task_promise>::from_promise(*this) };
		}

		template<typename T>
		task<T&> task_promise<T&>::get_return_object() noexcept
		{
			return task<T&>{ std::experimental::coroutine_handle<task_promise>::from_promise(*this) };
		}
	}

	// =========================================================================
	// make_task: 把任意 awaitable 适配成 task
	// 技巧: 函数体本身是个协程 (有 co_return/co_await), 编译器为它生成
	// 协程帧; 返回类型推导为 awaitable 的 await_result_t (去掉右值引用)。
	// 用途: 给不支持被多次等待/组合的 awaitable 套一层标准 task 外壳。
	// =========================================================================
	template<typename AWAITABLE>
	auto make_task(AWAITABLE awaitable)
		-> task<detail::remove_rvalue_reference_t<typename awaitable_traits<AWAITABLE>::await_result_t>>
	{
		co_return co_await static_cast<AWAITABLE&&>(awaitable);
	}
}

#endif
