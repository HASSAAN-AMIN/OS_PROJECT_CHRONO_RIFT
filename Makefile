cc = g++
cflags = -Wall -Wextra -pthread

all: arbiter_exec hip_exec asp_exec

arbiter_exec: arbiter/arbiter.cpp
	$(cc) $(cflags) arbiter/arbiter.cpp -o arbiter_exec -lrt

hip_exec: hip/hip.cpp
	$(cc) $(cflags) hip/hip.cpp -o hip_exec -lncurses -lrt

asp_exec: asp/asp.cpp
	$(cc) $(cflags) asp/asp.cpp -o asp_exec -lrt

clean:
	rm -f arbiter_exec hip_exec asp_exec arbiter/arbiter hip/hip asp/asp
