#pragma once

#if PROFILER_ENABLED > 0

#define SNDEBUG NDEBUG
#undef NDEBUG

#include <iostream>
#include <string.h>
#include <vector>
#include <fstream>
#include <cassert>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <map>
#include <shared_mutex>

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)

#include <windows.h>
#include <processthreadsapi.h>
#include <winbase.h>

namespace {
	inline int getNumberOfCores() {
		SYSTEM_INFO sysinfo;
		GetSystemInfo(&sysinfo);
		return sysinfo.dwNumberOfProcessors;
	}

	//! Return the cpuID starting by 1
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

	inline int getPid()
	{
		return GetCurrentProcessId();
	}

	// In MSWindows this seems possible, but unneeded.
	inline void killPool()
	{
	}

	#define FUNC_NAME __FUNCSIG__
}

#else // defined(WIN32)

#include <unistd.h>
#include <algorithm>
#include <array>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#ifdef _PSTL_PAR_BACKEND_TBB // This macro is defined in gcc libraries
#include <oneapi/tbb/global_control.h>
#endif

namespace {

	inline int getNumberOfCores() {
		return sysconf(_SC_NPROCESSORS_ONLN);
	}

	//! Return the cpuID starting by 1
	inline unsigned int getCPUId()
	{
		const int cpu = sched_getcpu();
		assert(cpu >= 0);
		return cpu + 1;
	}

	inline std::string getHostName()
	{
		constexpr size_t  len = 128;
		char  infoBuf[len];

		if (gethostname(infoBuf, len) != 0)
			perror("Error getting hostname");

		return infoBuf;
	}

	inline int getPid()
	{
		return getpid();
	}

	// function to kill tbb thread-pool on linux.
	inline void killPool()
	{
		// In principle this works with clang and gcc... still need to check intel compiler
		#ifdef _PSTL_PAR_BACKEND_TBB
		oneapi::tbb::task_scheduler_handle handle
			= oneapi::tbb::task_scheduler_handle{oneapi::tbb::attach{}};
		oneapi::tbb::finalize(handle);
		#endif
	}

	#define FUNC_NAME __PRETTY_FUNCTION__

}

#endif // defined(WIN32)

// Expand the empty namespace to add the common functions
namespace {

	//! Get nanoseconds since the trace begins for a given timePoint
	inline uint64_t getNanoseconds()
	{
		// Store the very first time we enter this function and then return the
		// number of nanoseconds AFTER this first call.
		static const std::chrono::steady_clock::time_point begin
			= std::chrono::steady_clock::now();

		thread_local std::chrono::steady_clock::time_point last
			= std::chrono::steady_clock::now();

		std::chrono::steady_clock::time_point current
			= std::chrono::steady_clock::now();

		if (current < last)
			throw std::runtime_error("Consecutive time error");

		last = current;

		return std::chrono::duration_cast<std::chrono::nanoseconds>(current - begin).count();
	}


}

namespace profiler {

	constexpr size_t bSize = (1 << 20);  //! Default buffer size in bytes (1Mb)

	/** Type used for event IDs and sub-value indices throughout the profiler. */
	using EventId = uint16_t;

	/** Type used for event measurement values stored in each EventEntry. */
	using EventValue = uint32_t;

	/** Custom error class to handle them if needed. */
	class profilerError : public std::exception {
		const std::string message;
    public:
		explicit profilerError(const std::string &msg)
			: message("Profiler error: " + msg) {}

		const char *what () const throw ()
		{
			return message.c_str();
		}
	};

	/** Buffer class to store the events.

		This class will always have a file associated to it to flush all the
		data when needed.  There will be 1 Buffer/Core in order to make the
		tracing events registration completely lock free. */
	class Buffer {

		/** Header struct

			This is the struct that will be written in the header of the
			file. This is update don every flush and keeps information needed in
			the head of the file to read it latter. */
		struct TraceHeader {
			const uint32_t _id;   // Can be ThreadID
			uint32_t _totalFlushed {0};
			const uint64_t _tid;
			uint64_t _startGTime;    /**< Start global time >*/

			TraceHeader(uint32_t id, uint64_t tid, uint64_t startGTime)
				: _id(id), _tid(tid), _startGTime(startGTime)
			{}

		};

		//! Event struct that will be reported (and saved into the traces files in binary format)
		/** I reserve all the negative values for internal events. And positive
			values for user/application events.
			Every starting event needs and end event. But I cannot ensure that they
			will be in the same cpu (due to thread migration), but they will in the
			same thread. So, I also save the threadId with every event in order to
			make a right reconstruction latter. */
		struct EventEntry {
			const uint64_t _time;
			const EventId  _id;
			const uint16_t _core;
			const EventValue _value;

			EventEntry(EventId id, EventValue value, uint64_t time, uint16_t core)
				: _time(time)
				, _id(id)
				, _core(core)
				, _value(value)
			{
			}

			explicit EventEntry(EventId id, EventValue value)
				: EventEntry(id, value, getNanoseconds(), getCPUId())
			{
			}
		};

		/** Maximum size for the buffers = 1Mb */
		static constexpr size_t _maxEntries = ( bSize + sizeof(EventEntry) - 1 ) / sizeof(EventEntry);

		const std::string _fileName;      //< Name of the binary file with trace information
		std::ofstream _file;		      //< fstream with file open; every write will be appended and binary. >/

		EventEntry *_entries {nullptr};   //< Buffer with entries; it will be flushed when the number of entries reach _maxEntries;
		size_t _nEntries {0};

		void flushBuffer()
		{
			if (_nEntries == 0)
				return;

			// We open the file the first time we need t flush the data.
			if (!_file.is_open()) {
				_file.open(_fileName, std::ios::out | std::ios::binary);

				// Reserve space for the header
				_file.write(reinterpret_cast<char *>(&_header), sizeof(TraceHeader));
			}

			for (size_t i = 1; i < _nEntries; ++i) {
				if (_entries[i - 1]._time > _entries[i]._time)
					std::cerr << "Two events are not time consecutive: " <<  std::to_string(i) << std::endl;
			}

			std::cout << "# Flushed: " << _fileName << " << " << _header._totalFlushed << " + " << _nEntries << std::endl;
			_header._totalFlushed += _nEntries;

			// Go to beginning and write the header
			_file.seekp(0, std::ios_base::beg);
			_file.write(reinterpret_cast<char *>(&_header), sizeof(TraceHeader));

			// Go to end and write the data
			_file.seekp(0, std::ios_base::end);
			_file.write(reinterpret_cast<char *>(_entries), _nEntries * sizeof(EventEntry));

			// Clear the buffer.
			_nEntries = 0;
		}

	public:
		// No copy
		Buffer(Buffer &&) = delete;
		Buffer(const Buffer &) = delete;
		Buffer &operator=(const Buffer &) = delete;

		Buffer(
			uint32_t id, uint64_t tid, const std::string &fileName, uint64_t startGTime
		) : _fileName(fileName)
			, _header(id, tid, startGTime)
		{
			// Reserve memory for the buffer.
			_entries = (EventEntry *) malloc(_maxEntries * sizeof(EventEntry));
			assert(_entries);
		}

		~Buffer()
		{
			flushBuffer(); // Flush all remaining events
			free(_entries);
			_file.close(); // close the file only at the end.
		}

		void emplaceEvent(EventId id, EventValue value)
		{
			new (&_entries[_nEntries++]) EventEntry(id, value);

			if (_nEntries > 1 && _entries[_nEntries - 2]._time > _entries[_nEntries - 1]._time)
				throw profilerError("Registered event is not time consecutive: " + std::to_string(_nEntries - 1));

			assert(_nEntries <= _maxEntries);
			if (_nEntries == _maxEntries)
				flushBuffer();
		}

		void emplaceMultipleEvents(const std::pair<EventId, EventValue> *events, size_t count)
		{
			const uint64_t t    = getNanoseconds();
			const uint16_t core = static_cast<uint16_t>(getCPUId());
			for (size_t i = 0; i < count; ++i)
			{
				new (&_entries[_nEntries++]) EventEntry(events[i].first, events[i].second, t, core);

				if (_nEntries > 1 && _entries[_nEntries - 2]._time > _entries[_nEntries - 1]._time)
					throw profilerError("Registered event is not time consecutive: " + std::to_string(_nEntries - 1));

				assert(_nEntries <= _maxEntries);
				if (_nEntries == _maxEntries)
					flushBuffer();
			}
		}

		TraceHeader _header;

	}; // Buffer

	/** Name set thread safe map container.

		This is a thread safe container to register the relation between event
		name and id. This container is intended to be accessed only once/event to
		register the name of the event. */
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

		struct nameEntry : public nameInfo
		{
			std::map<EventId, nameInfo> _namesValuesMap {};
		};

	public:
		#undef max
		static constexpr EventId maxUserEvent = std::numeric_limits<EventId>::max() / 2;
		static constexpr EventId maxEvent = std::numeric_limits<EventId>::max();

		EventId registerEventName(
			std::string eventName,
			const std::string &fileName = "profiler",
			size_t line = 0,
			EventId event = EventId()
		) {
			if (eventName.empty())
			{
				std::filesystem::path p(fileName);
				eventName = p.filename().string()+":"+std::to_string(line);
			}

			// Fast path: name already registered — shared lock suffices.
			{
				std::shared_lock sharedLock(_namesMutex);
				const auto it = _nameToEventId.find(eventName);
				if (it != _nameToEventId.end())
					return it->second;
			}

			// Slow path: first time seeing this name — exclusive access to insert.
			std::unique_lock exclusiveLock(_namesMutex);

			// Double-check: another thread may have inserted between the two locks.
			{
				const auto it = _nameToEventId.find(eventName);
				if (it != _nameToEventId.end())
					return it->second;
			}

			nameEntry entry = {eventName, fileName, line};

			if (event == EventId())
				event = ++_counter;

			auto it_pair = _namesEventMap.emplace(event, entry);

			if (it_pair.second)
			{
				_nameToEventId[eventName] = event;
				return event;
			}

			const std::string message
				= "Cannot register event: '" + eventName
				+ "' with id: " + std::to_string(event)
				+ " the id is already taken by: '" + std::string(it_pair.first->second) + "'";
			throw profilerError(message);
		}

		EventId registerValueName(
			std::string valueName,
			const std::string &fileName, size_t line, EventId event, EventId value
		) {
			if (valueName.empty())
			{
				std::filesystem::path p(fileName);
				valueName = p.filename().string()+":"+std::to_string(line);
			}

			std::lock_guard<std::shared_mutex> lk(_namesMutex);
			auto itEvent = _namesEventMap.find(event);

			if (itEvent == _namesEventMap.end())
			{
				const std::string message
					= "Cannot register event value: '" + valueName
					+ "' with id: " + std::to_string(event) + ":" + std::to_string(value)
					+ " the event ID does not exist.";
				throw profilerError(message);
			}

			nameEntry entry = {valueName, fileName, line};
			auto it_pair = itEvent->second._namesValuesMap.emplace(value, entry);

			// Insertion succeeded, we can return
			if (it_pair.second)
				return value;

			const std::string message
				= "Cannot cannot register event value: '" + valueName
				+ "' with id: " + std::to_string(event) + ":" + std::to_string(value)
				+ " it is already taken by '" + it_pair.first->second.name + "'";
			throw profilerError(message);
		}

		void createPCF(const std::string &traceDirectory) const
		{
			// PCF File
			std::ofstream pcffile(traceDirectory + "/Trace.pcf", std::ios::out);

			// Register all Events types names.
			for (auto it : _namesEventMap)
			{
				const nameEntry &eventEntry = it.second;

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
		}

	private:
		std::shared_mutex _namesMutex;               /**< mutex needed to write in the global file */
		EventId _counter = maxUserEvent;             /**< counter for automatic function registration */
		std::map<EventId, nameEntry> _namesEventMap; /**< map with the events names */
		std::map<std::string, EventId> _nameToEventId; /**< reverse map: name → event ID (enables idempotent registration) */
	}; // NameSet


	/** BufferSet container

	   This is container stores the buffer for every thread. in a map <tid,
	   Buffer> This is intended to remember the tid to reuse the Buffer because
	   the tid is usually recycled after a thread is deleted.
	   This class is allocated inside a shared_ptr to enforce that it will be
	   deleted only after the destruction of all the threads.
	   The Global container holds a reference to it; but every ThreadInfo will
	   also keep one reference.

	   This is because it seems like on GNU/Linux the global variables are
	   destructed after the main thread; but in MSWindows the Global variables
	   seems to be removed before the main thread completes. */
	class BufferSet {
		std::shared_mutex _mapMutex;             /**< mutex needed to access the _eventsMap */
		std::map<size_t, Buffer> _eventsMap;     /**< This map contains the relation tid->id */

		uint32_t _tcounter = 1;                  /**< tid counter always > 0 */

		friend EventId registerName(const std::string &name, EventId value);

	public:

		void createROW(const std::string &traceDirectory) const
		{
			const std::string hostname = getHostName();
			const int ncores = getNumberOfCores();
			const size_t nthreads = _tcounter - 1;

			// ROW File
			std::ofstream rowfile(traceDirectory + "/Trace.row", std::ios::out);

			rowfile << "LEVEL CPU SIZE " << ncores << std::endl;
			for (int i = 1; i <= ncores; ++i)
				rowfile << i << "." << hostname << std::endl;

			rowfile << "\nLEVEL NODE SIZE 1" << std::endl;
			rowfile << hostname << std::endl;

			rowfile << "\nLEVEL THREAD SIZE " << nthreads << std::endl;
			for (size_t i = 1; i <= nthreads; ++i)
				rowfile << "THREAD 1.1." << i << std::endl;
			rowfile.close();
		}


		/** Get the Buffer_t associated with a thread id hash

		   The threadIds are usually reused after a thread is destroyed.
		   Opening/closing files on every thread creation/deletion may be too
		   expensive; especially if the threads are created destroyed very
		   frequently.

		   We keep the associative map <tid, Buffer> in order to reuse Buffer
		   and only execute IO operations when the buffer is full or at the end
		   of the execution.

		   The extra cost for this is that we need to take a lock once (on
		   thread construction or when emitting the first event from a thread)
		   in order to get it's associated buffer.  This function is responsible
		   to take the lock and return the associated buffer.  When a threadId
		   is seen for a first time this function creates the new entry in the
		   map, construct the Buffer and assign an ordinal id for it.  Any
		   optimization here will be very welcome. */
		Buffer &getThreadBuffer(size_t tid);

	}; // BufferSet


	/** Class for thread local singleton.

		This class will be allocated in a copy/thead in order to perform the
		events emission completely thread free.  The constructor of this object
		takes place the first time an event is emitted from some thread; so it
		is not very accurate to use it to measure total thread
		duration. However, it is the best we have until we can enforce some
		thread hooks. */
	class InfoThread {
		const uint64_t _tid;

	public:
		Buffer &eventsBuffer;

		const uint32_t _id;

		/** Thread local Info initialization.

			The class is actually constructed the first time the thread local
			variables is accesses. But the buffer is not destroyed on thread
			finalization because the same threadID may be reused in the future.
			This emits an event of type 2 and value tid (which is always bigger
			than zero). */
		InfoThread();

		~InfoThread();

	}; // InfoThread

#ifdef __linux__
	/** Descriptor for a supported perf event: its perf list name, type, and config.
	    Entries must be kept sorted by name so the constructor can use
	    std::lower_bound for a single O(log n) lookup. */
	struct PerfCounterSpec
	{
		std::string_view name;
		uint32_t type;
		uint64_t config;

		constexpr bool operator<(std::string_view other) const { return name < other; }
	};

	/** All supported perf counters (hardware and software), sorted by name.
	    Opened with pid=0, cpu=-1: the kernel attaches each event to the calling
	    thread and transparently saves/restores the counter on context switches
	    and CPU migrations, so deltas always reflect only this thread's activity
	    regardless of which core it runs on. */
	constexpr PerfCounterSpec allCounters[] = {
		{"branch-instructions",     PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS},
		{"branch-misses",           PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
		{"bus-cycles",              PERF_TYPE_HARDWARE, PERF_COUNT_HW_BUS_CYCLES},
		{"cache-misses",            PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
		{"cache-references",        PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES},
		{"context-switches",        PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES},
		{"cpu-clock",               PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_CLOCK},
		{"cpu-cycles",              PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
		{"cpu-migrations",          PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS},
		{"instructions",            PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
		{"major-faults",            PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS_MAJ},
		{"minor-faults",            PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS_MIN},
		{"page-faults",             PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS},
		{"stalled-cycles-backend",  PERF_TYPE_HARDWARE, PERF_COUNT_HW_STALLED_CYCLES_BACKEND},
		{"stalled-cycles-frontend", PERF_TYPE_HARDWARE, PERF_COUNT_HW_STALLED_CYCLES_FRONTEND},
		{"task-clock",              PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK},
	};
#endif // __linux__


	/** Info container with global variables.

		This gives access to the thread and global static variables. And only
		holds one pointer to the BufferSet object to avoid its premature
		deletion. */
	class Global {
		/** Static utility function to build the trace directory */
		static std::string getTraceDirectory(uint64_t systemTimePoint)
		{
			const time_t localTime = static_cast<time_t>(systemTimePoint);

			std::stringstream ss;
			ss << "TRACEDIR_" << std::put_time(std::localtime(&localTime), "%Y-%m-%d_%H_%M_%S") << "_" << getPid();
			return ss.str();
		}

	public:

		static InfoThread &getInfoThread()
		{
			thread_local static InfoThread threadInfo;
			return threadInfo;
		}

		Global()
			: startSystemTimePoint(std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now()).time_since_epoch().count())
			, traceDirectory(getTraceDirectory(startSystemTimePoint))
			, _buffersSet()
			, _namesSet()
			, threadEventID(_namesSet.registerEventName("ThreadRunning"))
			, mutexID(_namesSet.registerEventName("mutex"))
		{
			// Create the directory
			if (!std::filesystem::create_directory(traceDirectory))
				throw profilerError("Cannot create traces directory: " + traceDirectory);

#ifdef __linux__
			// Pre-register all known perf counter names so their IDs are assigned
			// deterministically at startup, regardless of which counters are actually
			// opened at runtime.  Syscall tracepoints are dynamic and registered on
			// first use by PerfCounter.
			for (const PerfCounterSpec &spec : allCounters)
				_namesSet.registerEventName(std::string(spec.name));
#endif

			// getInfoThread() is NOT called here: InfoThread's constructor calls
			// getInfoGlobal(), which would re-enter this constructor and trigger
			// a recursive_init_error. InfoThread initializes lazily on the first
			// instrumented call, at which point Global is already fully constructed.
		}

		~Global()
		{
			killPool(); // kills the thread pool when needed.

			getInfoThread().eventsBuffer.emplaceEvent(threadEventID, 0);

			_buffersSet.createROW(traceDirectory);

			_namesSet.createPCF(traceDirectory);

			std::cout << "# Profiler TraceDir: " << traceDirectory << std::endl;
		}

		static Global & getInfoGlobal()
		{
			static Global instance;
			return instance;
		}

		const uint64_t startSystemTimePoint;
		const std::string traceDirectory;

		BufferSet _buffersSet;    // Buffers register
		NameSet _namesSet;        // Events names register

		const EventId threadEventID;
		const EventId mutexID;

	};

	/** Reference to the global profiler instance.
	    Declaring it here (after Global is fully defined) ensures every
	    translation unit that includes this header initializes the profiler
	    before main() starts, while avoiding the static-initialization-order
	    fiasco. */
	inline Global & infoGlobal = Global::getInfoGlobal();

	/** Public function to create new events.

	   This registers a new pair eventName -> value wrapping Object oriented
	   calls. */
	inline EventId registerName(
		const std::string &name,
		const std::string &fileName, size_t line,
		EventId event, EventId value
	) {
		if (value == 0)
			return infoGlobal._namesSet.registerEventName(name, fileName, line, event);
		else
			return infoGlobal._namesSet.registerValueName(name, fileName, line, event, value);
	}


	/** Guard class (more info in the constructor docstring).

		This is a tricky variable to rely event pairs emission (start-end) with
		RAII. This simplifies instrumentation on the user side and may rely on
		the instrumentation macro.  The constructor emits an event that will be
		paired with the equivalent one emitted in the destructor. */
	class ProfilerGuard {
		const EventId _id;  //< Event id for this guard. remembered to emit on the destructor

	public:
		// Profile guard should be unique and not transferable.
		ProfilerGuard(ProfilerGuard &&) = delete;
		ProfilerGuard(const ProfilerGuard &) = delete;
		ProfilerGuard& operator=(const ProfilerGuard &) = delete;

		//! Guard constructor
		ProfilerGuard(EventId id, EventId value)
			: _id(id)
		{
			assert(value != 0);
			Global::getInfoThread().eventsBuffer.emplaceEvent(_id, value);
		}

		//! Guard destructor
		~ProfilerGuard()
		{
			Global::getInfoThread().eventsBuffer.emplaceEvent(_id, 0);
		}

	}; // ProfilerGuard

#define INSTRUMENT_EVENT(ID, VALUE) \
	profiler::Global::getInfoThread().eventsBuffer.emplaceEvent(ID, VALUE);


	/** Intrumented mutex

		This is an intrumented mutex wrapper intended to register information
		about lock contentions.
		*/
	class mutex {
	public:
		mutex() noexcept
		{
			unsigned char expected = 0;
			// Globals: 0 = no initialized, 1 = initialization in progress, 2 = already initialized
			if (registered.compare_exchange_strong(expected, 1)) {

				[[maybe_unused]] EventId wait_value
					= registerName("Waiting", "", 0, infoGlobal.mutexID, waiting_value);
				assert(waiting_value == wait_value);

				registered.store(2); // Perform this always at the end if the initialization.
			}

			// block until the init members are set. This is very unlikely needed, but may happen.
			while (registered.load() != 2);

			_id = _counter.fetch_add(1, std::memory_order_relaxed);
			registerName("mutex_" + std::to_string(_id), "", 0, infoGlobal.mutexID, _id);
		}

		mutex(const mutex &) = delete;

		void lock()
		{
			INSTRUMENT_EVENT(infoGlobal.mutexID, waiting_value)
			_lock.lock();
			INSTRUMENT_EVENT(infoGlobal.mutexID, _id)
		}

		void unlock()
		{
			_lock.unlock();
			INSTRUMENT_EVENT(infoGlobal.mutexID, 0)
		}

		bool try_lock()
		{
			const bool locked = _lock.try_lock();
			if (locked) {
				INSTRUMENT_EVENT(infoGlobal.mutexID, _id)
			}
			return locked;
		}

	private:
		static inline std::atomic<unsigned char> registered = 0;
		static constexpr EventId waiting_value = 1;                          /// Reserve the event 1 to blocked by a mutex
		static inline std::atomic<EventId> _counter = waiting_value + 1;     /// The lock counter starts after it

		EventId _id;
		std::mutex _lock;
	};

#ifdef __linux__
	/** Wrapper around a Linux perf event group.

	    Opens N counters as a kernel group: the first as leader
	    (read_format=PERF_FORMAT_GROUP), the rest as members.  A single
	    ::read(leader_fd) returns all N values atomically.

	    The group is reset to zero at construction via
	    PERF_EVENT_IOC_RESET, so readAll() returns absolute values since
	    that reset — no baseline tracking is needed.

	    Opened with pid=0, cpu=-1: each event follows the calling thread
	    across CPU migrations.

	    If any perf_event_open call fails the whole group is marked invalid
	    (all FDs closed) and a diagnostic is printed to stderr.  No
	    fallback to standalone counters.

	    At most maxGroupSize counters per instance; enforced at the call
	    site by a static_assert in INSTRUMENT_PERF. */
	class PerfCounter
	{
	public:
		static constexpr size_t maxGroupSize = 4;

	private:
		int _fd = -1;

		std::array<int, maxGroupSize>              _memberFds {};
		std::array<EventId, maxGroupSize>          _ids {};
		size_t                                     _n = 0;
		mutable std::array<uint64_t, 1 + maxGroupSize> _readBuf {};

		/** Low-level wrapper around perf_event_open.
		    groupFd == -1: open as group leader and set PERF_FORMAT_GROUP.
		    groupFd >= 0: open as group member. */
		static int openPerfEvent(uint32_t type, uint64_t config, bool excludeKernel, int groupFd) noexcept
		{
			perf_event_attr attr{
				.type           = type,
				.size           = sizeof(perf_event_attr),
				.config         = config,
				.read_format    = (groupFd < 0) ? static_cast<uint64_t>(PERF_FORMAT_GROUP) : 0ULL,
				.exclude_kernel = excludeKernel ? 1u : 0u,
				.exclude_hv     = excludeKernel ? 1u : 0u,
			};
			return static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, groupFd, 0));
		}

		/** Open a named hardware or software counter from the static table. */
		static int openNamedCounter(std::string_view name, int groupFd)
		{
			const auto it = std::lower_bound(std::begin(allCounters), std::end(allCounters), name);
			if (it == std::end(allCounters) || it->name != name)
				throw profilerError("INSTRUMENT_PERF: unsupported counter '" + std::string(name) + "'");
			return openPerfEvent(it->type, it->config, true, groupFd);
		}

		/** Returns a hint string when perf_event_paranoid blocks access.
		    paranoid > 2 blocks all perf_event_open; paranoid > 0 blocks
		    syscall tracepoints.  CAP_PERFMON overrides all levels. */
		static std::string perfParanoidHint()
		{
			if (int paranoid = 0; std::ifstream("/proc/sys/kernel/perf_event_paranoid") >> paranoid && paranoid > 0)
				return " (perf_event_paranoid=" + std::to_string(paranoid)
				       + "; hardware/software counters require paranoid <= 2,"
				       " syscall tracepoints require paranoid <= 0 (or CAP_PERFMON))";
			return {};
		}

		/** Returns a hint string for common group-open errno values. */
		static std::string groupOpenHint(int err)
		{
			if (err == EINVAL)
				return " (counters may be incompatible types;"
				       " hardware and software counters cannot be grouped on most kernels)";
			if (err == EACCES)
				return perfParanoidHint();
			return {};
		}

		/** Search the kernel tracing filesystem for the tracepoint ID of a syscall.

		    Tracepoint IDs are assigned dynamically by the kernel at boot so they
		    cannot live in a static table; they must be read from tracefs at runtime.

		    Returns the ID (always > 0) if found and readable.
		    Returns 0 if the tracepoint exists but the id file is not readable
		    (e.g. insufficient permissions).
		    Throws profilerError if no tracing filesystem exposes this syscall name. */
		static uint64_t findSyscallTracepointId(std::string_view syscallName)
		{
			const std::string entryName = "sys_enter_" + std::string(syscallName);

			for (const char *base : {"/sys/kernel/tracing", "/sys/kernel/debug/tracing"}) {
				const std::string syscallsDir = std::string(base) + "/events/syscalls";

				// Use error_code to avoid exceptions when the tracing filesystem
				// root is inaccessible (e.g. debugfs not mounted).
				std::error_code ec;
				const std::filesystem::directory_iterator it(syscallsDir, ec);
				if (ec)
					continue;  // directory not accessible; try next base path

				if (!std::any_of(it, std::filesystem::directory_iterator{},
					[&entryName](const auto &entry) {
						return entry.path().filename() == entryName;
					}))
					continue;

				// Tracepoint exists; try to read its id file.
				// This may still fail with EACCES on locked-down systems.
				if (uint64_t traceId = 0; std::ifstream(syscallsDir + "/" + entryName + "/id") >> traceId)
					return traceId;
				return 0;  // exists but cannot read ID
			}
			throw profilerError("INSTRUMENT_PERF: unknown syscall 'syscall:" + std::string(syscallName) + "'");
		}

		/** Open a syscall tracepoint counter by syscall name (e.g. "read", "write").

		    Unlike hardware/software counters, tracepoints fire in kernel space so
		    exclude_kernel must be 0. The pid=0/cpu=-1 binding keeps the counter
		    thread-local: only syscalls from the calling thread are counted.

		    Returns -1 (with errno set) if the tracepoint exists but cannot be opened
		    due to permissions. Throws profilerError if the syscall name is unknown. */
		static int openSyscallTracepoint(std::string_view syscallName, int groupFd)
		{
			if (const uint64_t traceId = findSyscallTracepointId(syscallName); traceId != 0)
				return openPerfEvent(PERF_TYPE_TRACEPOINT, traceId, false, groupFd);
			return -1;  // tracepoint exists but ID is unreadable; caller will warn
		}

		/** Open one counter (named or syscall tracepoint) into the given group. */
		int openOne(std::string_view name, int groupFd)
		{
			static constexpr std::string_view syscallPrefix = "syscall:";
			if (name.substr(0, syscallPrefix.size()) == syscallPrefix)
				return openSyscallTracepoint(name.substr(syscallPrefix.size()), groupFd);
			return openNamedCounter(name, groupFd);
		}

		/** Close all open FDs and reset _fd to -1. */
		void closeAll() noexcept
		{
			if (_fd >= 0) { ::close(_fd); _fd = -1; }
			for (size_t i = 0; i < _n - 1; ++i)
				if (_memberFds[i] >= 0) { ::close(_memberFds[i]); _memberFds[i] = -1; }
		}

	public:
		explicit PerfCounter(std::initializer_list<const char *> names)
		{
			_n = names.size();
			_memberFds.fill(-1);

			// Register event IDs by name.  registerEventName is idempotent by
			// name, so all threads that hit the same call site get the same IDs.
			{
				size_t i = 0;
				for (const char *name : names)
					_ids[i++] = registerName(std::string(name), "", 0, 0, 0);
			}

			// Build group description once for use in error messages.
			std::string groupDesc = "[";
			for (const char *name : names)
			{
				if (groupDesc.size() > 1)
					groupDesc += ", ";
				groupDesc = groupDesc + "'" + name + "'";
			}
			groupDesc += ']';

			size_t memberIdx = 0;
			for (const char *name : names)
			{
				const int fd = openOne(name, _fd);
				if (_fd < 0 && fd >= 0)
					_fd = fd;
				else if (_fd >= 0 && fd >= 0)
					_memberFds[memberIdx++] = fd;
				else
				{
					const int err = errno;
					closeAll();
					throw profilerError(
						"INSTRUMENT_PERF: failed to open counter '" + std::string(name)
						+ "' in group " + groupDesc + ": "
						+ strerror(err) + groupOpenHint(err));
				}
			}

			ioctl(_fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
		}

		~PerfCounter() noexcept { closeAll(); }

		PerfCounter(const PerfCounter &) = delete;
		PerfCounter & operator=(const PerfCounter &) = delete;

		bool valid() const noexcept
		{
			return _fd >= 0;
		}

		/** Read all N counters in one syscall; returns absolute values since reset.
		    Only valid entries [0.._n) are meaningful. */
		std::array<EventValue, maxGroupSize> readAll() const noexcept
		{
			std::array<EventValue, maxGroupSize> values {};
			const size_t expected = (1 + _n) * sizeof(uint64_t);
			if (::read(_fd, _readBuf.data(), expected) != static_cast<ssize_t>(expected))
				return values;

			// _readBuf[0] == nr; counter values at [1.._n]
			for (size_t i = 0; i < _n; ++i)
			{
				const uint64_t raw = _readBuf[i + 1];
				values[i] = (raw > std::numeric_limits<EventValue>::max())
					? std::numeric_limits<EventValue>::max()
					: static_cast<EventValue>(raw);
			}
			return values;
		}

		void emitAll(Buffer &buf)
		{
			const std::array<EventValue, maxGroupSize> values = readAll();
			std::array<std::pair<EventId, EventValue>, maxGroupSize> events;
			for (size_t i = 0; i < _n; ++i)
				events[i] = {_ids[i], values[i]};
			buf.emplaceMultipleEvents(events.data(), _n);
		}
	};
#endif // __linux__

	// ==================================================
	// Outline function definitions (depend on Global).
	// ==================================================

	// =================== BufferSet ===========================================

	inline Buffer &BufferSet::getThreadBuffer(size_t tid)
	{
		// We attempt to take the read lock first. If this tid was
		// already used, the buffer must be already created, and we
		// don't need the exclusive access.
		{
			std::shared_lock sharedlock(_mapMutex);
			if (auto it = _eventsMap.lower_bound(tid); it != _eventsMap.end() && it->first == tid)
				return it->second;
		}

		// === else === create new entry: <tid, id>
		// Else, this is the first time we use this tid, so, we need
		// exclusive access to modify the map. So, let's release the
		// read lock and try to take the write (unique) lock.
		std::unique_lock uniquelock(_mapMutex); // Now lock exclusively

		const std::string filename
			= infoGlobal.traceDirectory + "/Trace_" + std::to_string(_tcounter) + ".bin";

		auto [it, inserted] = _eventsMap.try_emplace(
			tid, _tcounter, tid, filename, infoGlobal.startSystemTimePoint
		);

		if (inserted)
			++_tcounter;

		return it->second;
	}

	// =================== InfoThread ==========================================
	inline InfoThread::InfoThread()
		: _tid(std::hash<std::thread::id>()(std::this_thread::get_id()))
		, eventsBuffer(infoGlobal._buffersSet.getThreadBuffer(_tid))
		, _id(eventsBuffer._header._id)
	{
		assert(_tid == eventsBuffer._header._tid);
		if (_id > 1)
			eventsBuffer.emplaceEvent(infoGlobal.threadEventID, 1);
	}

	inline InfoThread::~InfoThread()
	{
		if (_id > 1)
			eventsBuffer.emplaceEvent(infoGlobal.threadEventID, 0);
	}


} // profiler


/** \defgroup public interface
	\brief This is the simpler linker list and its functions

	Here starts what is intended to be the public interface:
	A set of macros that generate instrumentation when PROFILER_ENABLED > 0
	otherwise they expand to nothing.
   @{
*/

#define TOKEN_PASTE(x, y) x##y
#define CAT(X,Y) TOKEN_PASTE(X,Y)

/** Instrument a named scope.

   TAG is a plain identifier used as the event name and to connect
   INSTRUMENT_SCOPE_UPDATE calls to this scope. VALUE is the numeric value
   emitted on entry (must be != 0). The end event (value = 0) is emitted
   automatically when the scope exits. */
#define INSTRUMENT_SCOPE(TAG, VALUE)                                                \
	static const profiler::EventId CAT(__profiler_scope_id_,TAG) =                 \
		profiler::registerName(#TAG, __FILE__, __LINE__, 0, 0);                     \
	profiler::ProfilerGuard CAT(__profiler_guard_,TAG)(                           \
		CAT(__profiler_scope_id_,TAG), VALUE);

/** Emit a new value on the event opened by INSTRUMENT_SCOPE(TAG, ...).

   Must be called in the same scope or a nested scope after INSTRUMENT_SCOPE(TAG).
   VALUE is the numeric value to emit (must be != 0).
   An optional string argument sets the value name; otherwise __FILE__:__LINE__
   is used. */
#define INSTRUMENT_SCOPE_UPDATE(TAG, VALUE, ...)                                    \
	static const profiler::EventId CAT(__profiler_scope_update_,__LINE__) =        \
		profiler::registerName(std::string(__VA_ARGS__), __FILE__, __LINE__,        \
			CAT(__profiler_scope_id_,TAG), VALUE);                                  \
	profiler::Global::getInfoThread().eventsBuffer.emplaceEvent(                  \
		CAT(__profiler_scope_id_,TAG), CAT(__profiler_scope_update_,__LINE__));

/** Main macro to instrument functions.

   This macro creates a new event value = 1 for the __profiler_function_id event.
   The event start is emitted when the macro is called and extend until the
   calling scope finalizes.
   This is intended to be called immediately after a function starts. */
#define INSTRUMENT_FUNCTION(...)										\
	static const std::string __profiler_function_name =                       \
		std::string_view(__VA_ARGS__).empty() ? FUNC_NAME : std::string(__VA_ARGS__); \
	static const profiler::EventId __profiler_function_id =						\
		profiler::registerName(__profiler_function_name, __FILE__, __LINE__, 0, 0);  \
    static profiler::EventId CAT(__profiler_function_,__LINE__) =			\
        profiler::registerName(__func__, __FILE__, __LINE__, __profiler_function_id, 1); \
	profiler::ProfilerGuard __guard(__profiler_function_id, CAT(__profiler_function_,__LINE__));


/** Emit a new value on the event opened by INSTRUMENT_FUNCTION.

	Must be called in a scope already instrumented by INSTRUMENT_FUNCTION —
	it references __profiler_function_id which that macro declares. A missing
	INSTRUMENT_FUNCTION will produce a compiler error:
	  "undeclared identifier '__profiler_function_id'"
	An optional string argument sets the value name; otherwise __func__:__LINE__
	is used.
	@param VALUE the numeric value for the event (must be != 0 and != 1). */
#define INSTRUMENT_FUNCTION_UPDATE(VALUE, ...)							\
	static const profiler::EventId CAT(__profiler_function_,__LINE__) =			\
		profiler::registerName(std::string(__VA_ARGS__), __FILE__, __LINE__, __profiler_function_id, VALUE); \
	profiler::Global::getInfoThread().eventsBuffer.emplaceEvent(      \
		__profiler_function_id, CAT(__profiler_function_,__LINE__)		\
		);

/** Emit one or more hardware/software performance counter values.

    Accepts 1–4 counter name strings (matching names shown in `perf list`
    or "syscall:<name>" for syscall tracepoints).  All counters at a given
    call site are opened as a single kernel perf event group so that one
    ::read() syscall returns all values atomically.  Each counter produces
    its own EventEntry sharing the same _time and _core.

    A thread_local PerfCounter group is opened once per (call site, thread).
    The group is reset to zero at construction; subsequent calls emit
    absolute values since that reset.

    If the group cannot be opened (e.g. incompatible counter types) the
    whole group is marked invalid, a diagnostic is printed to stderr, and
    no events are emitted.

    Only available on Linux. On other platforms this macro expands to nothing. */
#ifdef __linux__
#define PROFILER_NARGS(...) \
	PROFILER_NARGS_I(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define PROFILER_NARGS_I(_1,_2,_3,_4,_5,_6,_7,_8,N,...) N
#define INSTRUMENT_PERF(...)                                                     \
	{                                                                            \
		static_assert(PROFILER_NARGS(__VA_ARGS__) <= 4,                          \
			"INSTRUMENT_PERF: at most 4 counters per call");                     \
		thread_local static profiler::PerfCounter                                \
			CAT(__profiler_perf_ctr_, __LINE__)({__VA_ARGS__});                  \
		if (CAT(__profiler_perf_ctr_, __LINE__).valid())                         \
			CAT(__profiler_perf_ctr_, __LINE__).emitAll(                         \
				profiler::Global::getInfoThread().eventsBuffer);                  \
	}
#else
#define INSTRUMENT_PERF(...)
#endif

//!@}

#define NDEBUG SNDEBUG

#else // PROFILER_ENABLED

#define INSTRUMENT_EVENT(...)
#define INSTRUMENT_SCOPE(...)
#define INSTRUMENT_SCOPE_UPDATE(...)
#define INSTRUMENT_FUNCTION(...)
#define INSTRUMENT_FUNCTION_UPDATE(...)
#define INSTRUMENT_PERF(...)

namespace profiler {
    using mutex = std::mutex;
}

#endif // PROFILER_ENABLED
