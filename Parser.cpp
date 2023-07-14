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

	TraceHeader _header;
	std::vector<EventEntry> _body;

	operator const std::vector<EventEntry>&() const
	{
		return _body;
	}


	TraceFile(const TraceHeader &header, std::vector<EventEntry> body)
		: _header(header), _body(std::move(body))
	{
	}

	explicit TraceFile(const std::filesystem::path &filePath)
	{
		std::ifstream file(filePath.string(), std::ios::in | std::ios::binary);

		if (!file)
			throw std::runtime_error("Failed to open file: " + filePath.string());

		file.read(reinterpret_cast<char *>(&_header), sizeof(TraceHeader));

		const size_t size = _header._size;
		_body.resize(size);

		file.read(reinterpret_cast<char *>(_body.data()), size* sizeof(EventEntry));
	}


	static TraceFile mergeTwo(const TraceFile  &a, const TraceFile &b)
	{
		std::vector<EventEntry> res;
		const uint32_t totalSize = a._body.size() + b._body.size();

		res.reserve(totalSize);

		std::merge(a._body.begin(), a._body.end(),
		           b._body.begin(), b._body.end(),
		           std::back_inserter(res));

		TraceHeader _header = {0, totalSize, 0, a._header._startGTime};

		return TraceFile(_header, res);
	}

	friend std::ostream& operator<<(std::ostream& os, const TraceFile& in)
	{
		for (const EventEntry &it: in._body)
			os << it << "\n";
		return os;
	}

};


class ParsedTraces {
	std::map<uint16_t, TraceFile> _traceMap;

	uint64_t _startGlobalTime = 0;
	TraceFile::EventEntry _startEvent, _lastEvent;

	std::set<uint16_t> _coresList, _threadList;

	void addTraceFile(const std::string &fileName)
	{
		TraceFile tFile(fileName);

		const uint16_t key = tFile._header._id;

		auto emplaced = _traceMap.emplace(key, std::move(tFile));

		if (!emplaced.second)
			throw std::runtime_error("Error repeated input id: " + std::to_string(key));

		TraceFile &entry = emplaced.first->second;
		if (entry._header._id == 1)
		{
			_startEvent = entry._body.front();
			_lastEvent = entry._body.back();
			_startGlobalTime = entry._header._startGTime;
		}

		_threadList.emplace(key);

		for (const auto &it : entry._body)
			_coresList.emplace(it._core);

	}

	std::string getHeaderLine() const
	{
		assert(_startGlobalTime != 0);

		const uint64_t elapsed = _lastEvent._time - _startEvent._time;

		const time_t localTime = static_cast<time_t>(_startGlobalTime);

		std::stringstream ss;
		ss << "#Paraver " << std::put_time(std::localtime(&localTime), "(%d/%m/%Y at %H:%M):")
		   << elapsed << "_ns:1(" << _coresList.size() << "):1:1(" << _threadList.size() << ":1)";
		return ss.str();
	}

	TraceFile merge() const
	{
		std::vector<TraceFile> traces, merged;

		// Copy the values
		for (const auto &it : _traceMap)
			traces.push_back(it.second);

		while(traces.size() > 1) {
			for (size_t i = 0; i < traces.size(); i += 2) {
				merged.push_back(
					traces.size() - i > 1
					? TraceFile::mergeTwo(traces[i], traces[i+1])
					: traces[i]
				);
			}
			traces = std::move(merged);
		}

		return traces[0];
	}

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
			this->addTraceFile(file.path());
		}
	}

	friend std::ostream& operator<<(std::ostream& os, const ParsedTraces& in)
	{
		os << in.getHeaderLine() << std::endl;

		TraceFile tmp = in.merge();
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

	return 0;
}
