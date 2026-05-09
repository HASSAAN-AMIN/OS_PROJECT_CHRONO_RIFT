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
const int default_roll_number = 240880;
const useconds_t deadlock_check_sleep_us = 2000000;
const int eclipse_relic_spawn_seconds = 25;

int shared_memory_fd = -1;
game_state *shared_state = nullptr;
bool semaphores_ready = false;
bool resource_lock_ready = false;
pthread_t deadlock_monitor_thread;
pthread_t outcome_monitor_thread;
bool deadlock_monitor_started = false;
bool outcome_monitor_started = false;
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
time_t game_start_time = 0;
bool eclipse_relic_dropped = false;
int previous_enemy_hp[game_state::max_enemies] = {0};
int active_roll_number = default_roll_number;

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
        shared_state->player_max_hp[i] = 0;
        shared_state->player_speed[i] = 0;
        shared_state->player_stamina[i] = 0;
        shared_state->player_damage[i] = 0;
        shared_state->player_stun_end_time[i] = 0;
        for (int j = 0; j < game_state::inventory_slots; ++j) {
            shared_state->player_primary_inventory[i][j] = 0;
            shared_state->long_term_storage[i][j] = 0;
        }
    }
    for (int i = 0; i < game_state::max_enemies; ++i) {
        shared_state->enemy_hp[i] = 0;
        shared_state->enemy_max_hp[i] = 0;
        shared_state->enemy_speed[i] = 0;
        shared_state->enemy_stamina[i] = 0;
        shared_state->enemy_damage[i] = 0;
        shared_state->stun_end_time[i] = 0;
        previous_enemy_hp[i] = 0;
    }
    shared_state->eclipse_relic_holder = -1;
    shared_state->eclipse_relic_present = 0;
    shared_state->solar_core_holder = -1;
    shared_state->lunar_blade_holder = -1;
    shared_state->solar_core_waiter = -1;
    shared_state->lunar_blade_waiter = -1;
    shared_state->current_dropped_weapon = 0;
    shared_state->active_player_count = 0;
    shared_state->active_enemy_count = 0;
    shared_state->active_player_index = -1;
    shared_state->enemy_kills = 0;
    shared_state->total_kills = 0;
    shared_state->outcome = game_state::outcome_ongoing;
    shared_state->roll_number = active_roll_number;
    shared_state->arbiter_pid = 0;
    shared_state->asp_pid = 0;
    shared_state->hip_pid = 0;
    shared_state->action_log[0] = '\0';
    shared_state->last_event[0] = '\0';
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
    int player_count = shared_state->active_player_count;
    if (player_count < 0) {
        player_count = 0;
    }
    if (player_count > game_state::max_players) {
        player_count = game_state::max_players;
    }
    for (int i = 0; i < player_count; ++i) {
        if (!is_active_player(i)) {
            continue;
        }
        if (shared_state->player_stun_end_time[i] > time(nullptr)) {
            continue;
        }
        int next_value = shared_state->player_stamina[i] + shared_state->player_speed[i];
        shared_state->player_stamina[i] = clamp_value(next_value, 0, game_state::player_max_stamina);
    }
}

void update_enemy_stamina() {
    int enemy_count = shared_state->active_enemy_count;
    if (enemy_count < 0) {
        enemy_count = 0;
    }
    if (enemy_count > game_state::max_enemies) {
        enemy_count = game_state::max_enemies;
    }
    for (int i = 0; i < enemy_count; ++i) {
        if (!is_active_enemy(i)) {
            continue;
        }
        if (shared_state->stun_end_time[i] > time(nullptr)) {
            continue;
        }
        int next_value = shared_state->enemy_stamina[i] + shared_state->enemy_speed[i];
        shared_state->enemy_stamina[i] = clamp_value(next_value, 0, game_state::enemy_max_stamina);
    }
}

int last_two_digits(int value) {
    if (value < 0) {
        value = -value;
    }
    return value % 100;
}

int last_digit(int value) {
    if (value < 0) {
        value = -value;
    }
    return value % 10;
}

int second_last_digit(int value) {
    if (value < 0) {
        value = -value;
    }
    return (value / 10) % 10;
}

int roll_player_hp() {
    return 880 + (rand() % 901);
}

int roll_enemy_hp() {
    return 80 + 50 + (rand() % 151);
}

int roll_enemy_speed() {
    return 10 + (rand() % 21);
}

int roll_enemy_count() {
    return 2 + (rand() % 8);
}

int player_damage_value() {
    return last_digit(active_roll_number) + 10;
}

int enemy_damage_value() {
    return second_last_digit(active_roll_number) + 10;
}

void initialize_players() {
    int player_count = shared_state->active_player_count;
    if (player_count <= 0 || player_count > game_state::max_players) {
        player_count = game_state::max_players;
    }
    int per_player_speed = 100 / player_count;
    if (per_player_speed < 1) {
        per_player_speed = 1;
    }
    for (int i = 0; i < game_state::max_players; ++i) {
        if (i < player_count) {
            shared_state->player_hp[i] = roll_player_hp();
            shared_state->player_max_hp[i] = shared_state->player_hp[i];
            shared_state->player_speed[i] = per_player_speed;
            shared_state->player_stamina[i] = 0;
            shared_state->player_damage[i] = player_damage_value();
        } else {
            shared_state->player_hp[i] = 0;
            shared_state->player_max_hp[i] = 0;
            shared_state->player_speed[i] = 0;
            shared_state->player_stamina[i] = 0;
            shared_state->player_damage[i] = 0;
        }
    }
    if (shared_state->active_player_count <= 0) {
        shared_state->active_player_count = player_count;
    }
}

void initialize_enemies() {
    int enemy_count = roll_enemy_count();
    if (enemy_count < 2) {
        enemy_count = 2;
    }
    if (enemy_count > game_state::max_enemies) {
        enemy_count = game_state::max_enemies;
    }
    int common_damage = enemy_damage_value();
    for (int i = 0; i < game_state::max_enemies; ++i) {
        if (i < enemy_count) {
            shared_state->enemy_hp[i] = roll_enemy_hp();
            shared_state->enemy_max_hp[i] = shared_state->enemy_hp[i];
            shared_state->enemy_speed[i] = roll_enemy_speed();
            shared_state->enemy_stamina[i] = 0;
            shared_state->enemy_damage[i] = common_damage;
        } else {
            shared_state->enemy_hp[i] = 0;
            shared_state->enemy_max_hp[i] = 0;
            shared_state->enemy_speed[i] = 0;
            shared_state->enemy_stamina[i] = 0;
            shared_state->enemy_damage[i] = 0;
        }
        previous_enemy_hp[i] = shared_state->enemy_hp[i];
    }
    shared_state->active_enemy_count = enemy_count;
}

bool initialize_seeded_stats() {
    srand(240880);
    if (!lock_state()) {
        return false;
    }
    initialize_players();
    initialize_enemies();
    snprintf(
        shared_state->action_log, sizeof(shared_state->action_log),
        "battle begins: %d players vs %d enemies (seed=%d)",
        shared_state->active_player_count,
        shared_state->active_enemy_count,
        active_roll_number
    );
    snprintf(shared_state->last_event, sizeof(shared_state->last_event), "init");
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

void track_enemy_deaths_locked() {
    int enemy_count = shared_state->active_enemy_count;
    if (enemy_count < 0) {
        enemy_count = 0;
    }
    if (enemy_count > game_state::max_enemies) {
        enemy_count = game_state::max_enemies;
    }
    for (int i = 0; i < enemy_count; ++i) {
        int prev = previous_enemy_hp[i];
        int curr = shared_state->enemy_hp[i];
        if (prev > 0 && curr <= 0) {
            shared_state->total_kills += 1;
            shared_state->enemy_kills = shared_state->total_kills;
            if (shared_state->total_kills >= 10) {
                snprintf(shared_state->action_log, sizeof(shared_state->action_log), "VICTORY: 10 enemies defeated!");
                shared_state->outcome = game_state::outcome_win;
                arbiter_running = 0;
            } else {
                shared_state->enemy_hp[i] = roll_enemy_hp();
                shared_state->enemy_max_hp[i] = shared_state->enemy_hp[i];
                shared_state->enemy_stamina[i] = 0;
                shared_state->stun_end_time[i] = 0;
                snprintf(shared_state->action_log, sizeof(shared_state->action_log), "enemy respawned");
            }
        }
        previous_enemy_hp[i] = shared_state->enemy_hp[i];
    }
}

void update_active_player_locked() {
    int chosen = -1;
    int best_stamina = -1;
    int player_count = shared_state->active_player_count;
    if (player_count < 0) {
        player_count = 0;
    }
    if (player_count > game_state::max_players) {
        player_count = game_state::max_players;
    }
    for (int i = 0; i < player_count; ++i) {
        if (shared_state->player_hp[i] <= 0) {
            continue;
        }
        if (shared_state->player_stun_end_time[i] > time(nullptr)) {
            continue;
        }
        int s = shared_state->player_stamina[i];
        if (s > best_stamina) {
            best_stamina = s;
            chosen = i;
        }
    }
    if (chosen < 0) {
        for (int i = 0; i < player_count; ++i) {
            if (shared_state->player_hp[i] > 0) {
                chosen = i;
                break;
            }
        }
    }
    shared_state->active_player_index = chosen;
}

void maybe_spawn_eclipse_relic_locked(time_t now) {
    if (eclipse_relic_dropped) {
        return;
    }
    if (now - game_start_time < eclipse_relic_spawn_seconds) {
        return;
    }
    if (shared_state->current_dropped_weapon != 0) {
        return;
    }
    shared_state->current_dropped_weapon = game_state::eclipse_relic_id;
    shared_state->eclipse_relic_present = 1;
    snprintf(
        shared_state->action_log, sizeof(shared_state->action_log),
        "eclipse relic surges into the arena - pick it up!"
    );
    snprintf(shared_state->last_event, sizeof(shared_state->last_event), "eclipse_spawn");
    eclipse_relic_dropped = true;
}

void check_outcome_locked() {
    if (shared_state->outcome != game_state::outcome_ongoing) {
        return;
    }
    if (shared_state->enemy_kills >= game_state::kills_required_to_win) {
        shared_state->outcome = game_state::outcome_win;
        snprintf(
            shared_state->action_log, sizeof(shared_state->action_log),
            "VICTORY: %d enemies vanquished",
            shared_state->enemy_kills
        );
        snprintf(shared_state->last_event, sizeof(shared_state->last_event), "victory");
        return;
    }
    int alive_players = 0;
    for (int i = 0; i < game_state::max_players; ++i) {
        if (i < shared_state->active_player_count && shared_state->player_hp[i] > 0) {
            alive_players += 1;
        }
    }
    if (alive_players == 0 && shared_state->active_player_count > 0) {
        shared_state->outcome = game_state::outcome_lose;
        snprintf(
            shared_state->action_log, sizeof(shared_state->action_log),
            "DEFEAT: party wiped"
        );
        snprintf(shared_state->last_event, sizeof(shared_state->last_event), "defeat");
    }
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
        snprintf(shared_state->action_log, sizeof(shared_state->action_log), "ultimate triggered: asp frozen for 10s");
        snprintf(shared_state->last_event, sizeof(shared_state->last_event), "ultimate_start");
    }
    if (ultimate_ended) {
        ultimate_ended = 0;
        snprintf(shared_state->action_log, sizeof(shared_state->action_log), "ultimate ended: asp resumed");
        snprintf(shared_state->last_event, sizeof(shared_state->last_event), "ultimate_end");
    }
    if (stun_triggered) {
        stun_triggered = 0;
        snprintf(shared_state->action_log, sizeof(shared_state->action_log), "stun pulse: arena frozen 3s");
        snprintf(shared_state->last_event, sizeof(shared_state->last_event), "stun_start");
    }
    if (stun_ended) {
        stun_ended = 0;
        snprintf(shared_state->action_log, sizeof(shared_state->action_log), "stun ended: combatants released");
        snprintf(shared_state->last_event, sizeof(shared_state->last_event), "stun_end");
    }
    if (deadlock_broken) {
        deadlock_broken = 0;
        snprintf(shared_state->action_log, sizeof(shared_state->action_log), "deadlock monitor forced an artifact drop");
        snprintf(shared_state->last_event, sizeof(shared_state->last_event), "deadlock_break");
    }
    update_player_stamina();
    update_enemy_stamina();
    track_enemy_deaths_locked();
    update_active_player_locked();
    maybe_spawn_eclipse_relic_locked(now);
    check_outcome_locked();
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

void *outcome_monitor_loop(void *) {
    while (arbiter_running) {
        sleep(1);
        if (!arbiter_running) {
            break;
        }
        if (lock_state()) {
            int outcome_value = shared_state->outcome;
            unlock_state();
            if (outcome_value != game_state::outcome_ongoing) {
                arbiter_running = 0;
                break;
            }
        }
    }
    return nullptr;
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

void handle_quit_signal(int) {
    if (shared_state != nullptr) {
        shared_state->outcome = game_state::outcome_quit;
    }
    arbiter_running = 0;
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

bool start_outcome_monitor_thread() {
    if (pthread_create(&outcome_monitor_thread, nullptr, outcome_monitor_loop, nullptr) != 0) {
        fprintf(stderr, "pthread_create outcome monitor failed\n");
        return false;
    }
    outcome_monitor_started = true;
    return true;
}

void stop_monitor_threads() {
    arbiter_running = 0;
    if (deadlock_monitor_started) {
        pthread_join(deadlock_monitor_thread, nullptr);
        deadlock_monitor_started = false;
    }
    if (outcome_monitor_started) {
        pthread_join(outcome_monitor_thread, nullptr);
        outcome_monitor_started = false;
    }
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
    if (!register_signal_handler(SIGTERM, handle_quit_signal, "sigterm")) {
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

void parse_arguments(int argc, char **argv) {
    if (argc >= 2) {
        int parsed = atoi(argv[1]);
        if (parsed > 0) {
            active_roll_number = parsed;
        }
    }
    const char *env_value = getenv("ROLL_NUMBER");
    if (env_value != nullptr) {
        int parsed = atoi(env_value);
        if (parsed > 0) {
            active_roll_number = parsed;
        }
    }
}

int main(int argc, char **argv) {
    parse_arguments(argc, argv);
    printf("arbiter starting with roll number %d\n", active_roll_number);
    game_start_time = time(nullptr);
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
    if (!start_outcome_monitor_thread()) {
        cleanup();
        return 1;
    }
    printf("arbiter ready: %d players vs %d enemies\n",
           shared_state->active_player_count, shared_state->active_enemy_count);
    run_stamina_loop();
    stop_monitor_threads();
    cleanup();
    printf("arbiter exited cleanly\n");
    return 0;
}
