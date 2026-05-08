# chrono rift

chrono rift is a multi-process linux game prototype built with posix shared memory, raw pthreads, semaphores, and ncurses.

three separate processes run together:

- `arbiter`: owns and updates global game state
- `hip`: human interface process and terminal ui
- `asp`: automated strategic process for enemy bot actions

## requirements

- linux
- `g++`
- `make`
- ncurses development package

on arch linux, ncurses is usually already available. if needed:

```bash
sudo pacman -S ncurses
```

## build

from project root:

```bash
make clean
make
```

this builds:

- `arbiter/arbiter`
- `hip/hip`
- `asp/asp`

## run order

use three terminals from project root.

terminal 1:

```bash
./arbiter/arbiter
```

terminal 2:

```bash
./asp/asp
```

terminal 3:

```bash
./hip/hip
```

if `hip` says shared memory not found, start `arbiter` first.

## hip controls

- `q` quit hip
- `1` strike
- `2` exhaust
- `3` heal
- `4` skip
- `5` pick up drop
- `6` ultimate (requires both artifacts in inventory)
- `7` stun attack

## how the game state works

- shared memory name: `/chrono_rift_game_state`
- main struct: `game_state` in `gamestate.h`
- synchronization:
  - `state_lock` semaphore for shared state updates
  - `resource_lock` mutex for artifact-level lock/deadlock checks

## feature highlights

- seeded stat initialization in arbiter
- asynchronous enemy bot threads in asp
- signal-driven ultimate flow (`sigstop`, `sigusr1`, `sigalrm`, `sigcont`)
- stun timers for players and enemies
- inventory allocator with long-term storage swapping
- ncurses windows for status, squad, enemies, inventory, and action log

## quick troubleshooting

- inventory not visible:
  - enlarge terminal size
  - keep `hip` running after `arbiter` and `asp` are up
- bots not moving:
  - check if ultimate freeze is active
  - verify `asp` process is running
- full rebuild:

```bash
make clean
make
```
