CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -pthread

LIBS = -lncurses -lrt

TARGETS = arbiter hip asp

all: clean $(TARGETS)
	@echo Build complete.

arbiter: arbiter/arbiter.cpp
	$(CXX) $(CXXFLAGS) arbiter/arbiter.cpp -o arbiter/arbiter $(LIBS)

hip: hip/hip_main.cpp hip/hip_logic.cpp hip/hip_ui.cpp
	$(CXX) $(CXXFLAGS) hip/hip_main.cpp -o hip/hip $(LIBS)

asp: asp/asp.cpp
	$(CXX) $(CXXFLAGS) asp/asp.cpp -o asp/asp $(LIBS)

clean:
	rm -f judge hips enemy player arbiter_bin hip_bin asp_bin arbiter_exec hip_exec asp_exec arbiter/arbiter hip/hip asp/asp

.PHONY: all clean
