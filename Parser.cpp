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


#include <vector>
#include <cstdint>
#include <string>
#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <cassert>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <numeric>
#include <limits>
#include <iterator>
#include <sstream>
#include <memory>

class TraceFile {

public:
	struct TraceHeader {
		uint32_t _id;   // Can be cpuID or ThreadID
		uint32_t _nentries;
		uint64_t _tid;
		uint64_t _startGTime;    /**< Start global time >*/
	};

	//! EventEntry as written in the binary
	/** Needs to overlap with the one defined in Profiler */
	struct EventEntry {
		uint64_t _time;
		uint16_t _id;
		uint16_t _core;
		uint32_t _value;
	};

	//! EventEntry as stored internally in the Parses
	struct InternalEntry {

		uint64_t _time;
		uint16_t _id;
		uint16_t _core;
		uint32_t _value;
		uint16_t _thread;
		std::shared_ptr<InternalEntry> _send {};

		InternalEntry(const InternalEntry &) = default;
		InternalEntry(const EventEntry &event, uint16_t thread)
			: _time(event._time)
			, _id(event._id)
			, _core(event._core)
			, _value(event._value)
			, _thread(thread)
		{
		}

		void addMigration(const EventEntry &event, uint16_t thread)
		{
			assert(!_send);
			assert(thread == _thread);
			_send = std::make_shared<InternalEntry>(event, thread);
		}

		bool operator<(const InternalEntry &other) const
		{
			return _time < other._time;
		}

		friend std::ostream& operator<<(std::ostream& os, const InternalEntry& in)
		{
			static size_t commcounter = 0;
			// Format: https://tools.bsc.es/doc/1370.pdf
			// Communication
			// 3:object_send:lsend:psend:object_recv:lrecv:precv:size:tag
			//   object_send -> cpu:ptask:task:thread
			// 3:53:1:4:1:365298277:365387955:1:1:1:1:365413502:365424466:16:1
			if (in._send) {
				os << "3:";
				os << in._core << ":1:1:" << in._thread << ":" << in._time << ":" << in._time << ":";
				os << in._send->_core << ":1:1:" << in._send->_thread << ":" << in._send->_time << ":" << in._send->_time  << ":";
				os << 1 << ":" << ++commcounter;
			}

			// Event
			// 2:cpu:appl:task:thread:time:event:value
			// 2:1:1:1:1:358638325:50000002:8:50100001:0:50100002:0:50100004:1:70000001:1:80000001:1:70000002:1:80000002:1:70000003:1:80000003:1
			os << "2:" << in._core << ":1:1:" << in._thread << ":" << in._time << ":" << in._id << ":" << in._value;
			return os;
		}
	};


	std::vector<InternalEntry> _body;
	std::set<uint16_t> _coresList, _threadList;
	uint64_t _startGTime = std::numeric_limits<uint64_t>::max();

	operator const std::vector<InternalEntry>&() const
	{
		return _body;
	}

	TraceFile() = default;

	explicit TraceFile(const std::filesystem::path &filePath)
	{
		std::ifstream file(filePath.string(), std::ios::in | std::ios::binary);
		if (!file)
			throw std::runtime_error("Failed to open file: " + filePath.string());

		TraceHeader header;
		file.read(reinterpret_cast<char *>(&header), sizeof(TraceHeader));
		if (!file)
			throw std::runtime_error("Failed to read header in file: " + filePath.string());

		const size_t nentries = header._nentries;
		const size_t readSize = nentries * sizeof(EventEntry);
		std::cout << " -> " << nentries << " events (" << readSize << " bytes)" << std::endl;

		// We firt read the whole file into a vector and then run the vector once to copy to the
		// Internal storage.
		std::vector<EventEntry> tmp(nentries);

		file.read(reinterpret_cast<char *>(tmp.data()), readSize);
		if (!file)
			throw std::runtime_error("Failed to read events in file: " + filePath.string());

		assert(_body.size() == 0);
		_body.reserve(nentries);
		_threadList.emplace(header._id);

		for (const EventEntry &entry : tmp)
		{
			// Indicate to the last message that there was a migration between
			// these two steps.
			if (_body.size() > 0 && _body.back()._core != entry._core)
			{
				std::cout << "Fount migration" << std::endl;
				_body.back().addMigration(entry, header._id);
			}

			_body.emplace_back(entry, header._id);
			_coresList.emplace(entry._core);
		}

		_startGTime = header._startGTime;
	}

	//! The operator+ actually merges two TraceFiles
	/** The time order needs to be maintained and there is also another
		pass needed to create communications. */
	friend TraceFile operator+(const TraceFile &a, const TraceFile &b)
	{
		TraceFile ret;

		const uint32_t totalSize = a._body.size() + b._body.size();
		ret._body.reserve(totalSize);

		std::merge(a._body.begin(), a._body.end(),
		           b._body.begin(), b._body.end(),
		           std::back_inserter(ret._body));

		// Merge and sort cores
		std::set_union(a._coresList.begin(), a._coresList.end(),
		               b._coresList.begin(), b._coresList.end(),
		               std::inserter(ret._coresList, ret._coresList.begin()));

		// Merge and sort threads
		std::set_union(a._threadList.begin(), a._threadList.end(),
		               b._threadList.begin(), b._threadList.end(),
		               std::inserter(ret._threadList, ret._threadList.begin()));

		ret._startGTime = std::min(a._startGTime, b._startGTime);

		return ret;
	}

	std::string getHeaderLine() const
	{
		assert(_startGTime != 0);

		const uint64_t elapsed = _body.back()._time - _body.front()._time;

		const time_t localTime = static_cast<time_t>(_startGTime);

		std::stringstream ss;
		ss << "#Paraver " << std::put_time(std::localtime(&localTime), "(%d/%m/%Y at %H:%M):")
		   << elapsed << "_ns:1(" << _coresList.size() << "):1:1(" << _threadList.size() << ":1)";
		return ss.str();
	}

	friend std::ostream& operator<<(std::ostream& os, const TraceFile& in)
	{
		for (const InternalEntry &it: in._body)
			os << it << "\n";
		return os;
	}

};


class ParsedTraces {
	std::vector<TraceFile> _traceMap;

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
		TraceFile tmp = std::reduce(in._traceMap.begin(), in._traceMap.end());

		os << tmp.getHeaderLine() << std::endl;
		os << tmp << std::endl;

		return os;
	}
};


int main(int argc, char **argv)
{
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
