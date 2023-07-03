Readme
=======

This is a very simple multi-platform Instrument based profiler
intended to work in multi-threaded systems (not intended for
distributed memory systems). For more advanced use cases consider
using [Extrae](https://tools.bsc.es/extrae) instead of this.


The main goal is to make it portable into MSWindows and GNU/Linux
systems with a very minimal implementation and support for
multi-threading. Basically because that's what I need ATM.

Everything in a single header to include without requiring extra
shared/dynamic library.

The basic steps are:
--------------------

1. Instrument the code like in the `main.cpp` example code.
2. Execute your program. This will create a directory called `TRACEDIR_timestamp`
3. Process the Traces with the python parser script.

	```
	Parser.py TRACEDIR_timestamp
	```

	This will create a [Paraver](https://tools.bsc.es/paraver) trace.

