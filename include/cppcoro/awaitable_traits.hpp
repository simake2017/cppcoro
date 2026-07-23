///////////////////////////////////////////////////////////////////////////////
// Copyright (c) Lewis Baker
// Licenced under MIT license. See LICENSE.txt for details.
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
// 【教学导读】awaitable_traits —— 探测 "一个类型被 co_await 后得到什么"
// ---------------------------------------------------------------------------
// co_await expr 的实际流程:
//   1. 若 expr 的类型 (或其 operator co_await() 的返回值) 有 await_ready/
//      await_suspend/await_resume, 它就是 awaiter;
//   2. co_await 表达式的类型 = awaiter.await_resume() 的返回值类型。
//
// 这个 traits 用 SFINAE (std::void_t 惯用法) 把上述规则编译期化:
//   - 主模板: 空壳。T 不是 awaitable 时, 特化匹配失败 → 编译期拒绝
//   - 特化: 只有当 detail::get_awaiter(T) 合法时才存在, 暴露两个别名:
//       awaiter_t      —— 实际的 awaiter 类型
//       await_result_t —— co_await T 得到的值类型
//
// detail::get_awaiter 负责处理两种 awaitable 形态:
//   a. 类型自带 await_ready/suspend/resume (如你的 TimerAwaiter)
//   b. 类型有 operator co_await(), 返回真正的 awaiter (如 task<T> 本身)
//
// 用途示例: make_task() 靠 await_result_t 推导 task 的返回类型;
// when_all 靠它统一收集异构协程的结果类型。
// 对照 deploy 项目: 你如果要做 when_all 式并发组合, 第一个需要的就是它。
///////////////////////////////////////////////////////////////////////////////
#ifndef CPPCORO_AWAITABLE_TRAITS_HPP_INCLUDED
#define CPPCORO_AWAITABLE_TRAITS_HPP_INCLUDED

#include <cppcoro/detail/get_awaiter.hpp>

#include <type_traits>

namespace cppcoro
{
	// 主模板 (空): T 不可 co_await 时落到这里, 没有内部别名 → SFINAE 出局
	template<typename T, typename = void>
	struct awaitable_traits
	{};

	// 特化: decltype(get_awaiter(...)) 合法 (即 T 是 awaitable) 时启用。
	// std::void_t<合法表达式> 是经典的 "能算出这个表达式就选我" 探测器
	template<typename T>
	struct awaitable_traits<T, std::void_t<decltype(cppcoro::detail::get_awaiter(std::declval<T>()))>>
	{
		// get_awaiter 的产物: 真正带 await_ready/suspend/resume 的对象类型
		using awaiter_t = decltype(cppcoro::detail::get_awaiter(std::declval<T>()));

		// co_await T 最终拿到的值的类型
		using await_result_t = decltype(std::declval<awaiter_t>().await_resume());
	};
}

#endif
