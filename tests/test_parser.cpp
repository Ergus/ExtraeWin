/*
 * Copyright (C) 2024  Jimmy Aguilar Mena
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstdint>
#include <string>
#include <fstream>
#include <filesystem>
#include <functional>
#include <map>
#include <vector>
#include <iostream>
#include <cassert>
#include <cstdlib>
#include <cstring>

// ============================================================
// Binary format — must match the structs in Parser.cpp exactly
// ============================================================

struct TraceHeader {
	uint32_t _id;
	uint32_t _nentries;
	uint64_t _tid;
	uint64_t _startGTime;
};

struct EventEntry {
	uint64_t _time;
	uint16_t _id;
	uint16_t _core;
	uint32_t _value;
};

// ============================================================
// Helpers
// ============================================================

static void writeTraceFile(
	const std::filesystem::path &path,
	uint32_t id,
	uint64_t tid,
	uint64_t startGTime,
	const std::vector<EventEntry> &events)
{
	std::ofstream f(path, std::ios::binary);
	if (!f)
		throw std::runtime_error("Cannot open for writing: " + path.string());

	TraceHeader hdr{id, static_cast<uint32_t>(events.size()), tid, startGTime};
	f.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));

	if (!events.empty())
		f.write(reinterpret_cast<const char *>(events.data()),
		        static_cast<std::streamsize>(events.size() * sizeof(EventEntry)));
}

static int runParser(const std::string &parserPath, const std::filesystem::path &dir)
{
#ifdef _WIN32
	const std::string cmd = parserPath + " " + dir.string() + " > NUL 2>&1";
#else
	const std::string cmd = parserPath + " " + dir.string() + " > /dev/null 2>&1";
#endif
	return std::system(cmd.c_str());
}

static std::vector<std::string> readPRV(const std::filesystem::path &dir)
{
	std::ifstream f(dir / "Trace.prv");
	if (!f)
		throw std::runtime_error("Trace.prv not found in " + dir.string());

	std::vector<std::string> lines;
	std::string line;
	while (std::getline(f, line)) {
		if (!line.empty())
			lines.push_back(line);
	}
	return lines;
}

/** RAII wrapper for a temporary directory. */
struct TempDir {
	std::filesystem::path path;

	TempDir()
	{
		static int counter = 0;
		path = std::filesystem::temp_directory_path()
		     / ("test_parser_" + std::to_string(++counter));
		std::filesystem::create_directory(path);
	}

	~TempDir() noexcept
	{
		std::filesystem::remove_all(path);
	}
};

// ============================================================
// Auto-registration infrastructure
// ============================================================

static std::string gParserPath;

inline std::map<std::string, std::function<void()>> &getTestRegistry()
{
	static std::map<std::string, std::function<void()>> registry;
	return registry;
}

struct TestRegistrar {
	TestRegistrar(const std::string &name, std::function<void()> fn) {
		getTestRegistry().emplace(name, fn);
	}
};

#define DEFINE_TEST(name)                                   \
	static void name();                                     \
	static TestRegistrar registrar_##name(#name, name);    \
	static void name()

// ============================================================
// Tests
// ============================================================

/** Single-thread trace: verify event count, header duration, and exact PRV lines. */
DEFINE_TEST(test_single_thread)
{
	TempDir tmp;

	const std::vector<EventEntry> events = {
		{100, 1, 0, 1},
		{200, 1, 0, 2},
		{300, 1, 0, 0},
	};
	writeTraceFile(tmp.path / "thread_1.bin", 1, 1001, 1000000000ULL, events);

	assert(runParser(gParserPath, tmp.path) == 0);

	const std::vector<std::string> lines = readPRV(tmp.path);

	// 1 header line + 3 event lines
	assert(lines.size() == 4);
	assert(lines[0].substr(0, 8) == "#Paraver");

	// Duration = lastTime - firstTime = 300 - 100 = 200
	assert(lines[0].find("200_ns") != std::string::npos);

	// 1 file → (1:1) at end of header
	assert(lines[0].find("(1:1)") != std::string::npos);

	// PRV format: 2:<core>:1:1:<thread>:<time>:<event_id>:<value>
	assert(lines[1] == "2:0:1:1:1:100:1:1");
	assert(lines[2] == "2:0:1:1:1:200:1:2");
	assert(lines[3] == "2:0:1:1:1:300:1:0");
}

/** Two-thread trace: events must emerge from the heap in strict time order. */
DEFINE_TEST(test_two_thread_merge_order)
{
	TempDir tmp;

	// Thread 1: events at times 100, 300, 500
	const std::vector<EventEntry> ev1 = {
		{100, 1, 0, 1},
		{300, 1, 0, 2},
		{500, 1, 0, 0},
	};
	// Thread 2: events at times 200, 400, 600
	const std::vector<EventEntry> ev2 = {
		{200, 2, 1, 10},
		{400, 2, 1, 20},
		{600, 2, 1,  0},
	};

	writeTraceFile(tmp.path / "thread_1.bin", 1, 1001, 1000000000ULL, ev1);
	writeTraceFile(tmp.path / "thread_2.bin", 2, 1002, 1000000000ULL, ev2);

	assert(runParser(gParserPath, tmp.path) == 0);

	const std::vector<std::string> lines = readPRV(tmp.path);

	// 1 header + 6 event lines
	assert(lines.size() == 7);
	assert(lines[0].substr(0, 8) == "#Paraver");

	// Duration = 600 - 100 = 500
	assert(lines[0].find("500_ns") != std::string::npos);

	// Events must appear strictly ordered by time
	assert(lines[1].find(":100:") != std::string::npos);
	assert(lines[2].find(":200:") != std::string::npos);
	assert(lines[3].find(":300:") != std::string::npos);
	assert(lines[4].find(":400:") != std::string::npos);
	assert(lines[5].find(":500:") != std::string::npos);
	assert(lines[6].find(":600:") != std::string::npos);

	// Spot-check thread attribution on two events
	assert(lines[1] == "2:0:1:1:1:100:1:1");
	assert(lines[2] == "2:1:1:1:2:200:2:10");
}

/** Max core value must appear correctly in the Paraver header. */
DEFINE_TEST(test_max_core_in_header)
{
	TempDir tmp;

	// Events on cores 0, 3, 7 — header must report 7
	const std::vector<EventEntry> events = {
		{100, 1, 0, 1},
		{200, 1, 3, 2},
		{300, 1, 7, 0},
	};
	writeTraceFile(tmp.path / "thread_1.bin", 1, 1001, 1000000000ULL, events);

	assert(runParser(gParserPath, tmp.path) == 0);

	const std::vector<std::string> lines = readPRV(tmp.path);
	assert(!lines.empty());

	// Paraver header: ...<duration>_ns:1(<maxCore>):...
	assert(lines[0].find(":1(7):") != std::string::npos);
}

/** Thread-count field in the Paraver header must equal the number of .bin files. */
DEFINE_TEST(test_thread_count_in_header)
{
	TempDir tmp;

	const std::vector<EventEntry> events = {{100, 1, 0, 1}, {200, 1, 0, 0}};

	writeTraceFile(tmp.path / "t1.bin", 1, 1001, 1000000000ULL, events);
	writeTraceFile(tmp.path / "t2.bin", 2, 1002, 1000000000ULL, events);
	writeTraceFile(tmp.path / "t3.bin", 3, 1003, 1000000000ULL, events);

	assert(runParser(gParserPath, tmp.path) == 0);

	const std::vector<std::string> lines = readPRV(tmp.path);
	assert(!lines.empty());

	// 3 files → (3:1) near the end of the header line
	assert(lines[0].find("(3:1)") != std::string::npos);
}

/** Non-monotonic timestamps in a trace file must cause the parser to fail. */
DEFINE_TEST(test_non_monotonic_rejected)
{
	TempDir tmp;

	// Second event timestamp is smaller than the first — invalid
	const std::vector<EventEntry> events = {
		{300, 1, 0, 1},
		{100, 1, 0, 0},
	};
	writeTraceFile(tmp.path / "thread_1.bin", 1, 1001, 1000000000ULL, events);

	assert(runParser(gParserPath, tmp.path) != 0);
}

// ============================================================
// Entry point
// ============================================================

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::cerr << "Usage: " << argv[0] << " <test_name> <parser_path>\n";
		return 1;
	}

	const std::string testName = argv[1];
	gParserPath = argv[2];

	std::map<std::string, std::function<void()>> &registry = getTestRegistry();
	const std::map<std::string, std::function<void()>>::iterator it = registry.find(testName);
	if (it == registry.end()) {
		std::cerr << "Unknown test: " << testName << "\n";
		return 1;
	}

	try {
		it->second();
		std::cout << "PASS: " << testName << "\n";
		return 0;
	} catch (const std::exception &e) {
		std::cerr << "FAILED: " << e.what() << "\n";
		return 1;
	}
}
