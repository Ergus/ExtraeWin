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

class TraceFile {

public:
	struct TraceHeader {
		uint32_t _id;   // Can be cpuID or ThreadID
		uint32_t _size;
		uint64_t _tid;
		uint64_t _startGTime;    /**< Start global time >*/
	};

	struct EventEntry {
		uint64_t _time;
		uint16_t _id;
		uint16_t _value;
		uint16_t _core;
		uint16_t _thread;

		friend std::ostream& operator<<(std::ostream& os, const EventEntry& in)
		{
			os << "2:" << in._core << ":1:1:" << in._thread << ":" << in._time << ":" << in._id << ":" << in._value;
			return os;
		}

		bool operator<(const EventEntry &other) const
		{
			return _time < other._time;
		}
	};

	std::vector<EventEntry> _body;
	std::set<uint16_t> _coresList, _threadList;
	uint64_t _startGTime = std::numeric_limits<uint64_t>::max();

	operator const std::vector<EventEntry>&() const
	{
		return _body;
	}

	TraceFile() = default;

	explicit TraceFile(const std::filesystem::path &filePath)
	{
		TraceHeader header;

		std::ifstream file(filePath.string(), std::ios::in | std::ios::binary);

		if (!file)
			throw std::runtime_error("Failed to open file: " + filePath.string());

		file.read(reinterpret_cast<char *>(&header), sizeof(TraceHeader));

		const size_t size = header._size;
		_body.resize(size);

		file.read(reinterpret_cast<char *>(_body.data()), size* sizeof(EventEntry));

		for (const EventEntry &it: _body) {
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
		for (const EventEntry &it: in._body)
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

	std::filesystem::path outFilePath(dirPath / "Trace2.prv");
	std::ofstream traceFile(outFilePath, std::ios::out | std::ios::binary);

	traceFile << traces << std::endl;

	std::cout << "Trace file created: " << outFilePath << std::endl;

	return 0;
}
