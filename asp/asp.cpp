#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../gamestate.h"

using namespace std;

const char *shared_memory_name = "/chrono_rift_game_state";
const int enemy_attack_cost = game_state::enemy_max_stamina;
const int enemy_stun_chance_percent = 15;
const int enemy_skip_chance_percent = 10;
const useconds_t idle_sleep_us = 80000;

int shared_memory_fd = -1;
game_state *shared_state = nullptr;
volatile sig_atomic_t running = 1;
pthread_t npc_threads[game_state::max_enemies];
int npc_ids[game_state::max_enemies];
int started_thread_count = 0;

void print_errno(const char *action) {
    fprintf(stderr, "%s: %s\n", action, strerror(errno));
}

int clamp_value(int value, int low, int high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

bool open_shared_memory() {
    shared_memory_fd = shm_open(shared_memory_name, O_RDWR, 0600);
    if (shared_memory_fd < 0) {
        print_errno("shm_open failed");
        return false;
    }
    printf("shared memory linked\n");
    return true;
}

bool map_shared_memory() {
    void *mapped = mmap(nullptr, sizeof(game_state), PROT_READ | PROT_WRITE, MAP_SHARED, shared_memory_fd, 0);
    if (mapped == MAP_FAILED) {
        print_errno("mmap failed");
        return false;
    }
    shared_state = static_cast<game_state *>(mapped);
    printf("shared memory mapped\n");
    return true;
}

void close_shared_memory_fd() {
    if (shared_memory_fd < 0) {
        return;
    }
    if (close(shared_memory_fd) != 0) {
        print_errno("close failed");
    }
    shared_memory_fd = -1;
}

void unmap_shared_memory() {
    if (shared_state == nullptr) {
        return;
    }
    if (munmap(shared_state, sizeof(game_state)) != 0) {
        print_errno("munmap failed");
    }
    shared_state = nullptr;
}

bool lock_state() {
    while (sem_wait(&shared_state->state_lock) != 0) {
        if (errno == EINTR) {
            if (!running) {
                return false;
            }
            continue;
        }
        print_errno("sem_wait state_lock failed");
        return false;
    }
    return true;
}

bool unlock_state() {
    if (sem_post(&shared_state->state_lock) != 0) {
        print_errno("sem_post state_lock failed");
        return false;
    }
    return true;
}

bool register_asp_pid() {
    if (!lock_state()) {
        return false;
    }
    shared_state->asp_pid = getpid();
    if (!unlock_state()) {
        return false;
    }
    return true;
}

int collect_living_players(int *player_indices, int capacity) {
    int count = 0;
    for (int i = 0; i < game_state::max_players && count < capacity; ++i) {
        if (shared_state->player_hp[i] > 0) {
            player_indices[count++] = i;
        }
    }
    return count;
}

int pick_random_living_player(const int *player_indices, int player_count) {
    if (player_count <= 0) {
        return -1;
    }
    int random_index = rand() % player_count;
    return player_indices[random_index];
}

int find_living_player() {
    int player_indices[game_state::max_players];
    int player_count = collect_living_players(player_indices, game_state::max_players);
    return pick_random_living_player(player_indices, player_count);
}

int enemy_damage_for(int enemy_id) {
    int dmg = shared_state->enemy_damage[enemy_id];
    if (dmg <= 0) {
        return 10;
    }
    return dmg;
}

void write_attack_log(int enemy_id, int player_id, int dmg) {
    snprintf(
        shared_state->action_log,
        sizeof(shared_state->action_log),
        "enemy %d strike: hit player %d for %d damage",
        enemy_id + 1,
        player_id + 1,
        dmg
    );
}

void write_enemy_stun_log(int enemy_id, int player_id, int dmg) {
    snprintf(
        shared_state->action_log,
        sizeof(shared_state->action_log),
        "enemy %d stun strike: hit player %d for %d, stunned 3s",
        enemy_id + 1,
        player_id + 1,
        dmg
    );
}

void write_enemy_skip_log(int enemy_id) {
    snprintf(
        shared_state->action_log,
        sizeof(shared_state->action_log),
        "enemy %d skip: stamina drained to 50%%",
        enemy_id + 1
    );
}

void perform_enemy_attack(int enemy_id, int player_id) {
    int dmg = enemy_damage_for(enemy_id);
    int next_hp = shared_state->player_hp[player_id] - dmg;
    shared_state->player_hp[player_id] = clamp_value(next_hp, 0, 99999);
    shared_state->enemy_stamina[enemy_id] = 0;
    write_attack_log(enemy_id, player_id, dmg);
}

void perform_enemy_stun_attack(int enemy_id, int player_id) {
    int dmg = enemy_damage_for(enemy_id);
    int next_hp = shared_state->player_hp[player_id] - dmg;
    shared_state->player_hp[player_id] = clamp_value(next_hp, 0, 99999);
    shared_state->player_stun_end_time[player_id] = time(nullptr) + 3;
    shared_state->enemy_stamina[enemy_id] = 0;
    write_enemy_stun_log(enemy_id, player_id, dmg);
}

void perform_enemy_skip(int enemy_id) {
    shared_state->enemy_stamina[enemy_id] = game_state::enemy_max_stamina / 2;
    write_enemy_skip_log(enemy_id);
}

bool should_use_stun_attack() {
    return (rand() % 100) < enemy_stun_chance_percent;
}

bool should_skip() {
    return (rand() % 100) < enemy_skip_chance_percent;
}

void perform_enemy_pickup_drop(int enemy_id) {
    int dropped = shared_state->current_dropped_weapon;
    if (dropped == 0) {
        return;
    }
    int holder_id = game_state::max_players + enemy_id;
    shared_state->current_dropped_weapon = 0;
    if (dropped == game_state::solar_core_id) {
        shared_state->solar_core_holder = holder_id;
    } else if (dropped == game_state::lunar_blade_id) {
        shared_state->lunar_blade_holder = holder_id;
    } else if (dropped == game_state::eclipse_relic_id) {
        shared_state->eclipse_relic_holder = holder_id;
        shared_state->eclipse_relic_present = 1;
    }
    snprintf(
        shared_state->action_log,
        sizeof(shared_state->action_log),
        "enemy %d pickup: claimed %d",
        enemy_id + 1,
        dropped
    );
}

void run_enemy_step_locked(int enemy_id) {
    if (enemy_id < 0 || enemy_id >= game_state::max_enemies) {
        return;
    }
    if (shared_state->enemy_hp[enemy_id] <= 0) {
        return;
    }
    if (shared_state->stun_end_time[enemy_id] > time(nullptr)) {
        return;
    }
    if (shared_state->enemy_stamina[enemy_id] < enemy_attack_cost) {
        return;
    }
    if (shared_state->outcome != game_state::outcome_ongoing) {
        return;
    }
    if (shared_state->current_dropped_weapon != 0) {
        perform_enemy_pickup_drop(enemy_id);
    }
    if (should_skip()) {
        perform_enemy_skip(enemy_id);
        return;
    }
    int player_id = find_living_player();
    if (player_id < 0) {
        return;
    }
    if (should_use_stun_attack()) {
        perform_enemy_stun_attack(enemy_id, player_id);
    } else {
        perform_enemy_attack(enemy_id, player_id);
    }
}

void *npc_logic_loop(void *arg) {
    int enemy_id = *static_cast<int *>(arg);
    while (running) {
        if (lock_state()) {
            run_enemy_step_locked(enemy_id);
            unlock_state();
        }
        while (usleep(idle_sleep_us) == -1 && errno == EINTR) {
        }
    }
    return nullptr;
}

void handle_signal(int) {
    running = 0;
}

bool register_signals() {
    if (signal(SIGINT, handle_signal) == SIG_ERR) {
        fprintf(stderr, "failed to register sigint handler\n");
        return false;
    }
    if (signal(SIGTERM, handle_signal) == SIG_ERR) {
        fprintf(stderr, "failed to register sigterm handler\n");
        return false;
    }
    return true;
}

bool start_npc_threads() {
    int enemy_count = shared_state->active_enemy_count;
    if (enemy_count < 2 || enemy_count > game_state::max_enemies) {
        fprintf(stderr, "invalid active_enemy_count: %d\n", enemy_count);
        return false;
    }
    printf("asp: spawning %d enemy threads\n", enemy_count);
    for (int i = 0; i < enemy_count; ++i) {
        npc_ids[i] = i;
        if (pthread_create(&npc_threads[i], nullptr, npc_logic_loop, &npc_ids[i]) != 0) {
            print_errno("pthread_create failed");
            running = 0;
            return false;
        }
        started_thread_count++;
    }
    printf("npc threads started\n");
    return true;
}

void join_npc_threads() {
    for (int i = 0; i < started_thread_count; ++i) {
        pthread_join(npc_threads[i], nullptr);
    }
}

void cleanup() {
    unmap_shared_memory();
    close_shared_memory_fd();
}

int main() {
    srand(static_cast<unsigned int>(time(nullptr)) ^ static_cast<unsigned int>(getpid()));
    if (!register_signals()) {
        return 1;
    }
    if (!open_shared_memory()) {
        return 1;
    }
    if (!map_shared_memory()) {
        close_shared_memory_fd();
        return 1;
    }
    if (!register_asp_pid()) {
        cleanup();
        return 1;
    }
    if (!start_npc_threads()) {
        join_npc_threads();
        cleanup();
        return 1;
    }
    join_npc_threads();
    cleanup();
    printf("asp exited cleanly\n");
    return 0;
}
