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

void threadFuncion1(size_t id)
{
	INSTRUMENT_FUNCTION();

	std::vector<double> v1(1);

	for (size_t i = 0; i < 10; ++i) {
		INSTRUMENT_SCOPE(10, 1 + i, "LOOP");
		v1.resize(v1.size() * 2);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	v1.clear();

	for (size_t i = 0; i < 10; ++i) {
		INSTRUMENT_SCOPE(11, 1 + i);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
}

void threadFuncion2(size_t id)
{
	INSTRUMENT_FUNCTION("threadFuncion2Specified");

	INSTRUMENT_FUNCTION_UPDATE(10, "LOOP1");
	for (size_t i = 0; i < 10; ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	for (size_t i = 0; i < 10; ++i) {
		INSTRUMENT_FUNCTION_UPDATE(11);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

int main()
{
	std::cout << "Enter Main" << std::endl;
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	std::vector<std::thread> threadVector;

	std::vector<double> tmp(100);

	for (size_t i = 0; i < 10; ++i) {
		threadVector.emplace_back(threadFuncion1, i);
	}

	for(auto& t: threadVector)
		t.join();

	tmp.resize(10); // Used to get memory trace information

	std::cout << "Sleep Main" << std::endl;
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	std::cout << "Wake Up" << std::endl;

	threadVector.clear();
	for (size_t i = 0; i < 10; ++i) {
		threadVector.emplace_back(threadFuncion2, i);
	}

	for(auto& t: threadVector)
		t.join();

	// Try to use the other way to parallelize
	std::vector<size_t> in(20);
	std::iota(in.begin(), in.end(), 0);

	std::vector<size_t> out(20);
	std::transform(std::execution::par,
	               in.cbegin(), in.cend(), out.begin(),
	               [](size_t in) -> size_t
	               {
					   INSTRUMENT_FUNCTION("Parallel_Lambda");
					   std::this_thread::sleep_for(std::chrono::milliseconds(100));
					   return in * in;
				   }
	);

	std::cout << "Exit Main" << std::endl;
	return 0;
}
