#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../gamestate.h"

static const char *shared_memory_name = "/chrono_rift_game_state";
static const int player_stamina_cap = 100;
static const int enemy_stamina_cap = 150;
static const unsigned int seeded_roll_value = 880;
static const useconds_t deadlock_check_sleep_us = 2000000;

static int shared_memory_fd = -1;
static game_state *shared_state = nullptr;
static bool semaphores_ready = false;
static bool resource_lock_ready = false;
static pthread_t deadlock_monitor_thread;

static void print_errno(const char *action) {
    std::fprintf(stderr, "%s: %s\n", action, std::strerror(errno));
}

static bool open_shared_memory() {
    shared_memory_fd = shm_open(shared_memory_name, O_CREAT | O_RDWR, 0600);
    if (shared_memory_fd < 0) {
        print_errno("shm_open failed");
        return false;
    }
    std::printf("shared memory opened\n");
    return true;
}

static bool size_shared_memory() {
    if (ftruncate(shared_memory_fd, sizeof(game_state)) != 0) {
        print_errno("ftruncate failed");
        return false;
    }
    std::printf("shared memory sized\n");
    return true;
}

static bool map_shared_memory() {
    void *mapped = mmap(nullptr, sizeof(game_state), PROT_READ | PROT_WRITE, MAP_SHARED, shared_memory_fd, 0);
    if (mapped == MAP_FAILED) {
        print_errno("mmap failed");
        return false;
    }
    shared_state = static_cast<game_state *>(mapped);
    std::printf("shared memory initialized\n");
    return true;
}

static bool initialize_semaphore(sem_t *target, unsigned int value, const char *name) {
    if (sem_init(target, 1, value) != 0) {
        std::fprintf(stderr, "sem_init failed for %s: %s\n", name, std::strerror(errno));
        return false;
    }
    return true;
}

static bool initialize_semaphores() {
    if (!initialize_semaphore(&shared_state->state_lock, 1, "state_lock")) {
        return false;
    }
    if (!initialize_semaphore(&shared_state->memory_sem, 1, "memory_sem")) {
        return false;
    }
    if (!initialize_semaphore(&shared_state->player_sem, 1, "player_sem")) {
        return false;
    }
    if (!initialize_semaphore(&shared_state->enemy_sem, 1, "enemy_sem")) {
        return false;
    }
    if (!initialize_semaphore(&shared_state->inventory_sem, 1, "inventory_sem")) {
        return false;
    }
    if (!initialize_semaphore(&shared_state->relic_sem, 1, "relic_sem")) {
        return false;
    }
    semaphores_ready = true;
    std::printf("semaphores linked\n");
    return true;
}

static bool initialize_resource_lock() {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        std::fprintf(stderr, "pthread_mutexattr_init failed\n");
        return false;
    }
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
        std::fprintf(stderr, "pthread_mutexattr_setpshared failed\n");
        pthread_mutexattr_destroy(&attr);
        return false;
    }
    if (pthread_mutex_init(&shared_state->resource_lock, &attr) != 0) {
        std::fprintf(stderr, "pthread_mutex_init failed\n");
        pthread_mutexattr_destroy(&attr);
        return false;
    }
    pthread_mutexattr_destroy(&attr);
    resource_lock_ready = true;
    return true;
}

static void clear_state() {
    for (int i = 0; i < game_state::max_players; ++i) {
        shared_state->player_hp[i] = 0;
        shared_state->player_speed[i] = 0;
        shared_state->player_stamina[i] = 0;
        shared_state->player_stun_end_time[i] = 0;
        for (int j = 0; j < game_state::inventory_slots; ++j) {
            shared_state->player_primary_inventory[i][j] = 0;
            shared_state->long_term_storage[i][j] = 0;
        }
    }
    for (int i = 0; i < game_state::max_enemies; ++i) {
        shared_state->enemy_hp[i] = 0;
        shared_state->enemy_speed[i] = 0;
        shared_state->enemy_stamina[i] = 0;
        shared_state->stun_end_time[i] = 0;
    }
    shared_state->eclipse_relic_holder = 0;
    shared_state->solar_core_holder = -1;
    shared_state->lunar_blade_holder = -1;
    shared_state->solar_core_waiter = -1;
    shared_state->lunar_blade_waiter = -1;
    shared_state->current_dropped_weapon = 0;
    shared_state->active_player_count = 0;
    shared_state->active_enemy_count = 0;
    shared_state->arbiter_pid = 0;
    shared_state->asp_pid = 0;
    shared_state->action_log[0] = '\0';
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

static bool lock_state() {
    while (sem_wait(&shared_state->state_lock) != 0) {
        if (errno == EINTR) {
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

static bool is_active_player(int index) {
    return shared_state->player_hp[index] > 0;
}

static bool is_active_enemy(int index) {
    return shared_state->enemy_hp[index] > 0;
}

static void update_player_stamina() {
    for (int i = 0; i < game_state::max_players; ++i) {
        if (!is_active_player(i)) {
            continue;
        }
        int next_value = shared_state->player_stamina[i] + shared_state->player_speed[i];
        shared_state->player_stamina[i] = clamp_value(next_value, 0, player_stamina_cap);
    }
}

static void update_enemy_stamina() {
    for (int i = 0; i < game_state::max_enemies; ++i) {
        if (!is_active_enemy(i)) {
            continue;
        }
        int next_value = shared_state->enemy_stamina[i] + shared_state->enemy_speed[i];
        shared_state->enemy_stamina[i] = clamp_value(next_value, 0, enemy_stamina_cap);
    }
}

static int roll_player_hp() {
    return 880 + (std::rand() % 901);
}

static int roll_enemy_hp() {
    return 80 + (std::rand() % 151);
}

static int roll_enemy_speed() {
    return (std::rand() % 21) + 10;
}

static void initialize_players() {
    for (int i = 0; i < game_state::max_players; ++i) {
        shared_state->player_hp[i] = roll_player_hp();
        shared_state->player_speed[i] = 100 / 4;
        shared_state->player_stamina[i] = 0;
    }
}

static void initialize_enemies() {
    for (int i = 0; i < game_state::max_enemies; ++i) {
        shared_state->enemy_hp[i] = roll_enemy_hp();
        shared_state->enemy_speed[i] = roll_enemy_speed();
        shared_state->enemy_stamina[i] = 0;
    }
}

static bool initialize_seeded_stats() {
    std::srand(seeded_roll_value);
    if (!lock_state()) {
        return false;
    }
    shared_state->active_player_count = game_state::max_players;
    shared_state->active_enemy_count = game_state::max_enemies;
    initialize_players();
    initialize_enemies();
    if (!unlock_state()) {
        return false;
    }
    return true;
}

static bool tick_stamina_progression() {
    if (!lock_state()) {
        return false;
    }
    update_player_stamina();
    update_enemy_stamina();
    if (!unlock_state()) {
        return false;
    }
    return true;
}

static void run_stamina_loop() {
    while (true) {
        sleep(1);
        if (!tick_stamina_progression()) {
            break;
        }
    }
}

static void copy_action_log_literal(const char *text) {
    int i = 0;
    while (i < 255 && text[i] != '\0') {
        shared_state->action_log[i] = text[i];
        i++;
    }
    shared_state->action_log[i] = '\0';
}

static void handle_alarm_signal(int) {
    pid_t target_pid = shared_state->asp_pid;
    if (target_pid > 0) {
        kill(target_pid, SIGCONT);
    }
    copy_action_log_literal("ultimate ended, asp resumed");
}

static void handle_ultimate_signal(int) {
    alarm(10);
    copy_action_log_literal("ultimate triggered! asp frozen for 10s");
}

static bool register_arbiter_pid() {
    if (!lock_state()) {
        return false;
    }
    shared_state->arbiter_pid = getpid();
    if (!unlock_state()) {
        return false;
    }
    return true;
}

static bool is_player_entity(int entity_id) {
    return entity_id >= 0 && entity_id < game_state::max_players;
}

static bool is_enemy_entity(int entity_id) {
    int enemy_base = game_state::max_players;
    return entity_id >= enemy_base && entity_id < enemy_base + game_state::max_enemies;
}

static bool has_circular_wait_locked() {
    if (shared_state->solar_core_holder < 0 || shared_state->lunar_blade_holder < 0) {
        return false;
    }
    if (shared_state->solar_core_waiter < 0 || shared_state->lunar_blade_waiter < 0) {
        return false;
    }
    if (shared_state->solar_core_holder != shared_state->lunar_blade_waiter) {
        return false;
    }
    if (shared_state->lunar_blade_holder != shared_state->solar_core_waiter) {
        return false;
    }
    return true;
}

static void break_deadlock_locked() {
    if (is_enemy_entity(shared_state->lunar_blade_holder)) {
        shared_state->lunar_blade_holder = -1;
    } else if (is_enemy_entity(shared_state->solar_core_holder)) {
        shared_state->solar_core_holder = -1;
    } else if (is_player_entity(shared_state->lunar_blade_holder)) {
        shared_state->lunar_blade_holder = -1;
    } else {
        shared_state->solar_core_holder = -1;
    }
    std::snprintf(shared_state->action_log, sizeof(shared_state->action_log), "deadlock broken by arbiter");
}

static void *deadlock_monitor_loop(void *) {
    while (true) {
        usleep(deadlock_check_sleep_us);
        if (pthread_mutex_lock(&shared_state->resource_lock) != 0) {
            continue;
        }
        if (has_circular_wait_locked()) {
            break_deadlock_locked();
        }
        pthread_mutex_unlock(&shared_state->resource_lock);
    }
    return nullptr;
}

static bool start_deadlock_monitor_thread() {
    if (pthread_create(&deadlock_monitor_thread, nullptr, deadlock_monitor_loop, nullptr) != 0) {
        std::fprintf(stderr, "pthread_create deadlock monitor failed\n");
        return false;
    }
    return true;
}

static void destroy_semaphore(sem_t *target, const char *name) {
    if (sem_destroy(target) != 0) {
        std::fprintf(stderr, "sem_destroy failed for %s: %s\n", name, std::strerror(errno));
    }
}

static void destroy_semaphores() {
    if (!semaphores_ready || shared_state == nullptr) {
        return;
    }
    destroy_semaphore(&shared_state->state_lock, "state_lock");
    destroy_semaphore(&shared_state->memory_sem, "memory_sem");
    destroy_semaphore(&shared_state->player_sem, "player_sem");
    destroy_semaphore(&shared_state->enemy_sem, "enemy_sem");
    destroy_semaphore(&shared_state->inventory_sem, "inventory_sem");
    destroy_semaphore(&shared_state->relic_sem, "relic_sem");
    semaphores_ready = false;
}

static void destroy_resource_lock() {
    if (!resource_lock_ready || shared_state == nullptr) {
        return;
    }
    if (pthread_mutex_destroy(&shared_state->resource_lock) != 0) {
        std::fprintf(stderr, "pthread_mutex_destroy failed\n");
    }
    resource_lock_ready = false;
}

static void unmap_shared_memory() {
    if (shared_state == nullptr) {
        return;
    }
    if (munmap(shared_state, sizeof(game_state)) != 0) {
        print_errno("munmap failed");
    } else {
        std::printf("shared memory unmapped\n");
    }
    shared_state = nullptr;
}

static void close_descriptor() {
    if (shared_memory_fd >= 0) {
        if (close(shared_memory_fd) != 0) {
            print_errno("close failed");
        }
        shared_memory_fd = -1;
    }
}

static void unlink_shared_memory() {
    if (shm_unlink(shared_memory_name) != 0 && errno != ENOENT) {
        print_errno("shm_unlink failed");
    } else {
        std::printf("shared memory unlinked\n");
    }
}

static void cleanup() {
    destroy_resource_lock();
    destroy_semaphores();
    unmap_shared_memory();
    close_descriptor();
    unlink_shared_memory();
}

static void handle_exit_signal(int) {
    std::exit(0);
}

static bool register_exit_handlers() {
    if (std::atexit(cleanup) != 0) {
        std::fprintf(stderr, "failed to register cleanup\n");
        return false;
    }
    if (std::signal(SIGINT, handle_exit_signal) == SIG_ERR) {
        std::fprintf(stderr, "failed to register sigint handler\n");
        return false;
    }
    if (std::signal(SIGTERM, handle_exit_signal) == SIG_ERR) {
        std::fprintf(stderr, "failed to register sigterm handler\n");
        return false;
    }
    if (std::signal(SIGALRM, handle_alarm_signal) == SIG_ERR) {
        std::fprintf(stderr, "failed to register sigalrm handler\n");
        return false;
    }
    if (std::signal(SIGUSR1, handle_ultimate_signal) == SIG_ERR) {
        std::fprintf(stderr, "failed to register sigusr1 handler\n");
        return false;
    }
    return true;
}

static bool setup_shared_state() {
    if (!open_shared_memory()) {
        return false;
    }
    if (!size_shared_memory()) {
        return false;
    }
    if (!map_shared_memory()) {
        return false;
    }
    clear_state();
    if (!initialize_resource_lock()) {
        return false;
    }
    if (!initialize_semaphores()) {
        return false;
    }
    if (!initialize_seeded_stats()) {
        return false;
    }
    return true;
}

int main() {
    if (!register_exit_handlers()) {
        return 1;
    }
    if (!setup_shared_state()) {
        return 1;
    }
    if (!register_arbiter_pid()) {
        return 1;
    }
    if (!start_deadlock_monitor_thread()) {
        return 1;
    }
    std::printf("arbiter ready\n");
    run_stamina_loop();
    return 0;
}
