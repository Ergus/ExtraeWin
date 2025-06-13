#pragma once

#include <mutex>

#ifdef PROFILER_ENABLED

#include <atomic>
#include <Profiler.hpp>
#include <cassert>

namespace profiler {

	class mutex {
	public:
		constexpr mutex() noexcept
		{
			unsigned char expected = 0;
			// Globals: 0 = no initialized, 1 = initialization in progress, 2 = already initialized
			if (registered.compare_exchange_strong(expected, 1)) {
				bool tmp = profiler::Global<>::traceMemory;
				profiler::Global<>::traceMemory = false;

				instrument_mutex_id = profiler::registerName("Mutex", "", 0, 0, 0);
				[[maybe_unused]] uint16_t wait_value = profiler::registerName("Waiting", "", 0, instrument_mutex_id, instrument_waiting);
				assert(instrument_waiting == wait_value);

				profiler::Global<>::traceMemory = tmp;
				registered.store(2); // Perform this always at the end if the initialization.
			}

			// block until the init members are set. This is very unlikely needed, but may happen.
			while (registered.load() != 2);

			_id = _counter.fetch_add(1, std::memory_order_relaxed);
		}

		mutex(const mutex &) = delete;

		void lock()
		{
			INSTRUMENT_EVENT(instrument_mutex_id, instrument_waiting)
			_lock.lock();
			INSTRUMENT_EVENT(instrument_mutex_id, _id)
		}

		void unlock()
		{
			_lock.unlock();
			INSTRUMENT_EVENT(instrument_mutex_id, 0)
		}

		bool try_lock()
		{
			const bool locked = _lock.try_lock();
			if (locked)
				INSTRUMENT_EVENT(instrument_mutex_id, _id)
			return locked;
		}

	private:
		static inline std::atomic<unsigned char> registered = 0;
		static inline uint16_t instrument_mutex_id;
		static constexpr uint16_t instrument_waiting = 1;	  /// Reserve the event 1 to blocked by a mutex
		static inline std::atomic<unsigned int> _counter = instrument_waiting + 1; /// The lock counter starts after it


		unsigned int _id;
		std::mutex _lock;
	};
}

#define INSTRUMENTED_MUTEX() profiler::mutex

#else // PROFILER_ENABLED

namespace profiler {
    using mutex = std::mutex;
}

#define INSTRUMENTED_MUTEX() std::mutex

#endif //  PROFILER_ENABLED

