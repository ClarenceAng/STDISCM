CXX ?= clang++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -pedantic -pthread

lfg: main.cpp config.h
	$(CXX) $(CXXFLAGS) main.cpp -o lfg

clean:
	rm -f lfg

.PHONY: clean
