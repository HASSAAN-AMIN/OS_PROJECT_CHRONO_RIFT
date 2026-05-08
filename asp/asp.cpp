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

static const char *shared_memory_name = "/chrono_rift_game_state";
static const int enemy_attack_cost = 150;
static const int enemy_attack_damage = 18;
static const useconds_t idle_sleep_us = 20000;

static int shared_memory_fd = -1;
static game_state *shared_state = nullptr;
static volatile sig_atomic_t running = 1;
static pthread_t npc_threads[game_state::max_enemies];
static int npc_ids[game_state::max_enemies];
static int started_thread_count = 0;

static void print_errno(const char *action) {
    std::fprintf(stderr, "%s: %s\n", action, std::strerror(errno));
}

static int clamp_value(int value, int low, int high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static bool open_shared_memory() {
    shared_memory_fd = shm_open(shared_memory_name, O_RDWR, 0600);
    if (shared_memory_fd < 0) {
        print_errno("shm_open failed");
        return false;
    }
    std::printf("shared memory linked\n");
    return true;
}

static bool map_shared_memory() {
    void *mapped = mmap(nullptr, sizeof(game_state), PROT_READ | PROT_WRITE, MAP_SHARED, shared_memory_fd, 0);
    if (mapped == MAP_FAILED) {
        print_errno("mmap failed");
        return false;
    }
    shared_state = static_cast<game_state *>(mapped);
    std::printf("shared memory mapped\n");
    return true;
}

static void close_shared_memory_fd() {
    if (shared_memory_fd < 0) {
        return;
    }
    if (close(shared_memory_fd) != 0) {
        print_errno("close failed");
    }
    shared_memory_fd = -1;
}

static void unmap_shared_memory() {
    if (shared_state == nullptr) {
        return;
    }
    if (munmap(shared_state, sizeof(game_state)) != 0) {
        print_errno("munmap failed");
    }
    shared_state = nullptr;
}

static bool lock_state() {
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

static bool unlock_state() {
    if (sem_post(&shared_state->state_lock) != 0) {
        print_errno("sem_post state_lock failed");
        return false;
    }
    return true;
}

static int collect_living_players(int *player_indices, int capacity) {
    int count = 0;
    for (int i = 0; i < game_state::max_players && count < capacity; ++i) {
        if (shared_state->player_hp[i] > 0) {
            player_indices[count++] = i;
        }
    }
    return count;
}

static int pick_random_player(int *player_indices, int living_count) {
    if (living_count <= 0) {
        return -1;
    }
    int random_index = std::rand() % living_count;
    return player_indices[random_index];
}

static void apply_enemy_attack(int enemy_id) {
    int player_indices[game_state::max_players];
    int living_count = collect_living_players(player_indices, game_state::max_players);
    int target_player = pick_random_player(player_indices, living_count);
    if (target_player < 0) {
        return;
    }
    int next_hp = shared_state->player_hp[target_player] - enemy_attack_damage;
    shared_state->player_hp[target_player] = clamp_value(next_hp, 0, 99999);
    shared_state->enemy_stamina[enemy_id] = 0;
}

static void run_enemy_step(int enemy_id) {
    if (!lock_state()) {
        return;
    }
    if (enemy_id >= 0 && enemy_id < game_state::max_enemies) {
        if (shared_state->enemy_hp[enemy_id] > 0 && shared_state->enemy_stamina[enemy_id] >= enemy_attack_cost) {
            apply_enemy_attack(enemy_id);
        }
    }
    unlock_state();
}

static void *npc_logic_loop(void *arg) {
    int enemy_id = *static_cast<int *>(arg);
    while (running) {
        run_enemy_step(enemy_id);
        usleep(idle_sleep_us);
    }
    return nullptr;
}

static void handle_signal(int) {
    running = 0;
}

static bool register_signals() {
    if (std::signal(SIGINT, handle_signal) == SIG_ERR) {
        std::fprintf(stderr, "failed to register sigint handler\n");
        return false;
    }
    if (std::signal(SIGTERM, handle_signal) == SIG_ERR) {
        std::fprintf(stderr, "failed to register sigterm handler\n");
        return false;
    }
    return true;
}

static bool start_npc_threads() {
    for (int i = 0; i < game_state::max_enemies; ++i) {
        npc_ids[i] = i;
        if (pthread_create(&npc_threads[i], nullptr, npc_logic_loop, &npc_ids[i]) != 0) {
            print_errno("pthread_create failed");
            running = 0;
            return false;
        }
        started_thread_count++;
    }
    std::printf("npc threads started\n");
    return true;
}

static void join_npc_threads() {
    for (int i = 0; i < started_thread_count; ++i) {
        pthread_join(npc_threads[i], nullptr);
    }
}

static void cleanup() {
    unmap_shared_memory();
    close_shared_memory_fd();
}

int main() {
    std::srand(static_cast<unsigned int>(std::time(nullptr)) ^ static_cast<unsigned int>(getpid()));
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
    if (!start_npc_threads()) {
        join_npc_threads();
        cleanup();
        return 1;
    }
    join_npc_threads();
    cleanup();
    std::printf("asp exited cleanly\n");
    return 0;
}
