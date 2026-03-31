/*
 * Copyright (C) 2023  Jimmy Aguilar Mena
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "Profiler.hpp"
#include <vector>
#include <numeric>
#include <algorithm>
#include <thread>
#include <execution>
#include <iostream>
#include <map>
#include <functional>
#include <string>

// ============================================================
// Auto-registration infrastructure
// ============================================================

inline std::map<std::string, std::function<void()>> & getTestRegistry() {
	static std::map<std::string, std::function<void()>> registry;
	return registry;
}

struct TestRegistrar {
	TestRegistrar(const std::string & name, std::function<void()> fn) {
		getTestRegistry().emplace(name, fn);
	}
};

#define DEFINE_TEST(name)                                   \
	static void name();                                     \
	static TestRegistrar registrar_##name(#name, name);    \
	static void name()

// ============================================================
// Tests
// ============================================================

// INSTRUMENT_FUNCTION with auto name
DEFINE_TEST(test_function_auto_name) {
	INSTRUMENT_FUNCTION();
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

// INSTRUMENT_FUNCTION with custom name
DEFINE_TEST(test_function_custom_name) {
	INSTRUMENT_FUNCTION("my_custom_function");
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

// INSTRUMENT_FUNCTION + INSTRUMENT_FUNCTION_UPDATE
DEFINE_TEST(test_function_update) {
	INSTRUMENT_FUNCTION();
	INSTRUMENT_FUNCTION_UPDATE(2, "phase_init");
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	INSTRUMENT_FUNCTION_UPDATE(3, "phase_work");
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	INSTRUMENT_FUNCTION_UPDATE(4, "phase_cleanup");
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// INSTRUMENT_FUNCTION + INSTRUMENT_SCOPE
DEFINE_TEST(test_scope) {
	INSTRUMENT_FUNCTION();
	for (size_t i = 0; i < 5; ++i) {
		INSTRUMENT_SCOPE(work_loop, 1 + i);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

// INSTRUMENT_FUNCTION + INSTRUMENT_SCOPE + INSTRUMENT_SCOPE_UPDATE
DEFINE_TEST(test_scope_update) {
	INSTRUMENT_FUNCTION();
	INSTRUMENT_SCOPE(work_scope, 1);
	INSTRUMENT_SCOPE_UPDATE(work_scope, 2, "prepare");
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	INSTRUMENT_SCOPE_UPDATE(work_scope, 3, "execute");
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	INSTRUMENT_SCOPE_UPDATE(work_scope, 4, "finalize");
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// INSTRUMENT_FUNCTION_UPDATE interleaved with INSTRUMENT_SCOPE
DEFINE_TEST(test_function_and_scope) {
	INSTRUMENT_FUNCTION();
	INSTRUMENT_FUNCTION_UPDATE(2, "setup");
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	for (size_t i = 0; i < 5; ++i) {
		INSTRUMENT_SCOPE(iteration, 1 + i);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	INSTRUMENT_FUNCTION_UPDATE(3, "teardown");
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// INSTRUMENT_FUNCTION_UPDATE + INSTRUMENT_SCOPE + INSTRUMENT_SCOPE_UPDATE all together
DEFINE_TEST(test_nested) {
	INSTRUMENT_FUNCTION("nested_test");
	INSTRUMENT_FUNCTION_UPDATE(2, "outer_begin");
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	for (size_t i = 0; i < 3; ++i) {
		INSTRUMENT_SCOPE(inner, 1 + i);
		INSTRUMENT_SCOPE_UPDATE(inner, 2, "inner_work");
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		INSTRUMENT_SCOPE_UPDATE(inner, 3, "inner_done");
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	INSTRUMENT_FUNCTION_UPDATE(3, "outer_end");
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// INSTRUMENT_FUNCTION across multiple threads
static void threaded_worker(size_t id) {
	INSTRUMENT_FUNCTION();
	INSTRUMENT_FUNCTION_UPDATE(2, "work");
	std::this_thread::sleep_for(std::chrono::milliseconds(10 + id));
}

DEFINE_TEST(test_multithreaded_function) {
	std::vector<std::thread> threads;
	for (size_t i = 0; i < 4; ++i)
		threads.emplace_back(threaded_worker, i);
	for (auto& t : threads)
		t.join();
}

// INSTRUMENT_SCOPE + INSTRUMENT_SCOPE_UPDATE across multiple threads
static void threaded_scope_worker(size_t id) {
	INSTRUMENT_FUNCTION();
	INSTRUMENT_SCOPE(task, 1 + id % 5);
	INSTRUMENT_SCOPE_UPDATE(task, 2, "step_a");
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	INSTRUMENT_SCOPE_UPDATE(task, 3, "step_b");
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

DEFINE_TEST(test_multithreaded_scope) {
	std::vector<std::thread> threads;
	for (size_t i = 0; i < 4; ++i)
		threads.emplace_back(threaded_scope_worker, i);
	for (auto& t : threads)
		t.join();
}

// INSTRUMENT_FUNCTION inside a parallel lambda
DEFINE_TEST(test_parallel_lambda) {
	std::vector<size_t> in(10);
	std::iota(in.begin(), in.end(), 0);
	std::vector<size_t> out(10);
	std::transform(std::execution::par,
	               in.cbegin(), in.cend(), out.begin(),
	               [](size_t val) -> size_t {
	                   INSTRUMENT_FUNCTION("parallel_lambda");
	                   std::this_thread::sleep_for(std::chrono::milliseconds(10));
	                   return val * val;
	               });
}

// ============================================================
// Entry point
// ============================================================

int main(int argc, char* argv[]) {
	const auto & tests = getTestRegistry();

	if (argc == 2 && std::string(argv[1]) == "--list") {
		for (const auto& [name, _] : tests)
			std::cout << name << "\n";
		return 0;
	}

	if (argc != 2) {
		std::cerr << "Usage: " << argv[0] << " <test_name>|--list\n";
		return 1;
	}

	const auto it = tests.find(argv[1]);
	if (it == tests.end()) {
		std::cerr << "Unknown test: " << argv[1] << "\n";
		return 1;
	}

	try {
		it->second();
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAILED: " << e.what() << "\n";
		return 1;
	}
}
