
MAIN:=main.x Parser.x

all: $(MAIN)

CXXFLAGS += -Wall -DPROFILER_ENABLED -ggdb

main.x: Profiler.hpp

%.x: %.o
	$(CXX) $< -o $@

.PHONY: clean cleanall test

clean:
	rm -f *.x

cleanall:
	rm -rf *.x *.bin *.txt TRACEDIR*

test: $(MAIN)  Parser.x
	./$(MAIN)
