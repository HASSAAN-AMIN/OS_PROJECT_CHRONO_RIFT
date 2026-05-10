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
const int default_roll_number = 880;
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
volatile sig_atomic_t asp_frozen = 0;
volatile sig_atomic_t ultimate_requested = 0;
volatile sig_atomic_t stun_requested = 0;
volatile sig_atomic_t alarm_fired = 0;
volatile sig_atomic_t cached_asp_pid = 0;
volatile sig_atomic_t cached_hip_pid = 0;
volatile sig_atomic_t deadlock_broken = 0;
time_t ultimate_end_time = 0;
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
        shared_state->player_pending_action[i] = game_state::action_none;
        shared_state->player_pending_target[i] = -1;
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
        shared_state->enemy_ready_since[i] = 0;
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
    for (int i = 0; i < game_state::max_enemies; ++i) {
        shared_state->enemy_display_id[i] = 0;
        shared_state->enemy_dead_count[i] = 0;
    }
    shared_state->next_enemy_display_id = 1;
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
    for (int i = 0; i < game_state::max_players; ++i) {
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
    for (int i = 0; i < game_state::max_enemies; ++i) {
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
    return active_roll_number + 100 + (rand() % 901);
}

int roll_enemy_hp() {
    return (active_roll_number % 100) + 50 + (rand() % 151);
}

int roll_enemy_speed() {
    return 10 + (rand() % 21);
}

int weapon_slot_size(int weapon_id) {
    switch (weapon_id) {
        case game_state::splinter_stick_id: return game_state::splinter_stick_slots;
        case game_state::venom_dagger_id: return game_state::venom_dagger_slots;
        case game_state::obsidian_axe_id: return game_state::obsidian_axe_slots;
        case game_state::frostbow_id: return game_state::frostbow_slots;
        case game_state::thunderstaff_id: return game_state::thunderstaff_slots;
        case game_state::iron_halberd_id: return game_state::iron_halberd_slots;
        case game_state::solar_core_id: return game_state::solar_core_slots;
        case game_state::lunar_blade_id: return game_state::lunar_blade_slots;
        case game_state::eclipse_relic_id: return game_state::eclipse_relic_slots;
        default: return 0;
    }
}

int weapon_damage_value(int weapon_id) {
    switch (weapon_id) {
        case game_state::splinter_stick_id: return game_state::splinter_stick_damage;
        case game_state::venom_dagger_id: return game_state::venom_dagger_damage;
        case game_state::obsidian_axe_id: return game_state::obsidian_axe_damage;
        case game_state::frostbow_id: return game_state::frostbow_damage;
        case game_state::thunderstaff_id: return game_state::thunderstaff_damage;
        case game_state::iron_halberd_id: return game_state::iron_halberd_damage;
        case game_state::solar_core_id: return game_state::solar_core_damage;
        case game_state::lunar_blade_id: return game_state::lunar_blade_damage;
        case game_state::eclipse_relic_id: return game_state::eclipse_relic_damage;
        default: return 0;
    }
}

const char *weapon_name(int weapon_id) {
    switch (weapon_id) {
        case game_state::splinter_stick_id: return "splinter stick";
        case game_state::venom_dagger_id: return "venom dagger";
        case game_state::obsidian_axe_id: return "obsidian axe";
        case game_state::frostbow_id: return "frostbow";
        case game_state::thunderstaff_id: return "thunderstaff";
        case game_state::iron_halberd_id: return "iron halberd";
        case game_state::solar_core_id: return "solar core";
        case game_state::lunar_blade_id: return "lunar blade";
        case game_state::eclipse_relic_id: return "eclipse relic";
        default: return "fists";
    }
}

int random_drop_weapon_id() {
    int pool[] = {
        game_state::splinter_stick_id,
        game_state::venom_dagger_id,
        game_state::obsidian_axe_id,
        game_state::frostbow_id,
        game_state::thunderstaff_id,
        game_state::iron_halberd_id,
        game_state::solar_core_id,
        game_state::lunar_blade_id
    };
    int n = static_cast<int>(sizeof(pool) / sizeof(pool[0]));
    return pool[rand() % n];
}

int roll_enemy_count() {
    return 2 + (rand() % 8);
}

int player_damage_value() {
    return (active_roll_number % 10) + 10;
}

int enemy_damage_value() {
    return ((active_roll_number / 10) % 10) + 10;
}

int find_first_living_enemy_locked() {
    int enemy_count = shared_state->active_enemy_count;
    if (enemy_count < 0) {
        enemy_count = 0;
    }
    if (enemy_count > game_state::max_enemies) {
        enemy_count = game_state::max_enemies;
    }
    for (int i = 0; i < enemy_count; ++i) {
        if (shared_state->enemy_hp[i] > 0) {
            return i;
        }
    }
    return -1;
}

int resolve_enemy_target_locked(int requested) {
    int enemy_count = shared_state->active_enemy_count;
    if (enemy_count < 0) {
        enemy_count = 0;
    }
    if (enemy_count > game_state::max_enemies) {
        enemy_count = game_state::max_enemies;
    }
    if (requested >= 0 && requested < enemy_count && shared_state->enemy_hp[requested] > 0) {
        return requested;
    }
    return find_first_living_enemy_locked();
}

bool inventory_contains_locked(int player_id, int weapon_id) {
    for (int i = 0; i < game_state::inventory_slots; ++i) {
        if (shared_state->player_primary_inventory[player_id][i] == weapon_id) {
            return true;
        }
    }
    return false;
}

int find_contiguous_free_in_array(const int *arr, int needed) {
    int run = 0;
    int start = -1;
    for (int i = 0; i < game_state::inventory_slots; ++i) {
        if (arr[i] == 0) {
            if (run == 0) {
                start = i;
            }
            run++;
            if (run >= needed) {
                return start;
            }
        } else {
            run = 0;
            start = -1;
        }
    }
    return -1;
}

int find_first_weapon_run(const int *arr, int *run_size_out) {
    for (int i = 0; i < game_state::inventory_slots; ++i) {
        int w = arr[i];
        if (w == 0) {
            continue;
        }
        int size = weapon_slot_size(w);
        if (size <= 0) {
            size = 1;
        }
        *run_size_out = size;
        return i;
    }
    return -1;
}

void place_weapon(int *arr, int start, int size, int weapon_id) {
    for (int i = start; i < start + size; ++i) {
        arr[i] = weapon_id;
    }
}

void zero_weapon_run(int *arr, int start, int size) {
    for (int i = start; i < start + size; ++i) {
        arr[i] = 0;
    }
}

int allocate_weapon_iterative_locked(int player_id, int weapon_id) {
    int needed = weapon_slot_size(weapon_id);
    if (needed <= 0) {
        return -1;
    }
    int *primary = shared_state->player_primary_inventory[player_id];
    int *storage = shared_state->long_term_storage[player_id];
    int backup_primary[game_state::inventory_slots];
    int backup_storage[game_state::inventory_slots];
    for (int i = 0; i < game_state::inventory_slots; ++i) {
        backup_primary[i] = primary[i];
        backup_storage[i] = storage[i];
    }
    int safety = game_state::inventory_slots + 2;
    while (safety-- > 0) {
        int start = find_contiguous_free_in_array(primary, needed);
        if (start >= 0) {
            place_weapon(primary, start, needed, weapon_id);
            return start;
        }
        int evict_size = 0;
        int evict_start = find_first_weapon_run(primary, &evict_size);
        if (evict_start < 0) {
            break;
        }
        int evict_weapon = primary[evict_start];
        int storage_start = find_contiguous_free_in_array(storage, evict_size);
        if (storage_start < 0) {
            break;
        }
        place_weapon(storage, storage_start, evict_size, evict_weapon);
        zero_weapon_run(primary, evict_start, evict_size);
    }
    for (int i = 0; i < game_state::inventory_slots; ++i) {
        primary[i] = backup_primary[i];
        storage[i] = backup_storage[i];
    }
    return -1;
}

int find_best_weapon_locked(int player_id) {
    int best_id = 0;
    int best_dmg = 0;
    int i = 0;
    while (i < game_state::inventory_slots) {
        int w = shared_state->player_primary_inventory[player_id][i];
        if (w == 0) {
            i++;
            continue;
        }
        int size = weapon_slot_size(w);
        if (size <= 0) {
            size = 1;
        }
        int dmg = weapon_damage_value(w);
        if (dmg > best_dmg) {
            best_dmg = dmg;
            best_id = w;
        }
        i += size;
    }
    return best_id;
}

int swap_in_from_storage_locked(int player_id, int chosen_weapon) {
    int *storage = shared_state->long_term_storage[player_id];
    int chosen_start = -1;
    int chosen_size = 0;
    int i = 0;
    while (i < game_state::inventory_slots) {
        int w = storage[i];
        if (w == 0) {
            i++;
            continue;
        }
        int size = weapon_slot_size(w);
        if (size <= 0) {
            size = 1;
        }
        if (w == chosen_weapon) {
            chosen_start = i;
            chosen_size = size;
            break;
        }
        i += size;
    }
    if (chosen_weapon == 0 || chosen_start < 0 || chosen_size <= 0) {
        return -1;
    }
    zero_weapon_run(storage, chosen_start, chosen_size);
    int placed = allocate_weapon_iterative_locked(player_id, chosen_weapon);
    if (placed < 0) {
        place_weapon(storage, chosen_start, chosen_size, chosen_weapon);
        return -1;
    }
    return chosen_weapon;
}

void initialize_players() {
    int player_count = shared_state->active_player_count;
    if (player_count < 1 || player_count > game_state::max_players) {
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
    shared_state->active_player_count = player_count;
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
            shared_state->enemy_ready_since[i] = 0;
            shared_state->enemy_display_id[i] = i + 1;
            shared_state->enemy_dead_count[i] = 0;
        } else {
            shared_state->enemy_hp[i] = 0;
            shared_state->enemy_max_hp[i] = 0;
            shared_state->enemy_speed[i] = 0;
            shared_state->enemy_stamina[i] = 0;
            shared_state->enemy_damage[i] = 0;
            shared_state->enemy_ready_since[i] = 0;
            shared_state->enemy_display_id[i] = 0;
            shared_state->enemy_dead_count[i] = 0;
        }
        previous_enemy_hp[i] = shared_state->enemy_hp[i];
    }
    shared_state->active_enemy_count = enemy_count;
    shared_state->next_enemy_display_id = enemy_count + 1;
}

bool initialize_seeded_stats() {
    srand(static_cast<unsigned int>(active_roll_number));
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

void wait_for_hip_party_size() {
    const int max_attempts = 30;
    const useconds_t sleep_us = 100000;
    for (int attempt = 0; attempt < max_attempts && arbiter_running; ++attempt) {
        int player_count = shared_state->active_player_count;
        if (player_count > 0 && player_count <= game_state::max_players) {
            return;
        }
        usleep(sleep_us);
    }
}

bool is_player_entity_id(int entity_id) {
    return entity_id >= 0 && entity_id < game_state::max_players;
}

bool is_enemy_entity_id(int entity_id) {
    int base = game_state::max_players;
    return entity_id >= base && entity_id < base + game_state::max_enemies;
}

bool is_entity_alive_locked(int entity_id) {
    if (is_player_entity_id(entity_id)) {
        return shared_state->player_hp[entity_id] > 0;
    }
    if (is_enemy_entity_id(entity_id)) {
        int enemy_id = entity_id - game_state::max_players;
        return shared_state->enemy_hp[enemy_id] > 0;
    }
    return false;
}

void normalize_waiters_locked() {
    if (shared_state->solar_core_holder < 0 || !is_entity_alive_locked(shared_state->solar_core_holder)) {
        shared_state->solar_core_holder = -1;
        shared_state->solar_core_waiter = -1;
    }
    if (shared_state->lunar_blade_holder < 0 || !is_entity_alive_locked(shared_state->lunar_blade_holder)) {
        shared_state->lunar_blade_holder = -1;
        shared_state->lunar_blade_waiter = -1;
    }
    if (shared_state->solar_core_waiter >= 0 && !is_entity_alive_locked(shared_state->solar_core_waiter)) {
        shared_state->solar_core_waiter = -1;
    }
    if (shared_state->lunar_blade_waiter >= 0 && !is_entity_alive_locked(shared_state->lunar_blade_waiter)) {
        shared_state->lunar_blade_waiter = -1;
    }
}

void register_waiters_after_pickup_locked(int picker_entity, int weapon_id) {
    if (weapon_id == game_state::solar_core_id) {
        int lunar_holder = shared_state->lunar_blade_holder;
        if (lunar_holder >= 0 && lunar_holder != picker_entity) {
            shared_state->lunar_blade_waiter = picker_entity;
            shared_state->solar_core_waiter = lunar_holder;
        }
    } else if (weapon_id == game_state::lunar_blade_id) {
        int solar_holder = shared_state->solar_core_holder;
        if (solar_holder >= 0 && solar_holder != picker_entity) {
            shared_state->solar_core_waiter = picker_entity;
            shared_state->lunar_blade_waiter = solar_holder;
        }
    }
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
        (void)now;
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
    bool asp_should_stay_frozen = (ultimate_end_time > 0);
    if (asp_frozen && !asp_should_stay_frozen) {
        pid_t asp_pid = static_cast<pid_t>(cached_asp_pid);
        if (asp_pid > 0) {
            kill(asp_pid, SIGCONT);
        }
        asp_frozen = 0;
    }
    (void)now;
}

void schedule_next_alarm_locked(time_t now) {
    int next_alarm_seconds = 0;
    if (ultimate_end_time > now) {
        next_alarm_seconds = static_cast<int>(ultimate_end_time - now);
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
            normalize_waiters_locked();
            int dead_id = shared_state->enemy_display_id[i];
            if (dead_id <= 0) {
                dead_id = i + 1;
            }
            shared_state->enemy_dead_count[i] += 1;
            shared_state->total_kills += 1;
            shared_state->enemy_kills = shared_state->total_kills;
            shared_state->current_dropped_weapon = random_drop_weapon_id();
            if (!eclipse_relic_dropped && (rand() % 100) < 40) {
                shared_state->eclipse_relic_present = 1;
                shared_state->eclipse_relic_holder = -1;
                shared_state->current_dropped_weapon = game_state::eclipse_relic_id;
                eclipse_relic_dropped = true;
            }
            if (shared_state->total_kills >= game_state::kills_required_to_win) {
                shared_state->outcome = game_state::outcome_win;
                snprintf(shared_state->action_log, sizeof(shared_state->action_log), "VICTORY: all 10 enemies killed");
                snprintf(shared_state->last_event, sizeof(shared_state->last_event), "victory_10");
                arbiter_running = 0;
                previous_enemy_hp[i] = curr;
                continue;
            }
            int new_id = shared_state->next_enemy_display_id;
            if (new_id <= 0) {
                new_id = enemy_count + 1;
            }
            shared_state->next_enemy_display_id = new_id + 1;
            shared_state->enemy_display_id[i] = new_id;
            shared_state->enemy_hp[i] = roll_enemy_hp();
            shared_state->enemy_max_hp[i] = shared_state->enemy_hp[i];
            shared_state->enemy_stamina[i] = 0;
            shared_state->enemy_speed[i] = roll_enemy_speed();
            shared_state->enemy_damage[i] = enemy_damage_value();
            shared_state->stun_end_time[i] = 0;
            shared_state->enemy_ready_since[i] = 0;
            snprintf(
                shared_state->action_log, sizeof(shared_state->action_log),
                "enemy %d died, enemy %d entered slot %d, new enemy appeared",
                dead_id,
                new_id,
                i + 1
            );
            snprintf(shared_state->last_event, sizeof(shared_state->last_event), "enemy_respawn_%d", new_id);
            curr = shared_state->enemy_hp[i];
        }
        previous_enemy_hp[i] = curr;
    }
}

void update_active_player_locked() {
    int chosen = -1;
    int best_stamina = -1;
    for (int i = 0; i < game_state::max_players; ++i) {
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
        for (int i = 0; i < game_state::max_players; ++i) {
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
    if (shared_state->total_kills >= game_state::kills_required_to_win) {
        shared_state->outcome = game_state::outcome_win;
        snprintf(
            shared_state->action_log, sizeof(shared_state->action_log),
            "VICTORY: all 10 enemies killed"
        );
        snprintf(shared_state->last_event, sizeof(shared_state->last_event), "victory_10");
        arbiter_running = 0;
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

void reset_enemy_turn_timeout_locked() {
    for (int i = 0; i < game_state::max_enemies; ++i) {
        if (shared_state->enemy_hp[i] <= 0) {
            shared_state->enemy_ready_since[i] = 0;
            continue;
        }
        if (shared_state->enemy_stamina[i] < game_state::enemy_max_stamina) {
            shared_state->enemy_ready_since[i] = 0;
        }
    }
}

void apply_enemy_turn_timeouts_locked(time_t now) {
    int enemy_count = shared_state->active_enemy_count;
    if (enemy_count < 0) {
        enemy_count = 0;
    }
    if (enemy_count > game_state::max_enemies) {
        enemy_count = game_state::max_enemies;
    }
    for (int i = 0; i < enemy_count; ++i) {
        if (shared_state->enemy_hp[i] <= 0) {
            shared_state->enemy_ready_since[i] = 0;
            continue;
        }
        if (shared_state->stun_end_time[i] > now) {
            shared_state->enemy_ready_since[i] = 0;
            continue;
        }
        if (shared_state->enemy_stamina[i] >= game_state::enemy_max_stamina) {
            if (shared_state->enemy_ready_since[i] == 0) {
                shared_state->enemy_ready_since[i] = now;
            } else if (now - shared_state->enemy_ready_since[i] >= 3) {
                shared_state->enemy_stamina[i] = game_state::enemy_max_stamina / 2;
                shared_state->enemy_ready_since[i] = 0;
                snprintf(shared_state->action_log, sizeof(shared_state->action_log), "enemy %d timeout: forced skip", i + 1);
                snprintf(shared_state->last_event, sizeof(shared_state->last_event), "enemy_%d_timeout", i + 1);
            }
        } else {
            shared_state->enemy_ready_since[i] = 0;
        }
    }
}

void execute_player_action_locked(int player_id, int action, int target, time_t now) {
    if (player_id < 0 || player_id >= game_state::max_players) {
        return;
    }
    if (player_id >= shared_state->active_player_count) {
        return;
    }
    if (shared_state->player_hp[player_id] <= 0) {
        return;
    }
    if (shared_state->player_stun_end_time[player_id] > now) {
        return;
    }
    bool needs_full = action != game_state::action_none;
    if (needs_full && shared_state->player_stamina[player_id] < game_state::player_max_stamina) {
        return;
    }
    int enemy_id = -1;
    if (action == game_state::action_strike || action == game_state::action_exhaust ||
        action == game_state::action_use_weapon) {
        enemy_id = resolve_enemy_target_locked(target);
        if (enemy_id < 0) {
            return;
        }
    }
    if (action == game_state::action_strike) {
        int dmg = shared_state->player_damage[player_id];
        int next_hp = shared_state->enemy_hp[enemy_id] - dmg;
        if (next_hp < 0) {
            next_hp = 0;
        }
        shared_state->enemy_hp[enemy_id] = next_hp;
        shared_state->player_stamina[player_id] = 0;
    } else if (action == game_state::action_exhaust) {
        int dmg = shared_state->player_damage[player_id];
        int next_stamina = shared_state->enemy_stamina[enemy_id] - dmg;
        if (next_stamina < 0) {
            next_stamina = 0;
        }
        shared_state->enemy_stamina[enemy_id] = next_stamina;
        shared_state->player_stamina[player_id] = 0;
    } else if (action == game_state::action_heal) {
        int max_hp = shared_state->player_max_hp[player_id];
        int heal = max_hp / 10;
        if (heal < 1) {
            heal = 1;
        }
        int hp = shared_state->player_hp[player_id] + heal;
        if (hp > max_hp) {
            hp = max_hp;
        }
        shared_state->player_hp[player_id] = hp;
        shared_state->player_stamina[player_id] = 0;
    } else if (action == game_state::action_skip) {
        shared_state->player_stamina[player_id] = game_state::player_max_stamina / 2;
    } else if (action == game_state::action_pickup) {
        int dropped = shared_state->current_dropped_weapon;
        if (dropped != 0) {
            int placed = allocate_weapon_iterative_locked(player_id, dropped);
            if (placed >= 0) {
                shared_state->current_dropped_weapon = 0;
                if (dropped == game_state::solar_core_id) {
                    shared_state->solar_core_holder = player_id;
                    register_waiters_after_pickup_locked(player_id, dropped);
                } else if (dropped == game_state::lunar_blade_id) {
                    shared_state->lunar_blade_holder = player_id;
                    register_waiters_after_pickup_locked(player_id, dropped);
                } else if (dropped == game_state::eclipse_relic_id) {
                    shared_state->eclipse_relic_holder = player_id;
                    shared_state->eclipse_relic_present = 1;
                }
                shared_state->player_stamina[player_id] = 0;
            }
        }
    } else if (action == game_state::action_ultimate) {
        bool has_solar = inventory_contains_locked(player_id, game_state::solar_core_id);
        bool has_lunar = inventory_contains_locked(player_id, game_state::lunar_blade_id);
        if (has_solar && has_lunar) {
            shared_state->player_stamina[player_id] = 0;
            pid_t asp_pid = static_cast<pid_t>(cached_asp_pid);
            if (asp_pid > 0) {
                kill(asp_pid, SIGSTOP);
            }
            asp_frozen = 1;
            ultimate_end_time = now + 10;
            ultimate_triggered = 1;
        }
    } else if (action == game_state::action_use_weapon) {
        int weapon_id = find_best_weapon_locked(player_id);
        if (weapon_id != 0) {
            int dmg = weapon_damage_value(weapon_id);
            int next_hp = shared_state->enemy_hp[enemy_id] - dmg;
            if (next_hp < 0) {
                next_hp = 0;
            }
            shared_state->enemy_hp[enemy_id] = next_hp;
            shared_state->player_stamina[player_id] = 0;
        }
    } else if (action == game_state::action_swap_in) {
        int swapped = swap_in_from_storage_locked(player_id, target);
        if (swapped > 0) {
            shared_state->player_stamina[player_id] = 0;
        }
    }
}

void process_player_actions_locked(time_t now) {
    int player_count = shared_state->active_player_count;
    if (player_count < 0) {
        player_count = 0;
    }
    if (player_count > game_state::max_players) {
        player_count = game_state::max_players;
    }
    for (int i = 0; i < player_count; ++i) {
        int action = shared_state->player_pending_action[i];
        int target = shared_state->player_pending_target[i];
        if (action == game_state::action_none) {
            continue;
        }
        shared_state->player_pending_action[i] = game_state::action_none;
        shared_state->player_pending_target[i] = -1;
        execute_player_action_locked(i, action, target, now);
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
    if (deadlock_broken) {
        deadlock_broken = 0;
        snprintf(shared_state->action_log, sizeof(shared_state->action_log), "deadlock monitor forced an artifact drop");
        snprintf(shared_state->last_event, sizeof(shared_state->last_event), "deadlock_break");
    }
    process_player_actions_locked(now);
    update_player_stamina();
    update_enemy_stamina();
    apply_enemy_turn_timeouts_locked(now);
    track_enemy_deaths_locked();
    reset_enemy_turn_timeout_locked();
    normalize_waiters_locked();
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
    shared_state->solar_core_waiter = -1;
    shared_state->lunar_blade_waiter = -1;
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

bool register_signal_handler(int signal_number, void (*handler)(int), const char *signal_name, int flags) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handler;
    action.sa_flags = flags;
    sigemptyset(&action.sa_mask);
    if (sigaction(signal_number, &action, nullptr) != 0) {
        fprintf(stderr, "failed to register %s handler\n", signal_name);
        return false;
    }
    return true;
}

bool register_exit_handlers() {
    if (!register_signal_handler(SIGINT, handle_exit_signal, "sigint", SA_RESTART)) {
        return false;
    }
    if (!register_signal_handler(SIGTERM, handle_quit_signal, "sigterm", SA_RESTART)) {
        return false;
    }
    if (!register_signal_handler(SIGALRM, handle_alarm_signal, "sigalrm", 0)) {
        return false;
    }
    if (!register_signal_handler(SIGUSR1, handle_ultimate_signal, "sigusr1", SA_RESTART)) {
        return false;
    }
    if (!register_signal_handler(SIGUSR2, handle_stun_signal, "sigusr2", SA_RESTART)) {
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
    wait_for_hip_party_size();
    if (!initialize_seeded_stats()) {
        return false;
    }
    return true;
}

void parse_arguments(int argc, char **argv) {
    (void)argc;
    (void)argv;
    active_roll_number = default_roll_number;
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
