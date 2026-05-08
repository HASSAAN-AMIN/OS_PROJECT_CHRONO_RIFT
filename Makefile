cc = g++
cflags = -Wall -Wextra -pthread

all: arbiter/arbiter hip/hip asp/asp

arbiter/arbiter: arbiter/arbiter.cpp
	$(cc) $(cflags) arbiter/arbiter.cpp -o arbiter/arbiter -lrt

hip/hip: hip/hip.cpp
	$(cc) $(cflags) hip/hip.cpp -o hip/hip -lncurses -lrt

asp/asp: asp/asp.cpp
	$(cc) $(cflags) asp/asp.cpp -o asp/asp -lrt

clean:
	rm -f arbiter/arbiter hip/hip asp/asp
