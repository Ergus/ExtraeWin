/*
 * Copyright (C) 2024  Jimmy Aguilar Mena
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

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>
#include <iostream>
#include <fstream>
#include <set>
#include <cassert>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <limits>
#include <queue>
#include <vector>

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)

#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#define MAP_FAILED nullptr

namespace {
	inline void* mapFile(const std::filesystem::path &path, size_t &mappedSize)
	{
		HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
		                           nullptr, OPEN_EXISTING,
		                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (hFile == INVALID_HANDLE_VALUE)
			throw std::runtime_error("Failed to open file: " + path.string());
		LARGE_INTEGER fileSize;
		if (!GetFileSizeEx(hFile, &fileSize)) {
			CloseHandle(hFile);
			throw std::runtime_error("Failed to stat file: " + path.string());
		}
		mappedSize = static_cast<size_t>(fileSize.QuadPart);
		HANDLE hMapping = CreateFileMappingW(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
		CloseHandle(hFile);
		if (!hMapping)
			throw std::runtime_error("Failed to create file mapping: " + path.string());
		void *mapped = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
		CloseHandle(hMapping);
		if (!mapped)
			throw std::runtime_error("Failed to map file: " + path.string());
		return mapped;
	}

	inline void unmapFile(void *mapped, size_t)
	{
		UnmapViewOfFile(mapped);
	}
}

#else // defined(WIN32)

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace {
	inline void* mapFile(const std::filesystem::path &path, size_t &mappedSize)
	{
		const int fd = ::open(path.c_str(), O_RDONLY);
		if (fd < 0)
			throw std::runtime_error("Failed to open file: " + path.string());
		struct stat st;
		if (::fstat(fd, &st) < 0) {
			::close(fd);
			throw std::runtime_error("Failed to stat file: " + path.string());
		}
		mappedSize = static_cast<size_t>(st.st_size);
		void *mapped = ::mmap(nullptr, mappedSize, PROT_READ, MAP_PRIVATE, fd, 0);
		::close(fd);
		if (mapped == MAP_FAILED)
			throw std::runtime_error("Failed to mmap file: " + path.string());
		::madvise(mapped, mappedSize, MADV_SEQUENTIAL);
		return mapped;
	}

	inline void unmapFile(void *mapped, size_t size)
	{
		::munmap(mapped, size);
	}
}

#endif // defined(WIN32)

/** Memory-mapped view of a single per-thread binary trace file.

    The file is mapped read-only into virtual address space; no data is copied
    into heap memory.  A sequential read-ahead hint is applied at open time
    (madvise MADV_SEQUENTIAL on Linux, FILE_FLAG_SEQUENTIAL_SCAN on Windows)
    so the merge never stalls on I/O. */
class TraceFile {
	void *_mapped = MAP_FAILED;
	size_t _mappedSize = 0;

public:
	//! Header struct matching the layout written by Profiler.hpp
	struct TraceHeader {
		uint32_t _id;
		uint32_t _nentries;
		uint64_t _tid;
		uint64_t _startGTime;
	};

	//! Event record matching the layout written by Profiler.hpp
	struct EventEntry {
		uint64_t _time;
		uint16_t _id;
		uint16_t _core;
		uint32_t _value;
	};

	const TraceHeader *_header = nullptr;
	const EventEntry  *_events = nullptr;
	std::set<uint16_t> _coresList;

	TraceFile() = delete;
	TraceFile(const TraceFile &) = delete;
	TraceFile &operator=(const TraceFile &) = delete;

	TraceFile(TraceFile &&other) noexcept
		: _mapped(other._mapped)
		, _mappedSize(other._mappedSize)
		, _header(other._header)
		, _events(other._events)
		, _coresList(std::move(other._coresList))
	{
		other._mapped = MAP_FAILED;
		other._mappedSize = 0;
		other._header = nullptr;
		other._events = nullptr;
	}

	explicit TraceFile(const std::filesystem::path &path)
	{
		_mapped = mapFile(path, _mappedSize);

		const char *base = static_cast<const char *>(_mapped);
		_header = reinterpret_cast<const TraceHeader *>(base);
		_events = reinterpret_cast<const EventEntry *>(base + sizeof(TraceHeader));

		const size_t expected = sizeof(TraceHeader) + _header->_nentries * sizeof(EventEntry);
		if (_mappedSize < expected)
			throw std::runtime_error("File too small (truncated?): " + path.string());

		std::cout << " -> " << _header->_nentries << " events ("
		          << (_header->_nentries * sizeof(EventEntry)) << " bytes)" << std::endl;

		// Sequential scan: validate monotonicity and collect core IDs
		uint64_t lastTime = 0;
		for (size_t i = 0; i < _header->_nentries; ++i) {
			if (i > 0 && _events[i]._time < lastTime)
				throw std::runtime_error(
					"Non-monotonic timestamp at event " + std::to_string(i)
					+ " in file: " + path.string());
			lastTime = _events[i]._time;
			_coresList.emplace(_events[i]._core);
		}
	}

	~TraceFile() noexcept
	{
		if (_mapped != MAP_FAILED)
			unmapFile(_mapped, _mappedSize);
	}

	const EventEntry *begin() const { return _events; }
	const EventEntry *end()   const { return _events + _header->_nentries; }
};


class ParsedTraces {
	std::vector<TraceFile> _traceMap;

	/** One slot in the k-way merge heap.
	    Carries a pointer into the mmap'd event array of one file plus the
	    thread ID from that file's header.  Ordered by timestamp so that
	    std::priority_queue (max-heap) + std::greater gives a min-heap. */
	struct HeapEntry {
		const TraceFile::EventEntry *_current;
		const TraceFile::EventEntry *_end;
		uint32_t _thread;

		bool operator>(const HeapEntry &other) const {
			return _current->_time > other._current->_time;
		}
	};

public:
	explicit ParsedTraces(const std::filesystem::path &dirPath)
	{
		std::filesystem::directory_entry entry(dirPath);

		if (!std::filesystem::exists(dirPath))
			throw std::runtime_error("Path: " + dirPath.string() + " does not exist");

		if (!std::filesystem::is_directory(dirPath)) {
			throw std::runtime_error("Path: " + dirPath.string() + " is not a directory");
		}

		for (const auto &file : std::filesystem::directory_iterator(dirPath))
		{
			if (file.path().extension() != ".bin")
				continue;

			std::cout << "Processing trace file: " << file.path() << std::endl;
			_traceMap.emplace_back(file.path());
		}
	}

	friend std::ostream& operator<<(std::ostream& os, const ParsedTraces& in)
	{
		// Collect metadata from file headers and the core scan done at open time
		std::set<uint16_t> allCores;
		uint64_t startGTime = std::numeric_limits<uint64_t>::max();
		uint64_t firstTime  = std::numeric_limits<uint64_t>::max();
		uint64_t lastTime   = 0;

		for (const TraceFile &tf : in._traceMap) {
			allCores.insert(tf._coresList.begin(), tf._coresList.end());
			startGTime = std::min(startGTime, tf._header->_startGTime);
			if (tf._header->_nentries > 0) {
				firstTime = std::min(firstTime, tf.begin()->_time);
				lastTime  = std::max(lastTime,  (tf.end() - 1)->_time);
			}
		}

		// Write Paraver header line
		const time_t localTime = static_cast<time_t>(startGTime);
		os << "#Paraver "
		   << std::put_time(std::localtime(&localTime), "(%d/%m/%Y at %H:%M):")
		   << (lastTime - firstTime) << "_ns:1("
		   << *std::max_element(allCores.begin(), allCores.end()) << "):1:1("
		   << in._traceMap.size() << ":1)\n";

		// Seed min-heap with the first entry of each non-empty file
		std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<HeapEntry>> heap;

		for (const TraceFile &tf : in._traceMap) {
			if (tf._header->_nentries > 0)
				heap.push({tf.begin(), tf.end(), tf._header->_id});
		}

		// Stream events in time order directly from the mmap'd pages
		while (!heap.empty()) {
			HeapEntry top = heap.top();
			heap.pop();

			const TraceFile::EventEntry &e = *top._current;
			// Format: 2:cpu:appl:task:thread:time:event:value
			os << "2:" << e._core << ":1:1:" << top._thread << ":"
			   << e._time << ":" << e._id << ":" << e._value << "\n";

			++top._current;
			if (top._current != top._end)
				heap.push(top);
		}

		return os;
	}
};


int main(int argc, char **argv)
{
	// Replace the default terminate handler so that any unhandled exception
	// causes an immediate exit with a non-zero code instead of invoking the
	// platform crash reporter (WER on Windows), which would hang in CI.
	std::set_terminate([]{ _exit(1); });
	if (argc < 2)
		throw std::runtime_error(
			"Wrong argument. Usage: ./" + std::string(argv[0]) + " trace_directory"
		);

	std::filesystem::path dirPath(argv[1]);
	ParsedTraces traces(dirPath);

	std::filesystem::path outFilePath(dirPath / "Trace.prv");
	std::ofstream traceFile(outFilePath, std::ios::out | std::ios::binary);

	traceFile << traces << std::endl;

	std::cout << "Trace file created: " << outFilePath << std::endl;

	return 0;
}
