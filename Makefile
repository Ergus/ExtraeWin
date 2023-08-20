
MAIN:=main.x Parser.x

all: $(MAIN)

CXXFLAGS += -Wall -DPROFILER_ENABLED=2 -ggdb

main.x: Profiler.hpp

%.x: %.o
	$(CXX) $(CXXFLAGS) -ltbb $< -o $@

.PHONY: clean cleanall test

clean:
	rm -f *.x

cleanall:
	rm -rf *.x *.bin *.txt TRACEDIR*

test: $(MAIN)  Parser.x
	./$(MAIN)
