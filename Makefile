CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread
LIBS = -lncurses -lrt
TARGETS = arbiters hips asps

all: clean $(TARGETS)
	@echo Build complete.

arbiters:
	$(CXX) $(CXXFLAGS) arbiter/*.cpp -o $@ $(LIBS)

hips:
	$(CXX) $(CXXFLAGS) hip/hip_main.cpp -o $@ $(LIBS)

asps:
	$(CXX) $(CXXFLAGS) asp/*.cpp -o $@ $(LIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
