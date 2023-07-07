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

void threadFuncion(size_t id)
{
	static uint16_t _functionID = profiler::registerName(__func__);
	profiler::ProfilerGuard guard(_functionID, id + 1);

	for (size_t i = 0; i < 10; ++i) {
		static uint16_t _loopID = profiler::registerName("Loop", 11);
		profiler::ProfilerGuard guard2(_loopID, i + 1);

		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
}


int main()
{
	std::vector<std::thread> threadVector;

	for (size_t i = 0; i < 10; ++i) {
		threadVector.emplace_back(threadFuncion, i);
	}

	for(auto& t: threadVector)
		t.join();


	threadVector.clear();
	for (size_t i = 0; i < 10; ++i) {
		threadVector.emplace_back(threadFuncion, i);
	}

	for(auto& t: threadVector)
		t.join();


	return 0;
}
