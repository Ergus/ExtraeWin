#include <iostream>
#include <string.h>
#include <vector>
#include <fstream>
#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>

namespace profiler {

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)

	#include <processthreadsapi.h>
	#include <winbase.h>
	#include <windows.h>

	int getNumberOfCores() {
		SYSTEM_INFO sysinfo;
		GetSystemInfo(&sysinfo);
		return sysinfo.dwNumberOfProcessors;
	}

	unsigned int getCPUId()
	{
		PROCESSOR_NUMBER procNumber;
		GetCurrentProcessorNumberEx(&procNumber);
		return procNumber.Group * 64 + procNumber.Number;
	}

#else

	#include <unistd.h>

	int getNumberOfCores() {
		return sysconf(_SC_NPROCESSORS_ONLN);
	}

	unsigned int getCPUId()
	{
		const int cpu = sched_getcpu();
		assert(cpu >= 0);
		return cpu;
	}

#endif

	unsigned long getMicrosecondsTime()
	{
		return std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now()).time_since_epoch().count();
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
			const int _id;
			const unsigned int _value;
			const unsigned long _tid;
			const unsigned long _time;

			EventEntry(int id, unsigned int value, unsigned long tid)
				: _id(id),
				  _value(value),
				  _tid(tid),
				  _time(getMicrosecondsTime())
			{
			}
		};

		static constexpr size_t _maxEntries = ( BUFFERSIZE + sizeof(EventEntry) - 1 ) / sizeof(EventEntry);	 //< Maximum size for the buffers ~ 1Mb >/

		struct TraceHeader {
			unsigned int _cpuID;
			unsigned long _totalFlushed;

			TraceHeader(TraceHeader&& other)
				: _cpuID(other._cpuID),
				  _totalFlushed(other._totalFlushed)
			{
				other._cpuID = std::numeric_limits<unsigned int>::max();
				other._totalFlushed = 0;
			}

			TraceHeader()
				: _cpuID(getCPUId()),
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

		explicit Buffer(size_t cpuIdx)
			: _header(),
			  _fileName("Trace_" + std::to_string(cpuIdx) + ".bin"),
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
			emplace(-1, 1, 0);

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

		   This adds a new entry in the buffer with a value and id. when id is zero it means that it
		   is the end of the event
		*/
		void emplace(int id, unsigned int value, unsigned long tid)
		{
			_entries.emplace_back(id, value, tid);

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
				_size(getNumberOfCores()),
				_file("Trace.txt", std::ios::out)
			{
				assert(_file.is_open());
				for (size_t i = 0; i < _size; ++i)
					_profileBuffers.emplace_back(i);
			}

			void AddReport(const std::string& filename)
			{
				std::lock_guard<std::mutex> guard(_fileMutex);
				assert(_file.is_open());
				_file << filename << std::endl;
			}

			inline buffer_t &operator[](size_t idx)
			{
				assert(idx < _size);
				return _profileBuffers[idx];
			}

		  private:
			const size_t _size;

			std::mutex _fileMutex;	 //< mutex needed to write in the global file >/
			std::ofstream _file;

			// This AFTER _file to destroy in right order (RAII)
			std::vector<buffer_t> _profileBuffers;

		};

		const int _id;
		const size_t _tid;

		static void registerNewEvent(int id, unsigned int value, unsigned long tid)
		{
			unsigned int cpu = getCPUId();
			_singleton[cpu].emplace(id, value, tid);
		}

	  public:

		static BufferSet _singleton;

		ProfilerGuard(int id, unsigned int value)
			: _id(id),
			  _tid(std::hash<std::thread::id>()(std::this_thread::get_id()))
		{
			assert(value != 0);
			registerNewEvent(_id, value, _tid);
		}

		~ProfilerGuard()
		{
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
		emplace(-1, 0, 0);

		flushBuffer(); // Flush all remaining events
		_file.seekp(0);
		_file.write(reinterpret_cast<char *>(&_header), sizeof(TraceHeader));
		_file.close(); // close the file

		ProfilerGuard<BUFFERSIZE>::_singleton.AddReport(_fileName);
	}
}
