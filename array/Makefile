CXX = clang++
CXXFLAGS = -std=c++20 -Wall -Wextra -O2
JADE_INC = -I$(HOME)/projects/jade-ide/include

build: main.cpp engine.cpp engine.h exchange.h orderbook.h
	$(CXX) $(CXXFLAGS) $(JADE_INC) -o main main.cpp engine.cpp

run: build
	sudo mperf-stat -e cycles,instructions,branches,branch-misses,l1d-cache-misses,map-stalls -- ./main

clean: 
	rm -f main latency.csv summary.csv
