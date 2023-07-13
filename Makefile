
MAIN:=main.x

all: $(MAIN)

CXXFLAGS += -DPROFILER_ENABLED

%.x: %.o Profiler.hpp
	$(CXX) $< -o $@

.PHONY: clean cleanall test

clean:
	rm -f *.x

cleanall:
	rm -rf *.x *.bin *.txt TRACEDIR*

test: $(MAIN)
	./$(MAIN)
