CXX = g++
cflags = -Wall -Wextra -pthread

all: judge player enemy

judge: arbiter/arbiter.cpp
	$(CXX) $(cflags) arbiter/arbiter.cpp -o judge -lrt

player: hip/hip.cpp
	$(CXX) $(cflags) hip/hip.cpp -o player -lncurses -lrt

enemy: asp/asp.cpp
	$(CXX) $(cflags) asp/asp.cpp -o enemy -lrt

clean:
	rm -f judge player enemy arbiter_bin hip_bin asp_bin arbiter_exec hip_exec asp_exec arbiter/arbiter hip/hip asp/asp
