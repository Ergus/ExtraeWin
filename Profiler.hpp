#pragma once

#if PROFILER_ENABLED > 0

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

	#include <windows.h>
	#include <processthreadsapi.h>
	#include <winbase.h>

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
		return GetCurrentProcessorNumber();
		// PROCESSOR_NUMBER procNumber;
		// GetCurrentProcessorNumberEx(&procNumber);
		// return procNumber.Group * 64 + procNumber.Number + 1;
	}

	inline std::string getHostName()
	{
		TCHAR  infoBuf[32767];
		DWORD  bufCharCount = 32767;

		// Get and display the name of the computer.
		if (!GetComputerName( infoBuf, &bufCharCount ) )
		{
			perror("GetComputerName failed");
			abort();
		} 

		return infoBuf;
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

	std::string getHostName()
	{
		constexpr size_t  len = 128;
		char  infoBuf[len];

		if (gethostname(infoBuf, len) != 0)
			perror("Error getting hostname");

		return infoBuf;
	}

#endif

	/**
	   Get microseconds since the trace begins for a given timePoint
	*/
	inline uint64_t getNanoseconds()
	{
		// Store the very first time we enter this function and then return the
		// number of nanoseconds AFTER this first call.
		static const std::chrono::high_resolution_clock::time_point begin
			= std::chrono::high_resolution_clock::now();

		return std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::high_resolution_clock::now() - begin).count();
	}

	// ==================================================
	// End of the basic functions
	// ==================================================

	/**
	   Buffer class to store the events.

	   This class will always have a file associated to it to flush all the data
	   when needed.  There will be 1 Buffer/Core in order to make the tracing
	   events registration completely lock free.
	*/
	template <size_t BUFFERSIZE>
	class Buffer {

		/**
		   Header struct

		   This is the struct that will be written in the header of the
		   file. This is update don every flush and keeps information needed in
		   the head of the file to read it latter.
		 */
		struct TraceHeader {
			uint32_t _id;   // Can be cpuID or ThreadID
			uint32_t _totalFlushed;
			uint64_t _tid;
			uint64_t _startGTime;    /**< Start global time >*/

			TraceHeader(uint32_t id, uint64_t tid, uint64_t startGTime)
				: _id(id), _totalFlushed(0), _tid(tid), _startGTime(startGTime)
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
			) : _time(getNanoseconds()),
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
		Buffer(const Buffer &) = delete;
		Buffer &operator=(const Buffer &) =  delete;

		Buffer(uint16_t id, uint64_t tid, std::string fileName, uint64_t startGTime);

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

	/**
	   Name set thread save map container.

	   This is a thread safe container to register the relation between event
	   name and id. This container is intend to be accessed only once/event to
	   register the name of the event.
	 */
	template <typename T>
	class NameSet {

		struct nameInfo
		{
			std::string name;
			std::string fileName;
			size_t line;

			// Needed to compare entries
			bool operator==(const nameInfo &other) const
			{
				return name == other.name
					&& fileName == other.fileName
					&& line == other.line;
			}
			bool operator!=(const nameInfo &other) const
			{
				return !(*this == other);
			}

			operator std::string() const
			{
				return name + " ("+ fileName + ":" + std::to_string(line) + ")";
			}
		};


	public:
		static constexpr T maxUserEvent = std::numeric_limits<T>::max() / 2;
		static constexpr T maxEvent = std::numeric_limits<T>::max();

		struct nameEntry : public nameInfo
		{
			std::map<T, nameInfo> _namesValuesMap;
		};

		T registerEventName(
			const std::string &name, T event,
			const std::string &fileName, size_t line
		)
		{
			assert(!name.empty());

			if (event > maxUserEvent)
			{
				const std::string message
					= "Cannot register event: '" + name
					+ "' with id: " + std::to_string(event)
					+ " user event value limit is: '" + std::to_string(maxUserEvent) + "'";
				throw std::runtime_error(message);
			}

			std::lock_guard<std::mutex> lk(_namesMutex);
			auto it = _namesEventMap.emplace(event, nameEntry{name, fileName, line});

			// If not inserted
			if (!it.second)
			{
				const nameInfo &eventInside = it.first->second;

				const std::string message
					= "Cannot register event: '" + name
					+ "' with id: " + std::to_string(event)
					+ " the id is already taken by: '" + std::string(eventInside) + "'";
				throw std::runtime_error(message);
			}

			return event;
		}

		T registerValueName(
			const std::string &name, T event, T value,
			const std::string &fileName, size_t line
		)
		{
			assert(!name.empty());

			std::lock_guard<std::mutex> lk(_namesMutex);
			auto itEvent = _namesEventMap.find(event);

			if (itEvent == _namesEventMap.end())
			{
				const std::string message
					= "Cannot register event value: '" + name
					+ "' with id: " + std::to_string(event) + ":" + std::to_string(value)
					+ " the event ID does not exist.";
				throw std::runtime_error(message);
			}

			auto itValue = itEvent->second._namesValuesMap.emplace(value, nameEntry{name, fileName, line});
			if (!itValue.second)
			{
				const std::string message
					= "Cannot cannot register event value: '" + name
					+ "' with id: " + std::to_string(event) + ":" + std::to_string(value)
					+ " it is already taken by '" + itValue.first->second.name;
				throw std::runtime_error(message);
			}

			return event;
		}

		T autoRegisterName(
			const std::string &name,
			const std::string &fileName = "profiler", size_t line = 0
		)
		{
			assert(!name.empty());
			nameEntry entry {
				name, fileName, line
			};

			std::lock_guard<std::mutex> lk(_namesMutex);
			auto it =_namesEventMap.lower_bound(_counter);

			while ((it = _namesEventMap.emplace_hint(it, ++_counter, entry))->second != entry) {
				// If counter goes to zero there is overflow, so, no empty places.
				if (_counter == maxEvent)
					throw std::runtime_error("Profiler cannot register event: " + name);
			}

			return _counter;
		}

		auto begin() const
		{
			return _namesEventMap.begin();
		}

		auto end() const
		{
			return _namesEventMap.end();
		}

	private:
		std::mutex _namesMutex;	             /**< mutex needed to write in the global file */
		T _counter = maxUserEvent;           /**< counter for automatic function registration */
		std::map<T, nameEntry> _namesEventMap;    /**< map with the events names */
	}; // NameSet


	/**
	   BufferSet container

	   This is container stores the buffer for every thread. in a map <tid,
	   Buffer> This is intended to remember the tid to reuse the Buffer because
	   the tid is usually recycled after a thread is deleted.
	   This class is allocated inside a shared_ptr to enforce that it will be
	   deleted only after the destruction of all the threads.
	   The Global container holds a reference to it; but every ThreadInfo will
	   also keep one reference.

	   This is because it seems like on GNU/Linux the global variables are
	   destructed after the main thread; but in MSWindows the Global variables
	   seems to be removed before the main thread completes.
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
		Buffer<BUFFERSIZE> &getThreadBuffer(size_t tid)
		{
			// We attempt to tale the read lock first. If this tid was
			// already used, the buffer must be already created, and we
			// don't need the exclusive access.
			std::shared_lock sharedlock(_mapMutex);
			auto it = _eventsMap.lower_bound(tid);

			if (it != _eventsMap.end() && it->first == tid)
				return it->second;

			// === else === create new entry: <tid, id>
			// Else, this is the first time we use this tid, so, we need
			// exclusive access to modify the map. So, let's release the
			// read lock and try to take the write (unique) lock.
			sharedlock.release();
			_mapMutex.unlock_shared();
			std::unique_lock uniquelock(_mapMutex); // Now lock exclusively

			std::string filename
				= _traceDirectory + "/Trace_" + std::to_string(_tcounter) + ".bin";

			it = _eventsMap.try_emplace(it, tid, _tcounter++, tid, filename, _startSystemTimePoint);

			return it->second;
		}


	private:
		const uint64_t _startSystemTimePoint;
		const std::string _traceDirectory;

		mutable std::shared_mutex _mapMutex;             /**< mutex needed to access the _eventsMap */
		std::map<size_t, Buffer<BUFFERSIZE>> _eventsMap; /**< This map contains the relation tid->id */
		uint32_t _tcounter = 1;                          /**< tid counter always > 0 */

		friend uint16_t registerName(const std::string &name, uint16_t value);

	public:
		// Events names register
		NameSet<uint16_t> eventsNames;

		const uint16_t threadEventID;

	}; // BufferSet


	/**
	   Class for thread local singleton.

	   This class will be allocated in a copy/thead in order to perform the
	   events emission completely thread free.
	   The constructor of this object takes place the first time an event is
	   emitted from some thread; so it is not very accurate to use it to measure
	   total thread duration. However, it is the best we have until we can
	   enforce some thread hooks.
	*/
	template<size_t I>	 //< Maximum size for the buffers ~ 1Mb >/
	class InfoThread {
		const size_t _tid;

	public:

		std::shared_ptr<BufferSet<I>> globalBufferSet;
		Buffer<I> &buffer;

		/**
		   Thread local Info initialization.

		   The class is actually constructed the first time the thread
		   local variables is accesses. But the buffer is not destroyed on
		   thread finalization because the same threadID may be reused in
		   the future.  This emits an event of type 2 and value tid (which
		   is always bigger than zero).
		*/
		InfoThread();

		~InfoThread()
		{
			buffer.emplace(globalBufferSet->threadEventID, 0);
		}
	}; // InfoThread

	/**
	   Info container with global variables.

	   This gives access to the thread and global static variables. And only
	   holds one pointer to the BufferSet object to avoid its premature
	   deletion.
	 */
	template <size_t I>
	class Global {

	public:

		static InfoThread<I> getThreadInfo()
		{
			thread_local static InfoThread<I> threadInfo;
			return threadInfo;
		}

		std::shared_ptr<BufferSet<I>> _singleton;

		Global()
			: _singleton(new BufferSet<I>())
		{
			// Make just a trivial check to force the first access to the
			// _singletonThread construct it at the very beginning.
			// This is because the thread-local variables are constructed on demand,
			// but the static are built before main  (eagerly) So we need to do this
			// to compute the real execution time.
			if (getThreadInfo().buffer.getHeader()._id != 1)
				throw std::runtime_error("Master is not running in the first thread");

		}

		static Global globalInfo;

	};

	template <size_t I>
	Global<I> Global<I>::globalInfo;

	/**
	   Public function to create new events.

	   This registers a new pair eventName -> value wrapping Object oriented calls.
	 */
	inline uint16_t registerName(
		std::string name, const std::string &fileName, size_t line,
		uint16_t event = 0, uint16_t value = 0
	)
	{
		constexpr size_t I = (1 << 20);

		if (name.empty())
			name = fileName+":"+std::to_string(line);

		if (event == 0)
			return Global<I>::getThreadInfo().globalBufferSet->eventsNames.autoRegisterName(name, fileName, line);
		else if (value == 0)
			return Global<I>::getThreadInfo().globalBufferSet->eventsNames.registerEventName(name, event, fileName, line);
		else
			return Global<I>::getThreadInfo().globalBufferSet->eventsNames.registerValueName(name, event, value, fileName, line);
	}


	/**
	   Guard class (more info in the constructor docstring)

	   This is a tricky variable to rely event pairs emission (start-end)
	   with RAII. This simplifies instrumentation on the user side and may
	   rely on the instrumentation macro.
	   The constructor emits an event that will be paired with the
	   equivalent one emitted in the destructor.
	 */
	template<size_t I = (1 << 20)>	 //< Maximum size for the buffers ~ 1Mb >/
	class ProfilerGuard {

		const uint16_t _id;  /**< Event id for this guard. remembered to emit on the destructor */

	  public:

		// Profile guard should be unique.
		ProfilerGuard(const ProfilerGuard &) = delete;
		ProfilerGuard& operator=(const ProfilerGuard &) = delete;

		/**
		   Guard constructor.
		 */
		ProfilerGuard(uint16_t id, uint16_t value)
			: _id(id)
		{
			assert(value != 0);
			Global<I>::getThreadInfo().buffer.emplace(_id, value);
		}

		~ProfilerGuard()
		{
			Global<I>::getThreadInfo().buffer.emplace(_id, 0);
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
		// Reserve memory for the buffer.
		_entries.reserve(_maxEntries);
	}


	template <size_t BUFFERSIZE>
	Buffer<BUFFERSIZE>::~Buffer()
	{

		flushBuffer(); // Flush all remaining events
		_file.close(); // close the file only at the end.
	}


	template <size_t I>
	BufferSet<I>::BufferSet():
		_startSystemTimePoint(std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()).time_since_epoch().count()),
		_traceDirectory(getTraceDirectory(_startSystemTimePoint)),
		eventsNames(),
		threadEventID(eventsNames.autoRegisterName("ThreadRunning"))
	{
		// Create the directory
		if (!std::filesystem::create_directory(_traceDirectory))
			throw std::runtime_error("Cannot create traces directory: " + _traceDirectory);
	}


	template <size_t I>
	BufferSet<I>::~BufferSet()
	{
		std::string hostname = getHostName();
		int ncores = getNumberOfCores();
		size_t nthreads = _tcounter - 1;

		// ROW File
		std::ofstream rowfile(_traceDirectory + "/Trace.row", std::ios::out);

		rowfile << "LEVEL CPU SIZE " << ncores << std::endl;
		for (int i = 1; i <= ncores; ++i)
			rowfile << i << "." << hostname << std::endl;

		rowfile << "\nLEVEL NODE SIZE 1" << std::endl;
		rowfile << hostname << std::endl;

		rowfile << "\nLEVEL THREAD SIZE " << nthreads << std::endl;
		for (size_t i = 1; i <= nthreads; ++i)
			rowfile << "THREAD 1.1." << i << std::endl;
		rowfile.close();

		// PCF File
		std::ofstream pcffile(_traceDirectory + "/Trace.pcf", std::ios::out);

		// Register all Events types names.
		for (auto it : eventsNames)
		{
			const NameSet<uint16_t>::nameEntry &eventEntry = it.second;

			pcffile << "# " << eventEntry.fileName << ":" <<  eventEntry.line << std::endl;
			pcffile << "EVENT_TYPE" << std::endl;
			pcffile << "0 " << it.first << " " << eventEntry.name << std::endl;

			// Create a "VALUES" sections if some value is registered for this event
			if (!eventEntry._namesValuesMap.empty())
			{
				pcffile << "VALUES" << std::endl;
				for (auto itValues : eventEntry._namesValuesMap)
					pcffile << itValues.first << " "
					        << eventEntry.name << ":" << itValues.second.name << std::endl;
			}

			pcffile << std::endl;

		}
		pcffile.close();

		std::cout << "# Profiler TraceDir: " << _traceDirectory << std::endl;
	}


	template <size_t I>
	InfoThread<I>::InfoThread()
		: _tid(std::hash<std::thread::id>()(std::this_thread::get_id())),
		  globalBufferSet(Global<I>::globalInfo._singleton),
		  buffer(globalBufferSet->getThreadBuffer(_tid))
		{
			assert(_tid == buffer.getHeader()._tid);
			buffer.emplace(globalBufferSet->threadEventID, buffer.getHeader()._id);
		}
}

/**
  \defgroup public interface
  \brief This is the simpler linker list and its functions

  Here starts what is intended to be the public interface:
  A set of macros that generate instrumentation when PROFILER_ENABLED > 0
  otherwise they expand to nothing.
  @{
*/

#define TOKEN_PASTE(x, y) x##y
#define CAT(X,Y) TOKEN_PASTE(X,Y)

#define INSTRUMENT_SCOPE(EVENT, VALUE, ...)							\
	static uint16_t CAT(__profiler_id_,EVENT) =							\
		profiler::registerName(std::string(__VA_ARGS__), __FILE__, __LINE__, EVENT); \
	profiler::ProfilerGuard guard(EVENT, VALUE);

/**
   Main macro to instrument functions.

   This macro creates a new event value = 1 for the __profiler_function_id event.
   The event start is emitted when the macro is called and extend until the
   calling scope finalizes.
   This is intended to be called immediately after a function starts.
 */
#define INSTRUMENT_FUNCTION												\
	static uint16_t __profiler_function_id =							\
		profiler::registerName(__func__, __FILE__, __LINE__);			\
	profiler::ProfilerGuard guard(__profiler_function_id, 1);

/**
   Main macro to instrument functions subsections.

   This macro creates a new event value for the __profiler_function_id event.
   An extra second string argument can be passed to the macro in order to set a
   custom name to the event value. Otherwise the __funct__:__LINE__ will be used.
   \param VALUE the numeric value for the event.
 */
#define INSTRUMENT_FUNCTION_UPDATE(VALUE, ...)						\
	static uint16_t CAT(__profiler_function_,__LINE__) =				\
		profiler::registerName(std::string(__VA_ARGS__), __FILE__, __LINE__, __profiler_function_id, VALUE); \
	profiler::Global<(1 << 20)>::getThreadInfo().buffer.emplace(__profiler_function_id, VALUE)

//!@}

#else

#define INSTRUMENT_FUNCTION
#define INSTRUMENT_EVENT
#define INSTRUMENT_FUNCTION_UPDATE(VALUE)
#define INSTRUMENT_FUNCTION_UPDATE2(VALUE, NAME)

#endif //
