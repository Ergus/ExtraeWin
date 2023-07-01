
MAIN:=main.x

all: $(MAIN)

%.x: %.o
	$(CXX) $< -o $@

.PHONY: clean cleanall test

clean:
	rm -f *.x

cleanall:
	rm -f *.x *.bin *.txt

test: $(MAIN)
	./$(MAIN)
