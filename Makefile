CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread
LIBS = -lncurses -lrt
TARGETS = judge player enemy

all: clean $(TARGETS)
	@echo Build complete.

judge:
	$(CXX) $(CXXFLAGS) arbiter/*.cpp -o $@ $(LIBS)

player:
	$(CXX) $(CXXFLAGS) hip/hip_main.cpp -o $@ $(LIBS)

enemy:
	$(CXX) $(CXXFLAGS) asp/*.cpp -o $@ $(LIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
