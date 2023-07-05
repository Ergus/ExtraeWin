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

		struct TraceHeader {
			uint32_t _id;   // Can be cpuID or ThreadID
			uint32_t _totalFlushed;
			uint64_t _tid;

			TraceHeader(TraceHeader&& other)
				: _id(other._id),
				  _tid(other._tid),
				  _totalFlushed(other._totalFlushed)
			{
				other._id = std::numeric_limits<unsigned int>::max();
				other._tid = std::numeric_limits<unsigned int>::max();
				other._totalFlushed = 0;
			}

			TraceHeader(uint32_t id, uint64_t tid)
				: _id(id), _tid(tid), _totalFlushed(0)
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
			const uint32_t _time;
			const uint8_t _id;
			const uint8_t _value;
			const uint8_t _core;
			const uint8_t _thread;

			explicit EventEntry(
				uint32_t time, int8_t id, uint8_t value,  uint8_t core, uint8_t thread
			) : _time(time), _id(id), _value(value), _core(core), _thread(thread)
			{
			}
		};

		static constexpr size_t _maxEntries
			= ( BUFFERSIZE + sizeof(EventEntry) - 1 ) / sizeof(EventEntry);	 //< Maximum size for the buffers ~ 1Mb >/

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

		Buffer(uint32_t id, uint64_t tid, std::string fileName);

		Buffer(Buffer&& other)
			: _header(std::move(other._header)),
			  _fileName(other._fileName),
			  _file(std::move(other._file)),
			  _entries(std::move(other._entries))
		{
		}


		/** Destructor for the buffer type. */
		~Buffer();

		/**
		   Add (report) a new event.

		   This adds a new entry in the buffer with a value and id. when id is
		   zero it means that it is the end of the event
		*/
		template <typename ...T>
		void emplace(T&&... args)
		{
			_entries.emplace_back(std::forward<T>(args)..., _header._id);

			if (_entries.size() >= _maxEntries)
				flushBuffer();

			assert(_entries.size() < _maxEntries);
		}

		void emplace2(uint8_t id, uint8_t value)
		{
			this->emplace(
				getMicroseconds(std::chrono::high_resolution_clock::now()),
				id,
				value,
				getCPUId()
			);
		}

		const TraceHeader &getHeader() const
		{
			return _header;
		}
	}; // Buffer

	template<size_t BUFFERSIZE = (1 << 20)>	 //< Maximum size for the buffers ~ 1Mb >/
	class ProfilerGuard {

		class InfoThread
		{
		public:
			const size_t _tid;
			Buffer<BUFFERSIZE> &_threadBuffer;

			InfoThread()
				: _tid(std::hash<std::thread::id>()(std::this_thread::get_id())),
				  _threadBuffer(ProfilerGuard::_singleton.getEventsMap(_tid))
			{
				assert(_tid == _threadBuffer.getHeader()._tid);
				_threadBuffer.emplace2(2, 1);
			}

			~InfoThread()
			{
				_threadBuffer.emplace2(2, 0);
			}

		};


		class BufferSet {
			static std::string getTraceDirectory(
				std::chrono::time_point<std::chrono::system_clock> startTimePoint
			)
			{
				auto localTime = std::chrono::system_clock::to_time_t(startTimePoint);
				std::stringstream ss;
				ss << "TRACEDIR_" << std::put_time(std::localtime(&localTime), "%Y-%m-%d_%H_%M_%S");
				return ss.str();
			}


		  public:

			BufferSet() :
				_startTimePoint(std::chrono::system_clock::now()),
				_traceDirectory(getTraceDirectory(_startTimePoint))
			{
				// Create the directory
				if (!std::filesystem::create_directory(_traceDirectory))
					throw  std::runtime_error("Cannot create traces directory: " + _traceDirectory);

				_file.open(_traceDirectory + "/Trace.txt", std::ios::out);
				assert(_file.is_open());

				// This event is to record the monolitic time
				ProfilerGuard::_singletonThread._threadBuffer.emplace(0,0);

				// Event 1 is the start moment (to compute the whole trace execution)
				ProfilerGuard::_singletonThread._threadBuffer.emplace(
					getMicroseconds(_startTimePoint),
					1,
					1,
					getCPUId()
				);
			}

			~BufferSet()
			{
				ProfilerGuard::_singletonThread._threadBuffer.emplace(
					getMicroseconds(std::chrono::system_clock::now()),
					1,
					0,
					getCPUId()
				);
			}

			void AddReport(const std::string& filename)
			{
				std::lock_guard<std::mutex> guard(_fileMutex);
				assert(_file.is_open());
				_file << filename << std::endl;
			}

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
				std::unique_lock uniquelock(_mapMutex);

				std::string filename
					= _traceDirectory + "/Trace_" + std::to_string(_tcounter) + ".bin";

				it = _eventsMap.try_emplace(it, tid, _tcounter++, tid, filename);

				return it->second;
			}

		  private:
			const std::chrono::time_point<std::chrono::system_clock> _startTimePoint;
			const std::string _traceDirectory;

			std::mutex _fileMutex;	 //< mutex needed to write in the global file >/
			std::ofstream _file;

			mutable std::shared_mutex _mapMutex;	 //< mutex needed to write in the global file >/
			std::map<size_t, Buffer<BUFFERSIZE>> _eventsMap;
			uint32_t _tcounter = 1;  // Start on 1 because paraver uses 1 start index
		};

		const uint8_t _id;

		static void registerNewEvent(uint8_t id, uint8_t value)
		{
			_singletonThread._threadBuffer.emplace2(id, value);
		}

	  public:

		static BufferSet _singleton;
		thread_local static InfoThread _singletonThread;

		ProfilerGuard(const ProfilerGuard &) = delete;
		ProfilerGuard& operator=(const ProfilerGuard &) = delete;

		ProfilerGuard(uint8_t id, uint8_t value)
			: _id(id)
		{
			assert(value != 0);
			_singletonThread._threadBuffer.emplace2(_id, value);
		}

		~ProfilerGuard()
		{
			assert(std::hash<std::thread::id>()(std::this_thread::get_id()) == _singletonThread._tid);
			_singletonThread._threadBuffer.emplace2(_id, 0);
		}

	}; // ProfilerGuard

	// Outline constructors.
	template <size_t T>
	typename ProfilerGuard<T>::BufferSet ProfilerGuard<T>::_singleton;

	template <size_t T>
	thread_local typename ProfilerGuard<T>::InfoThread ProfilerGuard<T>::_singletonThread;

	template <size_t BUFFERSIZE>
	Buffer<BUFFERSIZE>::Buffer(uint32_t id, uint64_t tid, std::string fileName)
		: _header(id, tid),
		  _fileName(std::move(fileName)),
		  _entries(),
		  _file(_fileName, std::ios::out | std::ios::binary)
	{
		// Reserve space for the header
		_file.write(reinterpret_cast<char *>(&_header), sizeof(TraceHeader));

		// Reseerve memory for the buffer.
		_entries.reserve(_maxEntries);
		assert(_file.is_open());
	}

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

		flushBuffer(); // Flush all remaining events
		_file.seekp(0);
		_file.write(reinterpret_cast<char *>(&_header), sizeof(TraceHeader));
		_file.close(); // close the file

		ProfilerGuard<BUFFERSIZE>::_singleton.AddReport(_fileName);
	}
}
