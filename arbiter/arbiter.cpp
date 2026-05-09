#include <cerrno>
#include <csignal>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../gamestate.h"

using namespace std;

const char *shared_memory_name = "/chrono_rift_game_state";
const int player_stamina_cap = 100;
const int enemy_stamina_cap = 150;
const unsigned int seeded_roll_value = 880;
const useconds_t deadlock_check_sleep_us = 2000000;

int shared_memory_fd = -1;
game_state *shared_state = nullptr;
bool semaphores_ready = false;
bool resource_lock_ready = false;
pthread_t deadlock_monitor_thread;
bool deadlock_monitor_started = false;
volatile sig_atomic_t arbiter_running = 1;
volatile sig_atomic_t ultimate_triggered = 0;
volatile sig_atomic_t ultimate_ended = 0;
volatile sig_atomic_t stun_triggered = 0;
volatile sig_atomic_t stun_ended = 0;
volatile sig_atomic_t asp_frozen = 0;
volatile sig_atomic_t hip_frozen = 0;
volatile sig_atomic_t ultimate_requested = 0;
volatile sig_atomic_t stun_requested = 0;
volatile sig_atomic_t alarm_fired = 0;
volatile sig_atomic_t cached_asp_pid = 0;
volatile sig_atomic_t cached_hip_pid = 0;
volatile sig_atomic_t deadlock_broken = 0;
time_t ultimate_end_time = 0;
time_t stun_end_time = 0;

void print_errno(const char *action) {
    fprintf(stderr, "%s: %s\n", action, strerror(errno));
}

bool open_shared_memory() {
    shared_memory_fd = shm_open(shared_memory_name, O_CREAT | O_RDWR, 0600);
    if (shared_memory_fd < 0) {
        print_errno("shm_open failed");
        return false;
    }
    printf("shared memory opened\n");
    return true;
}

bool size_shared_memory() {
    if (ftruncate(shared_memory_fd, sizeof(game_state)) != 0) {
        print_errno("ftruncate failed");
        return false;
    }
    printf("shared memory sized\n");
    return true;
}

bool map_shared_memory() {
    void *mapped = mmap(nullptr, sizeof(game_state), PROT_READ | PROT_WRITE, MAP_SHARED, shared_memory_fd, 0);
    if (mapped == MAP_FAILED) {
        print_errno("mmap failed");
        return false;
    }
    shared_state = static_cast<game_state *>(mapped);
    printf("shared memory initialized\n");
    return true;
}

bool initialize_semaphore(sem_t *target, unsigned int value, const char *name) {
    if (sem_init(target, 1, value) != 0) {
        fprintf(stderr, "sem_init failed for %s: %s\n", name, strerror(errno));
        return false;
    }
    return true;
}

bool initialize_semaphores() {
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
    printf("semaphores linked\n");
    return true;
}

bool initialize_resource_lock() {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        fprintf(stderr, "pthread_mutexattr_init failed\n");
        return false;
    }
    if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0) {
        fprintf(stderr, "pthread_mutexattr_setpshared failed\n");
        pthread_mutexattr_destroy(&attr);
        return false;
    }
    if (pthread_mutex_init(&shared_state->resource_lock, &attr) != 0) {
        fprintf(stderr, "pthread_mutex_init failed\n");
        pthread_mutexattr_destroy(&attr);
        return false;
    }
    pthread_mutexattr_destroy(&attr);
    resource_lock_ready = true;
    return true;
}

void clear_state() {
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
    shared_state->eclipse_relic_holder = -1;
    shared_state->solar_core_holder = -1;
    shared_state->lunar_blade_holder = -1;
    shared_state->solar_core_waiter = -1;
    shared_state->lunar_blade_waiter = -1;
    shared_state->current_dropped_weapon = 0;
    shared_state->active_player_count = 0;
    shared_state->active_enemy_count = 0;
    shared_state->arbiter_pid = 0;
    shared_state->asp_pid = 0;
    shared_state->hip_pid = 0;
    shared_state->action_log[0] = '\0';
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

bool lock_state() {
    while (sem_wait(&shared_state->state_lock) != 0) {
        if (errno == EINTR) {
            if (!arbiter_running) {
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

bool is_active_player(int index) {
    return shared_state->player_hp[index] > 0;
}

bool is_active_enemy(int index) {
    return shared_state->enemy_hp[index] > 0;
}

void update_player_stamina() {
    for (int i = 0; i < game_state::max_players; ++i) {
        if (!is_active_player(i)) {
            continue;
        }
        int next_value = shared_state->player_stamina[i] + shared_state->player_speed[i];
        shared_state->player_stamina[i] = clamp_value(next_value, 0, player_stamina_cap);
    }
}

void update_enemy_stamina() {
    for (int i = 0; i < game_state::max_enemies; ++i) {
        if (!is_active_enemy(i)) {
            continue;
        }
        int next_value = shared_state->enemy_stamina[i] + shared_state->enemy_speed[i];
        shared_state->enemy_stamina[i] = clamp_value(next_value, 0, enemy_stamina_cap);
    }
}

int roll_player_hp() {
    return 880 + (rand() % 901);
}

int roll_enemy_hp() {
    return 80 + (rand() % 151);
}

int roll_enemy_speed() {
    return (rand() % 21) + 10;
}

void initialize_players() {
    for (int i = 0; i < game_state::max_players; ++i) {
        shared_state->player_hp[i] = roll_player_hp();
        shared_state->player_speed[i] = 100 / 4;
        shared_state->player_stamina[i] = 0;
    }
}

void initialize_enemies() {
    for (int i = 0; i < game_state::max_enemies; ++i) {
        shared_state->enemy_hp[i] = roll_enemy_hp();
        shared_state->enemy_speed[i] = roll_enemy_speed();
        shared_state->enemy_stamina[i] = 0;
    }
}

bool initialize_seeded_stats() {
    srand(seeded_roll_value);
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

void update_cached_pids_locked() {
    cached_asp_pid = static_cast<sig_atomic_t>(shared_state->asp_pid);
    cached_hip_pid = static_cast<sig_atomic_t>(shared_state->hip_pid);
}

void apply_signal_requests_locked(time_t now) {
    if (ultimate_requested) {
        ultimate_requested = 0;
        ultimate_triggered = 1;
        asp_frozen = 1;
        ultimate_end_time = now + 10;
    }
    if (stun_requested) {
        stun_requested = 0;
        stun_triggered = 1;
        asp_frozen = 1;
        hip_frozen = 1;
        stun_end_time = now + 3;
    }
}

void process_freeze_endings_locked(time_t now) {
    if (alarm_fired) {
        alarm_fired = 0;
    }
    if (ultimate_end_time > 0 && now >= ultimate_end_time) {
        ultimate_end_time = 0;
        ultimate_ended = 1;
    }
    if (stun_end_time > 0 && now >= stun_end_time) {
        stun_end_time = 0;
        stun_ended = 1;
    }
    bool asp_should_stay_frozen = (ultimate_end_time > 0) || (stun_end_time > 0);
    bool hip_should_stay_frozen = (stun_end_time > 0);
    if (asp_frozen && !asp_should_stay_frozen) {
        pid_t asp_pid = static_cast<pid_t>(cached_asp_pid);
        if (asp_pid > 0) {
            kill(asp_pid, SIGCONT);
        }
        asp_frozen = 0;
    }
    if (hip_frozen && !hip_should_stay_frozen) {
        pid_t hip_pid = static_cast<pid_t>(cached_hip_pid);
        if (hip_pid > 0) {
            kill(hip_pid, SIGCONT);
        }
        hip_frozen = 0;
    }
}

void schedule_next_alarm_locked(time_t now) {
    int next_alarm_seconds = 0;
    if (ultimate_end_time > now) {
        next_alarm_seconds = static_cast<int>(ultimate_end_time - now);
    }
    if (stun_end_time > now) {
        int stun_seconds = static_cast<int>(stun_end_time - now);
        if (next_alarm_seconds == 0 || stun_seconds < next_alarm_seconds) {
            next_alarm_seconds = stun_seconds;
        }
    }
    if (next_alarm_seconds <= 0) {
        alarm(0);
        return;
    }
    alarm(next_alarm_seconds);
}

bool tick_stamina_progression() {
    if (!lock_state()) {
        return false;
    }
    time_t now = time(nullptr);
    update_cached_pids_locked();
    apply_signal_requests_locked(now);
    process_freeze_endings_locked(now);
    schedule_next_alarm_locked(now);
    if (ultimate_triggered) {
        ultimate_triggered = 0;
        snprintf(shared_state->action_log, sizeof(shared_state->action_log), "ultimate triggered! asp frozen for 10s");
    }
    if (ultimate_ended) {
        ultimate_ended = 0;
        snprintf(shared_state->action_log, sizeof(shared_state->action_log), "ultimate ended, asp resumed");
    }
    if (stun_triggered) {
        stun_triggered = 0;
        snprintf(shared_state->action_log, sizeof(shared_state->action_log), "stun triggered! entities frozen for 3s");
    }
    if (stun_ended) {
        stun_ended = 0;
        snprintf(shared_state->action_log, sizeof(shared_state->action_log), "stun ended, entities resumed");
    }
    if (deadlock_broken) {
        deadlock_broken = 0;
        snprintf(shared_state->action_log, sizeof(shared_state->action_log), "deadlock broken by arbiter");
    }
    update_player_stamina();
    update_enemy_stamina();
    if (!unlock_state()) {
        return false;
    }
    return true;
}

void run_stamina_loop() {
    while (arbiter_running) {
        sleep(1);
        if (!arbiter_running) {
            break;
        }
        if (!tick_stamina_progression()) {
            break;
        }
    }
}

void handle_alarm_signal(int) {
    alarm_fired = 1;
}

void handle_ultimate_signal(int) {
    ultimate_requested = 1;
}

void handle_stun_signal(int) {
    stun_requested = 1;
}

bool register_arbiter_pid() {
    if (!lock_state()) {
        return false;
    }
    shared_state->arbiter_pid = getpid();
    if (!unlock_state()) {
        return false;
    }
    return true;
}

bool is_player_entity(int entity_id) {
    return entity_id >= 0 && entity_id < game_state::max_players;
}

bool is_enemy_entity(int entity_id) {
    int enemy_base = game_state::max_players;
    return entity_id >= enemy_base && entity_id < enemy_base + game_state::max_enemies;
}

bool has_circular_wait_locked() {
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

void break_deadlock_locked() {
    if (is_enemy_entity(shared_state->lunar_blade_holder)) {
        shared_state->lunar_blade_holder = -1;
    } else if (is_enemy_entity(shared_state->solar_core_holder)) {
        shared_state->solar_core_holder = -1;
    } else if (is_player_entity(shared_state->lunar_blade_holder)) {
        shared_state->lunar_blade_holder = -1;
    } else {
        shared_state->solar_core_holder = -1;
    }
    deadlock_broken = 1;
}

void *deadlock_monitor_loop(void *) {
    while (arbiter_running) {
        while (usleep(deadlock_check_sleep_us) == -1 && errno == EINTR) {
            if (!arbiter_running) {
                break;
            }
        }
        if (!arbiter_running) {
            break;
        }
        if (!lock_state()) {
            continue;
        }
        if (pthread_mutex_lock(&shared_state->resource_lock) != 0) {
            unlock_state();
            continue;
        }
        if (has_circular_wait_locked()) {
            break_deadlock_locked();
        }
        pthread_mutex_unlock(&shared_state->resource_lock);
        unlock_state();
    }
    return nullptr;
}

bool start_deadlock_monitor_thread() {
    if (pthread_create(&deadlock_monitor_thread, nullptr, deadlock_monitor_loop, nullptr) != 0) {
        fprintf(stderr, "pthread_create deadlock monitor failed\n");
        return false;
    }
    deadlock_monitor_started = true;
    return true;
}

void stop_deadlock_monitor_thread() {
    if (!deadlock_monitor_started) {
        return;
    }
    arbiter_running = 0;
    pthread_join(deadlock_monitor_thread, nullptr);
    deadlock_monitor_started = false;
}

void destroy_semaphore(sem_t *target, const char *name) {
    if (sem_destroy(target) != 0) {
        fprintf(stderr, "sem_destroy failed for %s: %s\n", name, strerror(errno));
    }
}

void destroy_semaphores() {
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

void destroy_resource_lock() {
    if (!resource_lock_ready || shared_state == nullptr) {
        return;
    }
    if (pthread_mutex_destroy(&shared_state->resource_lock) != 0) {
        fprintf(stderr, "pthread_mutex_destroy failed\n");
    }
    resource_lock_ready = false;
}

void unmap_shared_memory() {
    if (shared_state == nullptr) {
        return;
    }
    if (munmap(shared_state, sizeof(game_state)) != 0) {
        print_errno("munmap failed");
    } else {
        printf("shared memory unmapped\n");
    }
    shared_state = nullptr;
}

void close_descriptor() {
    if (shared_memory_fd >= 0) {
        if (close(shared_memory_fd) != 0) {
            print_errno("close failed");
        }
        shared_memory_fd = -1;
    }
}

void unlink_shared_memory() {
    if (shm_unlink(shared_memory_name) != 0 && errno != ENOENT) {
        print_errno("shm_unlink failed");
    } else {
        printf("shared memory unlinked\n");
    }
}

void cleanup() {
    destroy_resource_lock();
    destroy_semaphores();
    unmap_shared_memory();
    close_descriptor();
    unlink_shared_memory();
}

void handle_exit_signal(int) {
    arbiter_running = 0;
}

bool register_signal_handler(int signal_number, void (*handler)(int), const char *signal_name) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handler;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (sigaction(signal_number, &action, nullptr) != 0) {
        fprintf(stderr, "failed to register %s handler\n", signal_name);
        return false;
    }
    return true;
}

bool register_exit_handlers() {
    if (!register_signal_handler(SIGINT, handle_exit_signal, "sigint")) {
        return false;
    }
    if (!register_signal_handler(SIGTERM, handle_exit_signal, "sigterm")) {
        return false;
    }
    if (!register_signal_handler(SIGALRM, handle_alarm_signal, "sigalrm")) {
        return false;
    }
    if (!register_signal_handler(SIGUSR1, handle_ultimate_signal, "sigusr1")) {
        return false;
    }
    if (!register_signal_handler(SIGUSR2, handle_stun_signal, "sigusr2")) {
        return false;
    }
    return true;
}

bool setup_shared_state() {
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
        cleanup();
        return 1;
    }
    if (!register_arbiter_pid()) {
        cleanup();
        return 1;
    }
    if (!start_deadlock_monitor_thread()) {
        cleanup();
        return 1;
    }
    printf("arbiter ready\n");
    run_stamina_loop();
    stop_deadlock_monitor_thread();
    cleanup();
    return 0;
}
