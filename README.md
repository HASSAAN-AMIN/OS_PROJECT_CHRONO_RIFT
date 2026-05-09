# chrono rift

chrono rift is a multi-process linux tactical combat prototype built with posix shared memory, raw pthreads, semaphores, and ncurses.

three separate processes run together:

- `arbiter`: owns and updates global game state, monitors deadlocks, detects win/lose, spawns the eclipse relic
- `hip`: human interfacing process, multi-threaded (one thread per player) with elite ncurses tui
- `asp`: automated strategic process, one thread per enemy npc

## requirements

- linux
- `g++`
- `make`
- ncurses development package

on arch linux:

```bash
sudo pacman -S ncurses
```

## build

from project root:

```bash
make clean
make
```

builds in project root:

- `judge`
- `player`
- `enemy`

## run order

start one terminal per process from project root.

terminal 1 (game arbiter, takes roll number as cmdline arg):

```bash
./judge 240880
```

if no roll number is passed it falls back to `ROLL_NUMBER` env var or the default `240880`.

terminal 2 (asp, no args):

```bash
./enemy
```

terminal 3 (hip, takes party size as cmdline arg or prompts):

```bash
./player 4
```

with no arg, hip prompts for party size on stdin before launching the tui. `PARTY_SIZE` env var is also honored.

if `hip` says shared memory not found, start `arbiter` first.

## controls

- `1` strike (player damage stat)
- `2` exhaust (drain enemy stamina by player damage)
- `3` heal (restore 10% of max hp)
- `4` skip (drop stamina to 50%)
- `5` pickup (take ground drop)
- `6` ultimate (requires solar core + lunar blade in inventory)
- `7` stun attack (damage + freeze enemy 3s)
- `8` use weapon (best weapon damage from inventory)
- `9` swap in (bring weapon back from long term storage)
- `?` toggle help overlay
- `q` quit (sends `SIGTERM` to arbiter and asp)

all actions except pickup require full stamina (per the arrival-time scheduling in section 3 of the spec).

## stat formulas (roll number driven)

| stat | formula |
| --- | --- |
| player hp | roll_number + rand(100..1000) |
| player damage | last digit of roll_number + 10 |
| player speed | 100 / party_size |
| player max stamina | 100 |
| enemy hp | last 2 digits of roll_number + rand(50..200) |
| enemy damage | second last digit of roll_number + 10 |
| enemy speed | rand(10..30) |
| enemy max stamina | 150 |
| enemy count | rand(2..9) |

## weapons

| weapon | slots | damage |
| --- | --- | --- |
| splinter stick | 2 | 12 |
| venom dagger | 4 | 30 |
| obsidian axe | 5 | 45 |
| frostbow | 6 | 48 |
| thunderstaff | 6 | 50 |
| iron halberd | 7 | 55 |
| solar core | 10 | 95 |
| lunar blade | 10 | 90 |
| eclipse relic | 8 | 70 |

multi-slot weapons take contiguous slots in the 20-slot primary inventory. when there is not enough contiguous free space, the iterative allocator swaps existing weapons out to long term storage (also 20 slots) and zeros the freed primary slots before placing the new weapon. `swap in` reverses the trip.

## win / lose / quit

- win: kill 10 enemies (`kills_required_to_win`)
- lose: every member of the party dies
- quit: press `q` in hip, which sends `SIGTERM` to the arbiter and asp

the arbiter watches `outcome` each tick and exits cleanly when the game ends. hip shows a centered overlay (victory, defeat, or shutdown) before terminating.

## eclipse relic

the arbiter introduces the dynamic eclipse relic into the arena roughly 25 seconds after launch (when nothing else is on the ground). it follows the same pickup / locking rules as the solar core and lunar blade and shows up in the artifact tracker.

## state synchronisation

- shared memory name: `/chrono_rift_game_state`
- main struct: `game_state` in `gamestate.h`
- synchronisation primitives:
  - unnamed `sem_t state_lock` for general state mutations
  - process-shared `pthread_mutex_t resource_lock` for the artifact table / deadlock checks
  - additional unnamed semaphores wired up for future use (`memory_sem`, `player_sem`, `enemy_sem`, `inventory_sem`, `relic_sem`)
- the arbiter runs a background thread that watches for circular waits across solar core / lunar blade and forces a drop to break the deadlock

## ultimate / stun signal flow (no flags, no pipes)

- hip releases `state_lock` first, then `kill(asp_pid, SIGSTOP)` to freeze enemy decisions, then `kill(arbiter_pid, SIGUSR1)` (ult) or `SIGUSR2` (stun)
- arbiter sets a `SIGALRM` for 10s (ult) or 3s (stun) and `SIGCONT`s asp / hip when the alarm fires
- this avoids the classic "sigstop while holding the lock" deadlock

## tui highlights

- three responsive panels (player squad, combat arena, enemy forces) with acs box borders
- ascii banner header for "chrono rift"
- per-entity hp / stamina bars with critical blink, dead enemies flip the entire box bold red
- inventory tetris boxes that always render the 20-slot grid; multi-slot weapons span colored runs with semantic colors (solar=yellow, lunar=cyan, eclipse=magenta, etc.)
- combat arena packs an action log, system status (roll number, party size, pids, ultimate / stun / deadlock indicators, kill counter bar) and an artifact tracker that shows holders or "on ground"
- victory / defeat / shutdown overlays
- `?` toggles a help overlay listing every hotkey

## quick troubleshooting

- inventory not visible:
  - enlarge terminal size (>= 70x14)
- bots not moving:
  - check if the `STATE: ULTIMATE FROZEN` indicator is on
  - verify the asp process is running
- no enemies show up:
  - the random count is 2-9; check arbiter stdout for the actual count
  - try a different roll number for variety
- full rebuild:

```bash
make clean
make
```
