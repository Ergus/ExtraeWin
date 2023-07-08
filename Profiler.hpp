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
#include <map>
#include <shared_mutex>

namespace profiler {

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)

	#include <processthreadsapi.h>
	#include <winbase.h>
	#include <windows.h>

	inline int getNumberOfCores() {
		SYSTEM_INFO sysinfo;
		GetSystemInfo(&sysinfo);
		return sysinfo.dwNumberOfProcessors;
	}

	/**
	   Return the cpuID starting by 1
	 */
	inline unsigned int getCPUId()
	{
		PROCESSOR_NUMBER procNumber;
		GetCurrentProcessorNumberEx(&procNumber);
		return procNumber.Group * 64 + procNumber.Number + 1;
	}

#else

	#include <unistd.h>

	inline int getNumberOfCores() {
		return sysconf(_SC_NPROCESSORS_ONLN);
	}

	/**
	   Return the cpuID starting by 1
	 */
	inline unsigned int getCPUId()
	{
		const int cpu = sched_getcpu();
		assert(cpu >= 0);
		return cpu + 1;
	}

#endif

	/**
	   Get microseconds since epoch for a given timePoint
	*/
	template<typename clocktype>
	inline unsigned long getMicroseconds(const std::chrono::time_point<clocktype> &timePoint)
	{
		return std::chrono::time_point_cast<std::chrono::microseconds>(timePoint).time_since_epoch().count();
	}

	// ==================================================
	// End of the basic functions
	// ==================================================

	/**
	   Buffer class to store the events.

	   This class will always have a file associated to it to flush all the data when needed.  There
	   will be 1 Buffer/Core in order to make the tracing events registration completely lock free.
	*/
	template <size_t BUFFERSIZE>
	class Buffer {

		struct TraceHeader {
			uint32_t _id;   // Can be cpuID or ThreadID
			uint32_t _totalFlushed;
			uint64_t _tid;
			uint64_t _startGTime;    /**< Start global time >*/

			TraceHeader(TraceHeader&& other)
				: _id(other._id),
				  _tid(other._tid),
				  _totalFlushed(other._totalFlushed),
				  _startGTime(other._startGlobalTime)
			{
				other._id = std::numeric_limits<unsigned int>::max();
				other._tid = std::numeric_limits<unsigned int>::max();
				other._totalFlushed = 0;
			}

			TraceHeader(uint32_t id, uint64_t tid, uint64_t startGTime)
				: _id(id), _tid(tid), _totalFlushed(0), _startGTime(startGTime)
			{}

		} _header;


		/**
		   Event struct that will be reported (and saved into the traces files in binary format)

		   I reserve all the negative values for internal events. And positive values for
		   user/application events.
		   Every starting event needs and end event. But I cannot ensure that they will be in the
		   same cpu (due to thread migration), but they will in the same thread. So, I also save the
		   threadId with every event in order to make a right reconstruction latter.
		 */
		struct EventEntry {
			const uint64_t _time;
			const uint16_t _id;
			const uint16_t _value;
			const uint16_t _core;
			const uint16_t _thread;

			explicit EventEntry(
				uint16_t id, uint16_t value, uint16_t thread
			) : _time(getMicroseconds(std::chrono::high_resolution_clock::now())),
				_id(id),
				_value(value),
				_core(getCPUId()),
				_thread(thread)
			{
			}
		};

		static constexpr size_t _maxEntries
			= ( BUFFERSIZE + sizeof(EventEntry) - 1 ) / sizeof(EventEntry);	 //< Maximum size for the buffers ~ 1Mb >/

		std::string _fileName;            //< Name of the binary file with trace information
		std::ofstream _file;		      //< fstream with file open; every write will be appended and binary. >/
		std::vector<EventEntry> _entries; //< Buffer with entries; it will be flushed when the number of entries reach _maxEntries;

		void flushBuffer()
		{
			if (_entries.empty())
				return;

			// We open the file the first time we need t flush the data.
			if (!_file.is_open())
			{
				_file.open(_fileName, std::ios::out | std::ios::binary);

				// Reserve space for the header
				_file.write(reinterpret_cast<char *>(&_header), sizeof(TraceHeader));
			}

			_header._totalFlushed += _entries.size();

			// Go to beginning and write the header
			_file.seekp(0, std::ios_base::beg);
			_file.write(reinterpret_cast<char *>(&_header), sizeof(TraceHeader));

			// Go to end and write the data
			_file.seekp(0, std::ios_base::end);
			_file.write(reinterpret_cast<char *>(_entries.data()), _entries.size() * sizeof(EventEntry));

			// clear the buffer.
			_entries.clear();
		}

	  public:

		Buffer(uint16_t id, uint64_t tid, std::string fileName, uint64_t startGTime);

		Buffer(Buffer&& other)
			: _header(std::move(other._header)),
			  _fileName(std::move(other._fileName)),
			  _file(std::move(other._file)),
			  _entries(std::move(other._entries))
		{
		}


		/**
		   Destructor for the buffer type.
		*/
		~Buffer();

		void emplace(uint16_t id, uint16_t value)
		{
			_entries.emplace_back(
				id,
				value,
				_header._id  // TID from here
			);

			if (_entries.size() >= _maxEntries)
				flushBuffer();

			assert(_entries.size() < _maxEntries);
		}

		const TraceHeader &getHeader() const
		{
			return _header;
		}
	}; // Buffer


	template <typename T>
	class NameSet {
	public:
		static constexpr T maxUserEvent = std::numeric_limits<T>::max() / 2;
		static constexpr T maxEvent = std::numeric_limits<T>::max();

		T registerName(const std::string &name, T value)
		{
			if (value > maxUserEvent)
			{
				const std::string message
					= "Cannot register event: '" + name
					+ "' with id: " + std::to_string(value)
					+ " user value limit is: '" + std::to_string(value) + "'";
				throw std::runtime_error(message);
			}

			std::lock_guard<std::mutex> lk(_namesMutex);
			auto it = _namesMap.emplace(value, name);
			const std::string valueInside = it.first->second;

			if (valueInside != name)
			{
				const std::string message
					= "Cannot register event: '" + name
					+ "' with id: " + std::to_string(value)
					+ " the id is already taken by: '" + valueInside + "'";
				throw std::runtime_error(message);
			}

			return value;
		}

		T autoRegisterName(const std::string &name)
		{
			std::lock_guard<std::mutex> lk(_namesMutex);
			auto it =_namesMap.lower_bound(_counter);

			while ((it = _namesMap.emplace_hint(it, ++_counter, name))->second != name) {
				// If counter goes to zero there is overflow, so, no empty places.
				if (_counter == maxEvent)
					throw std::runtime_error("Profiler cannot register event: " + name);
			}

			return _counter;
		}

		auto begin() const
		{
			return _namesMap.begin();
		}

		auto end() const
		{
			return _namesMap.end();
		}

	private:
		std::mutex _namesMutex;	             /**< mutex needed to write in the global file */
		T _counter = maxUserEvent;           /**< counter for automatic function registration */
		std::map<T, std::string> _namesMap;  /**< map with the events names */
	};


	/**
	   Class for global singleton.
	*/
	template<size_t BUFFERSIZE>	 //< Maximum size for the buffers ~ 1Mb >/
	class BufferSet {
		/**
		   Static utility function to build the trace directory
		*/
		static std::string getTraceDirectory(uint64_t systemTimePoint)
		{
			const time_t localTime = static_cast<time_t>(systemTimePoint);

			std::stringstream ss;
			ss << "TRACEDIR_" << std::put_time(std::localtime(&localTime), "%Y-%m-%d_%H_%M_%S");
			return ss.str();
		}

	public:

		static BufferSet<BUFFERSIZE> _singleton;                     /**< This is the global singleton for all the profiler */

		/**
		   Buffer set constructor.

		   This is used to construct a singleton stored in a global
		   variable.  The constructor and destructor automatically emit the
		   thread creation event.  Which in main thread (id == 1) is the
		   full execution time.
		*/
		BufferSet();

		~BufferSet();


		/**
		   Get the Buffer_t associated with a thread id hash

		   The threadIds are usually reused after a thread is destroyed.
		   Opening/closing files on every thread creation/deletion may be
		   too expensive; especially if the threads are created destroyed
		   very frequently.

		   We keep the associative map <tid, Buffer> in order to reuse
		   Buffer and only execute IO operations when the buffer is full or
		   at the end of the execution.

		   The extra cost for this is that we need to take a lock once (on
		   thread construction or when emitting the first event from a
		   thread) in order to get it's associated buffer.  This function is
		   responsible to take the lock and return the associated buffer.
		   When a threadId is seen for a first time this function creates
		   the new entry in the map, construct the Buffer and assign an
		   ordinal id for it.  Any optimization here will be very welcome.
		*/
		Buffer<BUFFERSIZE> &getEventsMap(size_t tid)
		{
			// We attempt to tale the read lock first. If this tid was
			// already used, the buffer must be already created, and we
			// don't need the exclusive access.
			std::shared_lock sharedlock(_mapMutex);
			auto it = _eventsMap.lower_bound(tid);

			if (it != _eventsMap.end() && it->first == tid)
				return it->second;

			// Else, this is the first time we use this tid, so, we need
			// exclusive access to modify the map. So, let's release the
			// read lock and try to take the write (unique) lock.
			sharedlock.release();
			_mapMutex.unlock_shared();
			std::unique_lock uniquelock(_mapMutex); // Now lock exclusively

			std::string filename
				= _traceDirectory + "/Trace_" + std::to_string(_tcounter) + ".bin";

			it = _eventsMap.try_emplace(
				it, tid, _tcounter++, tid, filename, _startSystemTimePoint
			);

			return it->second;
		}

		/**
		   Add a line to the report text file.

		   Use this functions with caution due to it takes a lock cause the
		   file is shared.
		*/
		void AddToReport(const std::string& text)
		{
			std::lock_guard<std::mutex> guard(_fileMutex);
			if (!_file.is_open()) {
				_file.open(_traceDirectory + "/Trace.txt", std::ios::out);
				assert(_file.is_open());
			}

			_file << text << std::endl;
		}


	private:
		const uint64_t _startSystemTimePoint;
		const std::string _traceDirectory;

		std::mutex _fileMutex;	 /**< mutex needed to write in the global file */
		std::ofstream _file;     /**< report global file */

		mutable std::shared_mutex _mapMutex;             /**< mutex needed to access the _eventsMap */
		std::map<size_t, Buffer<BUFFERSIZE>> _eventsMap; /**< This map contains the relation tid->id */
		uint32_t _tcounter = 1;                          /**< tid counter always > 0 */

		// Events names register
		NameSet<uint16_t> _eventsNames;

		friend uint16_t registerName(const std::string &name, uint16_t value);

	public:
		const uint16_t threadEventID;

	}; // BufferSet

	template <size_t T>
	BufferSet<T> BufferSet<T>::_singleton;

	inline uint16_t registerName(const std::string &name, uint16_t value = 0)
	{
		if (value != 0)
			return BufferSet<(1 << 20)>::_singleton._eventsNames.registerName(name, value);

		return BufferSet<(1 << 20)>::_singleton._eventsNames.autoRegisterName(name);
	}


	/**
	   Class for thread local singleton.
	*/
	template<size_t BUFFERSIZE>	 //< Maximum size for the buffers ~ 1Mb >/
	class InfoThread {
		thread_local static InfoThread<BUFFERSIZE> _singletonThread; /**< Thread local singleton */

		const size_t _tid;
		Buffer<BUFFERSIZE> &_threadBuffer;

	public:
		static Buffer<BUFFERSIZE> &getBuffer()
		{
			return _singletonThread._threadBuffer;
		}

		static void emplaceEvent(uint16_t id, uint16_t value)
		{
			getBuffer().emplace(id, value);
		}

		/**
		   Thread local Info initialization.

		   The class is actually constructed the first time the thread
		   local variables is accesses. But the buffer is not destroyed on
		   thread finalization because the same threadID may be reused in
		   the future.  This emits an event of type 2 and value tid (which
		   is always bigger than zero).
		*/
		InfoThread()
			: _tid(std::hash<std::thread::id>()(std::this_thread::get_id())),
			  _threadBuffer(BufferSet<BUFFERSIZE>::_singleton.getEventsMap(_tid))
		{
			assert(_tid == _threadBuffer.getHeader()._tid);

			emplaceEvent(BufferSet<BUFFERSIZE>::_singleton.threadEventID,
			             _threadBuffer.getHeader()._id);
		}

		~InfoThread()
		{
			emplaceEvent(BufferSet<BUFFERSIZE>::_singleton.threadEventID, 0);
		}

	}; // InfoThread

	template <size_t T>
	thread_local InfoThread<T> InfoThread<T>::_singletonThread;



	/**
	   Guard class (more info in the constructor docstring)
	 */
	template<size_t BUFFERSIZE = (1 << 20)>	 //< Maximum size for the buffers ~ 1Mb >/
	class ProfilerGuard {

		const uint16_t _id;  /**< Event id for this guard. remembered to emit on the destructor */

	  public:

		// Profile guard should be unique.
		ProfilerGuard(const ProfilerGuard &) = delete;
		ProfilerGuard& operator=(const ProfilerGuard &) = delete;

		/**
		   Guard constructor.

		   This is a tricky variable to rely event pairs emission (start-end)
		   with RAII. This simplifies instrumentation on the user side and may
		   rely on the instrumentation macro.
		   The constructor emits an event that will be paired with the
		   equivalent one emitted in the destructor.
		 */
		ProfilerGuard(uint16_t id, uint16_t value)
			: _id(id)
		{
			assert(value != 0);
			InfoThread<BUFFERSIZE>::emplaceEvent(_id, value);
		}

		~ProfilerGuard()
		{
			InfoThread<BUFFERSIZE>::emplaceEvent(_id, 0);
		}

	}; // ProfilerGuard

	// ==================================================
	// Outline function definitions.
	// ==================================================

	template <size_t BUFFERSIZE>
	Buffer<BUFFERSIZE>::Buffer(
		uint16_t id, uint64_t tid, std::string fileName, uint64_t startGTime
	)
		: _header(id, tid, startGTime),
		  _fileName(std::move(fileName)),
		  _entries()
	{
		// Reseerve memory for the buffer.
		_entries.reserve(_maxEntries);
	}

	// This is declared outside the class because it calls members of ProfilerGuard singleton.
	template <size_t BUFFERSIZE>
	Buffer<BUFFERSIZE>::~Buffer()
	{
		// Early exit without writing anything if this buffer was moved somewhere else.
		if (_fileName.empty())
		{
			assert(_header._totalFlushed == 0);
			assert(_entries.size() == 0);
			return;
		}

		flushBuffer(); // Flush all remaining events
		_file.close(); // close the file only at the end.

		BufferSet<BUFFERSIZE>::_singleton.AddToReport(_fileName);
	}


	template <size_t BUFFERSIZE>
	BufferSet<BUFFERSIZE>::BufferSet():
		_startSystemTimePoint(std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()).time_since_epoch().count()),
		_traceDirectory(getTraceDirectory(_startSystemTimePoint)),
		_eventsNames(),
		threadEventID(_eventsNames.autoRegisterName("ThreadRunning"))
	{
		// Make just a trivial check to force the first access to the
		// _singletonThread construct it at the very beginning.
		//This is because the thread-local variables are constructed on demand,
		// but the static are built before main  (eagerly)
		
		uint32_t thread_id	= InfoThread<BUFFERSIZE>::getBuffer().getHeader()._id;

		if (thread_id != 1)
			throw std::runtime_error("Master is not running in the first thread");

		// Create the directory
		if (!std::filesystem::create_directory(_traceDirectory))
			throw std::runtime_error("Cannot create traces directory: " + _traceDirectory);
	}

	template <size_t BUFFERSIZE>
	BufferSet<BUFFERSIZE>::~BufferSet()
	{
		std::string ret;

		for (auto it : _eventsNames)
			ret += std::to_string(it.first) + " " + it.second + "\n";

		this->AddToReport(ret);
	}

}
