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

		InternalEntry(const EventEntry &event, uint16_t thread)
			: _time(event._time)
			, _id(event._id)
			, _core(event._core)
			, _value(event._value)
			, _thread(thread)
		{
		}

		friend std::ostream& operator<<(std::ostream& os, const InternalEntry& in)
		{
			os << "2:" << in._core << ":1:1:" << in._thread << ":" << in._time << ":" << in._id << ":" << in._value;
			return os;
		}

		bool operator<(const InternalEntry &other) const
		{
			return _time < other._time;
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
		std::cout << " -> "  << nentries
		          << " events (" <<  nentries * sizeof(EventEntry) << " bytes)" << std::endl;

		std::vector<EventEntry> tmp(nentries);

		file.read(reinterpret_cast<char *>(tmp.data()), nentries * sizeof(EventEntry));
		if (!file)
			throw std::runtime_error(
				"Failed to read events in file: " + filePath.string()
			);

		assert(_body.size() == 0);
		_body.reserve(nentries);

		for (const EventEntry &entry : tmp)
		{
			_body.emplace_back(entry, header._id);

			const InternalEntry it = _body.back();
			_coresList.emplace(it._core);
			_threadList.emplace(it._thread);
		}

		_startGTime = header._startGTime;
	}

	friend TraceFile operator+(const TraceFile &a, const TraceFile &b)
	{
		TraceFile ret;

		const uint32_t totalSize = a._body.size() + b._body.size();
		ret._body.reserve(totalSize);

		std::merge(a._body.begin(), a._body.end(),
		           b._body.begin(), b._body.end(),
		           std::back_inserter(ret._body));

		std::set_union(a._coresList.begin(), a._coresList.end(),
		               b._coresList.begin(), b._coresList.end(),
		               std::inserter(ret._coresList, ret._coresList.begin()));

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
