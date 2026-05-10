CXX = g++
CXXFLAGS = -Wall -Wextra -pthread

all: judge hips enemy

judge: arbiter/arbiter.cpp
	$(CXX) $(CXXFLAGS) arbiter/arbiter.cpp -o judge -lrt

hips: hip/hip_main.cpp hip/hip_ui.cpp hip/hip_logic.cpp
	$(CXX) $(CXXFLAGS) hip/hip_main.cpp -o hips -lncurses -lrt

enemy: asp/asp.cpp
	$(CXX) $(CXXFLAGS) asp/asp.cpp -o enemy -lrt

clean:
	rm -f judge hips enemy player arbiter_bin hip_bin asp_bin arbiter_exec hip_exec asp_exec arbiter/arbiter hip/hip asp/asp
