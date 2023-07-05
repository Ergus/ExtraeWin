#pragma once

#include <iostream>
#include <string.h>
#include <vector>
#include <fstream>
#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>
#include <filesystem>
#include <sstream>
#include <iomanip>

namespace profiler {

	namespace fs = std::filesystem;

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)

	#include <processthreadsapi.h>
	#include <winbase.h>
	#include <windows.h>

	inline int getNumberOfCores() {
		SYSTEM_INFO sysinfo;
		GetSystemInfo(&sysinfo);
		return sysinfo.dwNumberOfProcessors;
	}

	inline unsigned int getCPUId()
	{
		PROCESSOR_NUMBER procNumber;
		GetCurrentProcessorNumberEx(&procNumber);
		return procNumber.Group * 64 + procNumber.Number;
	}

#else

	#include <unistd.h>

	inline int getNumberOfCores() {
		return sysconf(_SC_NPROCESSORS_ONLN);
	}

	inline unsigned int getCPUId()
	{
		const int cpu = sched_getcpu();
		assert(cpu >= 0);
		return cpu;
	}

#endif

	template<typename clocktype>
	inline unsigned long getMicroseconds(const std::chrono::time_point<clocktype> &timePoint)
	{
		return std::chrono::time_point_cast<std::chrono::microseconds>(timePoint).time_since_epoch().count();
	}


	/**
	   Buffer class to store the events.

	   This class will always have a file associated to it to flush all the data when needed.  There
	   will be 1 Buffer/Core in order to make the tracing events registration completely lock free.
	*/
	template <size_t BUFFERSIZE>
	class Buffer {

		/**
		   Event struct that will be reported (and saved into the traces files in binary format)

		   I reserve all the negative values for internal events. And positive values for
		   user/application events.
		   Every starting event needs and end event. But I cannot ensure that they will be in the
		   same cpu (due to thread migration), but they will in the same thread. So, I also save the
		   threadId with every event in order to make a right reconstruction latter.
		 */
		struct EventEntry {
			const uint8_t _id;
			const uint8_t _value;
			const uint16_t _core;
			const uint32_t _tid;
			const uint32_t _time;

			explicit EventEntry(
				uint16_t core, uint32_t tid, int8_t id, uint8_t value, uint32_t time
			) : _id(id),
				  _value(value),
				  _core(core),
				  _tid(tid),
				  _time(time)
			{
			}

			explicit EventEntry(
				uint16_t core, uint32_t tid, uint8_t id, uint8_t value
			) : EventEntry(
					core,
					tid,
					id,
					value,
					getMicroseconds(std::chrono::high_resolution_clock::now()))
			{
			}

		};

		static constexpr size_t _maxEntries
			= ( BUFFERSIZE + sizeof(EventEntry) - 1 ) / sizeof(EventEntry);	 //< Maximum size for the buffers ~ 1Mb >/

		struct TraceHeader {
			uint32_t _cpuID;
			uint32_t _totalFlushed;

			TraceHeader(TraceHeader&& other)
				: _cpuID(other._cpuID),
				  _totalFlushed(other._totalFlushed)
			{
				other._cpuID = std::numeric_limits<unsigned int>::max();
				other._totalFlushed = 0;
			}

			explicit TraceHeader(unsigned int cpuID)
				: _cpuID(cpuID),
				  _totalFlushed(0)
			{}
		} _header;

		const std::string _fileName;  //< Name of the binary file with trace information
		std::ofstream _file;		  //< fstream with file open; every write will be appended and binary. >/
		std::vector<EventEntry> _entries; //< Buffer with entries; it will be flushed when the number of entries reach _maxEntries;

		void flushBuffer()
		{
			if (_entries.empty())
				return;

			_file.write(reinterpret_cast<char *>(_entries.data()), _entries.size() * sizeof(EventEntry));
			_header._totalFlushed += _entries.size();
			_file.clear();
		}

	  public:

		explicit Buffer(uint32_t cpuID, const std::string &fileName)
			: _header(cpuID),
			  _fileName(fileName),
			  _file(_fileName.c_str(), std::ios::out | std::ios::binary),
			  _entries()
		{
			_entries.reserve(_maxEntries);
			assert(_file.is_open());

			// Reserve size for the header
			_file.write(reinterpret_cast<char *>(&_header), sizeof(TraceHeader));

			// The -1 event is the global execution event.
			// We use it to measure the total time and rely on constructors/destructors
			// of static members (the singleton) to register them.
			emplace(cpuID, 0, 0, 1, getMicroseconds(std::chrono::system_clock::now()));
		}

		Buffer(Buffer&& other)
			: _header(std::move(other._header)),
			  _fileName(other._fileName),
			  _file(std::move(other._file)),
			  _entries(std::move(other._entries))
		{
		}

		/**
		   Destructor for the buffer type.

		   Declared outside in order to call singleton functions.
		*/
		~Buffer();

		/**
		   Add (report) a new event.

		   This adds a new entry in the buffer with a value and id. when id is
		   zero it means that it is the end of the event
		*/
		template <typename ...T>
		void emplace(T&&... args)
		{
			_entries.emplace_back(std::forward<T>(args)...);

			// Assert that the element inserted belongs to this core.
			assert(_entries.back()._core == _header._cpuID);

			if (_entries.size() >= _maxEntries)
				flushBuffer();

			assert(_entries.size() < _maxEntries);
		}
	};

	template<size_t BUFFERSIZE = (1 << 20)>	 //< Maximum size for the buffers ~ 1Mb >/
	class ProfilerGuard {

		class BufferSet {

		  public:
			using buffer_t = Buffer<BUFFERSIZE>;

			BufferSet() :
				_startTimePoint(std::chrono::system_clock::now()),
				_size(getNumberOfCores())
			{
				auto localTime = std::chrono::system_clock::to_time_t(_startTimePoint);

				std::stringstream ss;
				ss << "TRACEDIR_" << std::put_time(std::localtime(&localTime), "%Y-%m-%d_%H_%M_%S");
				_traceDirectory = ss.str();

				// Create the directory
				if (!std::filesystem::create_directory(_traceDirectory))
					throw  std::runtime_error("Cannot create traces directory: " + _traceDirectory);

				_file.open(_traceDirectory + "/Trace.txt", std::ios::out);
				assert(_file.is_open());

				for (size_t i = 0; i < _size; ++i)
				{
					const std::string fileNamei = _traceDirectory + "/Trace_" + std::to_string(i) + ".bin";
					_profileBuffers.emplace_back(i, fileNamei);
				}
			}

			void AddReport(const std::string& filename)
			{
				std::lock_guard<std::mutex> guard(_fileMutex);
				assert(_file.is_open());
				_file << filename << std::endl;
			}

			inline buffer_t &operator[](uint16_t coreIdx)
			{
				assert(coreIdx < _size);
				return _profileBuffers[coreIdx];
			}

		  private:
			std::chrono::time_point<std::chrono::system_clock> _startTimePoint;
			std::string _traceDirectory;
			const size_t _size;

			std::mutex _fileMutex;	 //< mutex needed to write in the global file >/
			std::ofstream _file;

			// This AFTER _file to destroy in right order (RAII)
			std::vector<buffer_t> _profileBuffers;

		};

		const uint8_t _id;
		const uint32_t _tid;

		static void registerNewEvent(uint8_t id, uint8_t value, uint32_t tid)
		{
			uint16_t cpu = getCPUId();
			_singleton[cpu].emplace(cpu, tid, id, value);
		}

	  public:

		static BufferSet _singleton;

		ProfilerGuard(uint8_t id, uint8_t value)
			: _id(id),
			  _tid(std::hash<std::thread::id>()(std::this_thread::get_id()))
		{
			assert(value != 0);
			registerNewEvent(_id, value, _tid);
		}

		~ProfilerGuard()
		{
			assert(std::hash<std::thread::id>()(std::this_thread::get_id()) == _tid);
			registerNewEvent(_id, 0, _tid);
		}

	};

	template <size_t T>
	typename ProfilerGuard<T>::BufferSet ProfilerGuard<T>::_singleton;


	// This is declared outside the class because it calls members of ProfilerGuard singleton.
	template <size_t BUFFERSIZE>
	Buffer<BUFFERSIZE>::~Buffer()
	{
		// Early exit without writing anything if this buffer was moved somewhere else.
		if (!_file.is_open())
		{
			assert(_header._totalFlushed == 0);
			assert(_entries.size() == 0);
			return;
		}

		// The -1 event is the global execution event.
		// We use it to measure the total time and rely on constructors/destructors
		// of static members (the singleton) to register them.
		emplace(_header._cpuID, 0, 0, 0, getMicroseconds(std::chrono::system_clock::now()));

		flushBuffer(); // Flush all remaining events
		_file.seekp(0);
		_file.write(reinterpret_cast<char *>(&_header), sizeof(TraceHeader));
		_file.close(); // close the file

		ProfilerGuard<BUFFERSIZE>::_singleton.AddReport(_fileName);
	}
}
