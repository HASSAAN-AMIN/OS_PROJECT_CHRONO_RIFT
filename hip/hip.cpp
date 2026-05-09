#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <ncurses.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../gamestate.h"

using namespace std;

const char *shared_memory_name = "/chrono_rift_game_state";
const useconds_t frame_sleep_us = 1000000 / 60;
const useconds_t input_sleep_us = 16000;

const int pair_default = 1;
const int pair_border = 2;
const int pair_active = 3;
const int pair_dead = 4;
const int pair_hp_good = 5;
const int pair_hp_mid = 6;
const int pair_hp_low = 7;
const int pair_hp_critical = 8;
const int pair_stamina = 9;
const int pair_stamina_full = 10;
const int pair_title = 11;
const int pair_command = 12;
const int pair_status_ok = 13;
const int pair_status_warn = 14;
const int pair_inv_empty = 15;
const int pair_inv_splinter = 16;
const int pair_inv_venom = 17;
const int pair_inv_obsidian = 18;
const int pair_inv_frost = 19;
const int pair_inv_thunder = 20;
const int pair_inv_iron = 21;
const int pair_inv_solar = 22;
const int pair_inv_lunar = 23;
const int pair_inv_eclipse = 24;
const int pair_player_panel = 25;
const int pair_arena_panel = 26;
const int pair_enemy_panel = 27;
const int pair_log = 28;
const int pair_artifact = 29;

int shared_memory_fd = -1;
game_state *shared_state = nullptr;
volatile sig_atomic_t running = 1;
volatile sig_atomic_t resize_pending = 0;

pthread_t render_thread;
pthread_t input_thread;
pthread_mutex_t ui_lock;

char action_log_history[128][256];
int action_log_start = 0;
int action_log_count = 0;
char last_action_log_seen[256];

struct entity_snapshot {
    int hp;
    int max_hp;
    int stamina;
    int max_stamina;
    int damage;
    time_t stun_end;
    bool active;
    bool dead;
    bool current_turn;
    int inventory[game_state::inventory_slots];
};

struct ui_snapshot {
    entity_snapshot players[game_state::max_players];
    entity_snapshot enemies[game_state::max_enemies];
    int active_player_count;
    int active_enemy_count;
    int active_player_index;
    int target_enemy_index;
    int solar_core_holder;
    int lunar_blade_holder;
    int eclipse_relic_holder;
    int current_dropped_weapon;
    int enemy_kills;
    int total_kills;
    int outcome;
    int roll_number;
    pid_t arbiter_pid;
    pid_t asp_pid;
    pid_t hip_pid;
    char action_log[256];
    char last_event[128];
};

void print_errno(const char *action) {
    fprintf(stderr, "%s: %s\n", action, strerror(errno));
}

void pause_us(useconds_t us) {
    while (usleep(us) == -1 && errno == EINTR) {
        if (!running) {
            return;
        }
    }
}

bool lock_memory() {
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

bool unlock_memory() {
    if (sem_post(&shared_state->state_lock) != 0) {
        print_errno("sem_post state_lock failed");
        return false;
    }
    return true;
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
        default: return "empty";
    }
}

char weapon_letter(int weapon_id) {
    switch (weapon_id) {
        case game_state::splinter_stick_id: return 's';
        case game_state::venom_dagger_id: return 'v';
        case game_state::obsidian_axe_id: return 'o';
        case game_state::frostbow_id: return 'f';
        case game_state::thunderstaff_id: return 't';
        case game_state::iron_halberd_id: return 'i';
        case game_state::solar_core_id: return 'c';
        case game_state::lunar_blade_id: return 'l';
        case game_state::eclipse_relic_id: return 'e';
        default: return ' ';
    }
}

int weapon_color_pair(int weapon_id) {
    switch (weapon_id) {
        case game_state::splinter_stick_id: return pair_inv_splinter;
        case game_state::venom_dagger_id: return pair_inv_venom;
        case game_state::obsidian_axe_id: return pair_inv_obsidian;
        case game_state::frostbow_id: return pair_inv_frost;
        case game_state::thunderstaff_id: return pair_inv_thunder;
        case game_state::iron_halberd_id: return pair_inv_iron;
        case game_state::solar_core_id: return pair_inv_solar;
        case game_state::lunar_blade_id: return pair_inv_lunar;
        case game_state::eclipse_relic_id: return pair_inv_eclipse;
        default: return pair_inv_empty;
    }
}

void add_log_line(const char *line) {
    if (line == nullptr || line[0] == '\0') {
        return;
    }
    int write_index = (action_log_start + action_log_count) % 128;
    snprintf(action_log_history[write_index], sizeof(action_log_history[write_index]), "%s", line);
    if (action_log_count < 128) {
        action_log_count++;
    } else {
        action_log_start = (action_log_start + 1) % 128;
    }
}

void update_log_history_from_shared(const char *line) {
    if (line == nullptr) {
        return;
    }
    if (strcmp(last_action_log_seen, line) != 0) {
        snprintf(last_action_log_seen, sizeof(last_action_log_seen), "%s", line);
        add_log_line(line);
    }
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

int find_best_weapon_locked(int player_index) {
    int best_weapon = 0;
    int best_damage = 0;
    int s = 0;
    while (s < game_state::inventory_slots) {
        int w = shared_state->player_primary_inventory[player_index][s];
        if (w == 0) {
            s++;
            continue;
        }
        int dmg = weapon_damage_value(w);
        if (dmg > best_damage) {
            best_damage = dmg;
            best_weapon = w;
        }
        int size = weapon_slot_size(w);
        if (size <= 0) {
            size = 1;
        }
        s += size;
    }
    return best_weapon;
}

bool inventory_contains_locked(int player_index, int weapon_id) {
    for (int s = 0; s < game_state::inventory_slots; ++s) {
        if (shared_state->player_primary_inventory[player_index][s] == weapon_id) {
            return true;
        }
    }
    return false;
}

int find_contiguous_free_slots(int *arr, int needed) {
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

void fill_weapon_slots(int *arr, int start, int size, int weapon_id) {
    for (int i = start; i < start + size; ++i) {
        arr[i] = weapon_id;
    }
}

void clear_slots(int *arr, int start, int size) {
    for (int i = start; i < start + size; ++i) {
        arr[i] = 0;
    }
}

int find_first_weapon_run(int *arr, int *run_size) {
    for (int i = 0; i < game_state::inventory_slots; ++i) {
        if (arr[i] == 0) {
            continue;
        }
        int size = weapon_slot_size(arr[i]);
        if (size <= 0) {
            size = 1;
        }
        *run_size = size;
        return i;
    }
    return -1;
}

int allocate_weapon_iterative_locked(int player_index, int weapon_id) {
    int size_needed = weapon_slot_size(weapon_id);
    if (size_needed <= 0) {
        return -1;
    }
    int *primary = shared_state->player_primary_inventory[player_index];
    int *storage = shared_state->long_term_storage[player_index];
    int backup_primary[game_state::inventory_slots];
    int backup_storage[game_state::inventory_slots];
    for (int i = 0; i < game_state::inventory_slots; ++i) {
        backup_primary[i] = primary[i];
        backup_storage[i] = storage[i];
    }
    int guard = game_state::inventory_slots + 2;
    while (guard-- > 0) {
        int pos = find_contiguous_free_slots(primary, size_needed);
        if (pos >= 0) {
            fill_weapon_slots(primary, pos, size_needed, weapon_id);
            return pos;
        }
        int evict_size = 0;
        int evict_pos = find_first_weapon_run(primary, &evict_size);
        if (evict_pos < 0) {
            break;
        }
        int evict_weapon = primary[evict_pos];
        int storage_pos = find_contiguous_free_slots(storage, evict_size);
        if (storage_pos < 0) {
            break;
        }
        fill_weapon_slots(storage, storage_pos, evict_size, evict_weapon);
        clear_slots(primary, evict_pos, evict_size);
    }
    for (int i = 0; i < game_state::inventory_slots; ++i) {
        primary[i] = backup_primary[i];
        storage[i] = backup_storage[i];
    }
    return -1;
}

void append_action_log_locked(const char *line) {
    snprintf(shared_state->action_log, sizeof(shared_state->action_log), "%s", line);
    update_log_history_from_shared(shared_state->action_log);
}

void perform_strike() {
    if (!lock_memory()) {
        return;
    }
    int p = shared_state->active_player_index;
    if (p < 0 || p >= shared_state->active_player_count || shared_state->player_hp[p] <= 0) {
        unlock_memory();
        return;
    }
    if (shared_state->player_stamina[p] < game_state::player_max_stamina) {
        append_action_log_locked("player strike denied: stamina not full");
        unlock_memory();
        return;
    }
    int enemy_id = find_first_living_enemy_locked();
    if (enemy_id < 0) {
        append_action_log_locked("player strike denied: no enemy alive");
        unlock_memory();
        return;
    }
    int weapon_id = find_best_weapon_locked(p);
    int damage = shared_state->player_damage[p];
    if (weapon_id != 0) {
        damage = weapon_damage_value(weapon_id);
    }
    int next_hp = shared_state->enemy_hp[enemy_id] - damage;
    if (next_hp < 0) {
        next_hp = 0;
    }
    shared_state->enemy_hp[enemy_id] = next_hp;
    shared_state->player_stamina[p] = 0;
    char line[256];
    snprintf(line, sizeof(line), "enemy %d strike: hit player target by player %d for %d damage", enemy_id + 1, p + 1, damage);
    append_action_log_locked(line);
    if (next_hp <= 0) {
        int drop_id = game_state::iron_halberd_id;
        if (shared_state->solar_core_holder < 0) {
            drop_id = game_state::solar_core_id;
        } else if (shared_state->lunar_blade_holder < 0) {
            drop_id = game_state::lunar_blade_id;
        }
        shared_state->current_dropped_weapon = drop_id;
        snprintf(line, sizeof(line), "enemy %d died: dropped %s", enemy_id + 1, weapon_name(drop_id));
        append_action_log_locked(line);
    }
    unlock_memory();
}

void perform_exhaust() {
    if (!lock_memory()) {
        return;
    }
    int p = shared_state->active_player_index;
    if (p < 0 || p >= shared_state->active_player_count || shared_state->player_hp[p] <= 0) {
        unlock_memory();
        return;
    }
    if (shared_state->player_stamina[p] < game_state::player_max_stamina) {
        append_action_log_locked("player exhaust denied: stamina not full");
        unlock_memory();
        return;
    }
    int enemy_id = find_first_living_enemy_locked();
    if (enemy_id < 0) {
        append_action_log_locked("player exhaust denied: no enemy alive");
        unlock_memory();
        return;
    }
    int damage = shared_state->player_damage[p];
    int next_stamina = shared_state->enemy_stamina[enemy_id] - damage;
    if (next_stamina < 0) {
        next_stamina = 0;
    }
    shared_state->enemy_stamina[enemy_id] = next_stamina;
    shared_state->player_stamina[p] = 0;
    char line[256];
    snprintf(line, sizeof(line), "enemy %d exhaust: player %d drained enemy stamina by %d", enemy_id + 1, p + 1, damage);
    append_action_log_locked(line);
    unlock_memory();
}

void perform_heal() {
    if (!lock_memory()) {
        return;
    }
    int p = shared_state->active_player_index;
    if (p < 0 || p >= shared_state->active_player_count || shared_state->player_hp[p] <= 0) {
        unlock_memory();
        return;
    }
    if (shared_state->player_stamina[p] < game_state::player_max_stamina) {
        append_action_log_locked("player heal denied: stamina not full");
        unlock_memory();
        return;
    }
    int max_hp = shared_state->player_max_hp[p];
    int heal = max_hp / 10;
    if (heal < 1) {
        heal = 1;
    }
    int hp = shared_state->player_hp[p] + heal;
    if (hp > max_hp) {
        hp = max_hp;
    }
    shared_state->player_hp[p] = hp;
    shared_state->player_stamina[p] = 0;
    char line[256];
    snprintf(line, sizeof(line), "player %d heal: restored %d hp", p + 1, heal);
    append_action_log_locked(line);
    unlock_memory();
}

void perform_pickup() {
    if (!lock_memory()) {
        return;
    }
    int p = shared_state->active_player_index;
    if (p < 0 || p >= shared_state->active_player_count || shared_state->player_hp[p] <= 0) {
        unlock_memory();
        return;
    }
    int weapon_id = shared_state->current_dropped_weapon;
    if (weapon_id == 0) {
        append_action_log_locked("pickup ignored: no dropped weapon");
        unlock_memory();
        return;
    }
    int pos = allocate_weapon_iterative_locked(p, weapon_id);
    if (pos < 0) {
        append_action_log_locked("pickup denied: no contiguous inventory space");
        unlock_memory();
        return;
    }
    shared_state->current_dropped_weapon = 0;
    if (weapon_id == game_state::solar_core_id) {
        shared_state->solar_core_holder = p;
    }
    if (weapon_id == game_state::lunar_blade_id) {
        shared_state->lunar_blade_holder = p;
    }
    if (weapon_id == game_state::eclipse_relic_id) {
        shared_state->eclipse_relic_holder = p;
    }
    char line[256];
    snprintf(line, sizeof(line), "player %d pickup: acquired %s at slot %d", p + 1, weapon_name(weapon_id), pos + 1);
    append_action_log_locked(line);
    unlock_memory();
}

void perform_ultimate() {
    if (!lock_memory()) {
        return;
    }
    int p = shared_state->active_player_index;
    if (p < 0 || p >= shared_state->active_player_count || shared_state->player_hp[p] <= 0) {
        unlock_memory();
        return;
    }
    if (shared_state->player_stamina[p] < game_state::player_max_stamina) {
        append_action_log_locked("ultimate denied: stamina not full");
        unlock_memory();
        return;
    }
    bool has_solar = inventory_contains_locked(p, game_state::solar_core_id);
    bool has_lunar = inventory_contains_locked(p, game_state::lunar_blade_id);
    if (!has_solar || !has_lunar) {
        append_action_log_locked("ultimate denied: solar core and lunar blade required");
        unlock_memory();
        return;
    }
    shared_state->player_stamina[p] = 0;
    pid_t asp_pid = shared_state->asp_pid;
    pid_t arbiter_pid = shared_state->arbiter_pid;
    append_action_log_locked("ultimate triggered: asp frozen for 10 seconds");
    unlock_memory();
    if (asp_pid > 0) {
        kill(asp_pid, SIGSTOP);
    }
    if (arbiter_pid > 0) {
        kill(arbiter_pid, SIGUSR1);
    }
}

void perform_stun() {
    if (!lock_memory()) {
        return;
    }
    int p = shared_state->active_player_index;
    if (p < 0 || p >= shared_state->active_player_count || shared_state->player_hp[p] <= 0) {
        unlock_memory();
        return;
    }
    if (shared_state->player_stamina[p] < game_state::player_max_stamina) {
        append_action_log_locked("stun denied: stamina not full");
        unlock_memory();
        return;
    }
    int enemy_id = find_first_living_enemy_locked();
    if (enemy_id < 0) {
        append_action_log_locked("stun denied: no enemy alive");
        unlock_memory();
        return;
    }
    int damage = shared_state->player_damage[p];
    int next_hp = shared_state->enemy_hp[enemy_id] - damage;
    if (next_hp < 0) {
        next_hp = 0;
    }
    shared_state->enemy_hp[enemy_id] = next_hp;
    shared_state->stun_end_time[enemy_id] = time(nullptr) + 3;
    shared_state->player_stamina[p] = 0;
    pid_t asp_pid = shared_state->asp_pid;
    pid_t arbiter_pid = shared_state->arbiter_pid;
    char line[256];
    snprintf(line, sizeof(line), "enemy %d strike: hit player %d for %d damage", enemy_id + 1, p + 1, damage);
    append_action_log_locked(line);
    unlock_memory();
    if (asp_pid > 0) {
        kill(asp_pid, SIGSTOP);
    }
    if (arbiter_pid > 0) {
        kill(arbiter_pid, SIGUSR2);
    }
}

void request_quit() {
    pid_t arbiter_pid = 0;
    pid_t asp_pid = 0;
    if (lock_memory()) {
        arbiter_pid = shared_state->arbiter_pid;
        asp_pid = shared_state->asp_pid;
        shared_state->outcome = game_state::outcome_quit;
        unlock_memory();
    }
    if (asp_pid > 0) {
        kill(asp_pid, SIGTERM);
    }
    if (arbiter_pid > 0) {
        kill(arbiter_pid, SIGTERM);
    }
    running = 0;
}

void handle_input_key(int key) {
    if (key == '1') {
        perform_strike();
    } else if (key == '2') {
        perform_exhaust();
    } else if (key == '3') {
        perform_heal();
    } else if (key == '5') {
        perform_pickup();
    } else if (key == '6') {
        perform_ultimate();
    } else if (key == '7') {
        perform_stun();
    } else if (key == 'q' || key == 'Q') {
        request_quit();
    }
}

void take_snapshot(ui_snapshot *snap) {
    memset(snap, 0, sizeof(*snap));
    if (!lock_memory()) {
        return;
    }
    snap->active_player_count = shared_state->active_player_count;
    snap->active_enemy_count = shared_state->active_enemy_count;
    if (snap->active_player_count < 0) {
        snap->active_player_count = 0;
    }
    if (snap->active_player_count > game_state::max_players) {
        snap->active_player_count = game_state::max_players;
    }
    if (snap->active_enemy_count < 0) {
        snap->active_enemy_count = 0;
    }
    if (snap->active_enemy_count > game_state::max_enemies) {
        snap->active_enemy_count = game_state::max_enemies;
    }
    snap->active_player_index = shared_state->active_player_index;
    snap->solar_core_holder = shared_state->solar_core_holder;
    snap->lunar_blade_holder = shared_state->lunar_blade_holder;
    snap->eclipse_relic_holder = shared_state->eclipse_relic_holder;
    snap->current_dropped_weapon = shared_state->current_dropped_weapon;
    snap->enemy_kills = shared_state->enemy_kills;
    snap->total_kills = shared_state->total_kills;
    snap->outcome = shared_state->outcome;
    snap->roll_number = shared_state->roll_number;
    snap->arbiter_pid = shared_state->arbiter_pid;
    snap->asp_pid = shared_state->asp_pid;
    snap->hip_pid = shared_state->hip_pid;
    snprintf(snap->action_log, sizeof(snap->action_log), "%s", shared_state->action_log);
    snprintf(snap->last_event, sizeof(snap->last_event), "%s", shared_state->last_event);

    int target_enemy = -1;
    for (int i = 0; i < snap->active_enemy_count; ++i) {
        if (shared_state->enemy_hp[i] > 0) {
            target_enemy = i;
            break;
        }
    }
    snap->target_enemy_index = target_enemy;

    for (int i = 0; i < game_state::max_players; ++i) {
        entity_snapshot *e = &snap->players[i];
        e->active = i < snap->active_player_count;
        e->hp = shared_state->player_hp[i];
        e->max_hp = shared_state->player_max_hp[i];
        e->stamina = shared_state->player_stamina[i];
        e->max_stamina = game_state::player_max_stamina;
        e->damage = shared_state->player_damage[i];
        e->stun_end = shared_state->player_stun_end_time[i];
        e->dead = e->active && e->hp <= 0;
        e->current_turn = i == snap->active_player_index;
        for (int s = 0; s < game_state::inventory_slots; ++s) {
            e->inventory[s] = shared_state->player_primary_inventory[i][s];
        }
    }

    for (int i = 0; i < game_state::max_enemies; ++i) {
        entity_snapshot *e = &snap->enemies[i];
        e->active = i < snap->active_enemy_count;
        e->hp = shared_state->enemy_hp[i];
        e->max_hp = shared_state->enemy_max_hp[i];
        e->stamina = shared_state->enemy_stamina[i];
        e->max_stamina = game_state::enemy_max_stamina;
        e->damage = shared_state->enemy_damage[i];
        e->stun_end = shared_state->stun_end_time[i];
        e->dead = e->active && e->hp <= 0;
        e->current_turn = i == snap->target_enemy_index;
        for (int s = 0; s < game_state::inventory_slots; ++s) {
            e->inventory[s] = 0;
        }
        if (e->active) {
            int slot = 0;
            if (snap->solar_core_holder == game_state::max_players + i && slot + game_state::solar_core_slots <= game_state::inventory_slots) {
                for (int k = 0; k < game_state::solar_core_slots; ++k) {
                    e->inventory[slot + k] = game_state::solar_core_id;
                }
                slot += game_state::solar_core_slots;
            }
            if (snap->lunar_blade_holder == game_state::max_players + i && slot + game_state::lunar_blade_slots <= game_state::inventory_slots) {
                for (int k = 0; k < game_state::lunar_blade_slots; ++k) {
                    e->inventory[slot + k] = game_state::lunar_blade_id;
                }
                slot += game_state::lunar_blade_slots;
            }
            if (snap->eclipse_relic_holder == game_state::max_players + i && slot + game_state::eclipse_relic_slots <= game_state::inventory_slots) {
                for (int k = 0; k < game_state::eclipse_relic_slots; ++k) {
                    e->inventory[slot + k] = game_state::eclipse_relic_id;
                }
            }
        }
    }
    update_log_history_from_shared(snap->action_log);
    unlock_memory();
}

int hp_pair_from_ratio(double ratio) {
    if (ratio < 0.2) {
        return pair_hp_critical;
    }
    if (ratio < 0.45) {
        return pair_hp_low;
    }
    if (ratio < 0.75) {
        return pair_hp_mid;
    }
    return pair_hp_good;
}

void draw_window_box(WINDOW *win, const char *title, int pair) {
    wattron(win, COLOR_PAIR(pair) | A_BOLD);
    wborder(win, ACS_VLINE, ACS_VLINE, ACS_HLINE, ACS_HLINE, ACS_ULCORNER, ACS_URCORNER, ACS_LLCORNER, ACS_LRCORNER);
    if (title != nullptr) {
        int max_x = getmaxx(win);
        int title_len = (int)strlen(title);
        int tx = 2;
        if (tx + title_len + 2 < max_x - 1) {
            mvwaddch(win, 0, tx - 1, ' ');
            mvwprintw(win, 0, tx, "%s", title);
            mvwaddch(win, 0, tx + title_len, ' ');
        }
    }
    wattroff(win, COLOR_PAIR(pair) | A_BOLD);
}

void draw_bar_line(WINDOW *win, int row, int col, int width, const char *label, int value, int max_value, int pair, bool critical_blink) {
    if (max_value <= 0) {
        max_value = 1;
    }
    if (value < 0) {
        value = 0;
    }
    if (value > max_value) {
        value = max_value;
    }
    int bar_width = width - 14;
    if (bar_width < 6) {
        bar_width = 6;
    }
    double ratio = (double)value / (double)max_value;
    int fill_count = (int)(ratio * bar_width);
    if (fill_count > bar_width) {
        fill_count = bar_width;
    }
    wattron(win, COLOR_PAIR(pair_default) | A_BOLD);
    mvwprintw(win, row, col, "%s", label);
    wattroff(win, COLOR_PAIR(pair_default) | A_BOLD);
    mvwaddch(win, row, col + 3, '[');
    int attrs = COLOR_PAIR(pair) | A_BOLD;
    if (critical_blink) {
        attrs |= A_BLINK;
    }
    wattron(win, attrs);
    for (int i = 0; i < fill_count; ++i) {
        mvwaddch(win, row, col + 4 + i, ACS_CKBOARD);
    }
    wattroff(win, attrs);
    wattron(win, COLOR_PAIR(pair_inv_empty));
    for (int i = fill_count; i < bar_width; ++i) {
        mvwaddch(win, row, col + 4 + i, ACS_BOARD);
    }
    wattroff(win, COLOR_PAIR(pair_inv_empty));
    mvwaddch(win, row, col + 4 + bar_width, ']');
    wattron(win, COLOR_PAIR(pair_default));
    mvwprintw(win, row, col + 6 + bar_width, "%d", value);
    wattroff(win, COLOR_PAIR(pair_default));
}

void draw_inventory_tetris(WINDOW *win, const int *inventory, bool dead_state) {
    int width = getmaxx(win) - 2;
    int y = 1;
    int x = 1;
    if (width < game_state::inventory_slots) {
        return;
    }
    int slot_width = width / game_state::inventory_slots;
    if (slot_width < 1) {
        slot_width = 1;
    }
    if (slot_width > 2) {
        slot_width = 2;
    }
    int total_w = slot_width * game_state::inventory_slots;
    int start_x = x + (width - total_w) / 2;
    int s = 0;
    while (s < game_state::inventory_slots) {
        int weapon_id = inventory[s];
        int run = 1;
        while (s + run < game_state::inventory_slots && inventory[s + run] == weapon_id) {
            run++;
        }
        int pair = weapon_id == 0 ? pair_inv_empty : weapon_color_pair(weapon_id);
        if (dead_state) {
            pair = pair_dead;
        }
        wattron(win, COLOR_PAIR(pair) | A_REVERSE | A_BOLD);
        for (int k = 0; k < run * slot_width; ++k) {
            mvwaddch(win, y, start_x + s * slot_width + k, ' ');
        }
        if (weapon_id != 0) {
            int cx = start_x + s * slot_width + (run * slot_width) / 2;
            mvwaddch(win, y, cx, weapon_letter(weapon_id));
        }
        wattroff(win, COLOR_PAIR(pair) | A_REVERSE | A_BOLD);
        s += run;
    }
}

void draw_stats_box(WINDOW *stats_win, const entity_snapshot *entity, bool player_view, int border_pair) {
    draw_window_box(stats_win, "stats", border_pair);
    int inner_width = getmaxx(stats_win) - 2;
    int hp_pair = hp_pair_from_ratio(entity->max_hp > 0 ? (double)entity->hp / (double)entity->max_hp : 0.0);
    bool critical = player_view && entity->max_hp > 0 && ((double)entity->hp / (double)entity->max_hp) < 0.2;
    if (entity->dead) {
        hp_pair = pair_dead;
        critical = false;
    }
    draw_bar_line(stats_win, 1, 1, inner_width, "hp", entity->hp, entity->max_hp, hp_pair, critical);
    int stamina_pair = entity->stamina >= entity->max_stamina ? pair_stamina_full : pair_stamina;
    if (entity->dead) {
        stamina_pair = pair_dead;
    }
    draw_bar_line(stats_win, 2, 1, inner_width, "sp", entity->stamina, entity->max_stamina, stamina_pair, false);
    wattron(stats_win, COLOR_PAIR(entity->dead ? pair_dead : pair_default) | A_BOLD);
    if (entity->dead) {
        mvwprintw(stats_win, 3, 1, "state dead");
    } else if (entity->current_turn) {
        mvwprintw(stats_win, 3, 1, "state turn");
    } else {
        mvwprintw(stats_win, 3, 1, "state ready");
    }
    wattroff(stats_win, COLOR_PAIR(entity->dead ? pair_dead : pair_default) | A_BOLD);
}

void draw_entity_box(WINDOW *container, const char *title, const entity_snapshot *entity, bool player_view) {
    int border_pair = pair_border;
    if (entity->dead) {
        border_pair = pair_dead;
    } else if (entity->current_turn) {
        border_pair = pair_active;
    }
    draw_window_box(container, title, border_pair);
    int h = getmaxy(container);
    int w = getmaxx(container);
    int stats_h = h - 4;
    if (stats_h < 5) {
        stats_h = 5;
    }
    int inv_h = h - stats_h - 1;
    if (inv_h < 3) {
        inv_h = 3;
        stats_h = h - inv_h - 1;
    }
    if (stats_h < 5) {
        stats_h = 5;
    }
    WINDOW *stats_win = derwin(container, stats_h, w - 2, 1, 1);
    WINDOW *inv_win = derwin(container, inv_h, w - 2, 1 + stats_h, 1);
    draw_stats_box(stats_win, entity, player_view, border_pair);
    draw_window_box(inv_win, "inventroy tetris", border_pair);
    draw_inventory_tetris(inv_win, entity->inventory, entity->dead);
    wrefresh(stats_win);
    wrefresh(inv_win);
    delwin(stats_win);
    delwin(inv_win);
}

void draw_player_squad(WINDOW *left_win, const ui_snapshot *snap) {
    draw_window_box(left_win, "player_squad", pair_player_panel);
    int active = snap->active_player_count;
    if (active < 1) {
        active = 1;
    }
    int inner_h = getmaxy(left_win) - 2;
    int inner_w = getmaxx(left_win) - 2;
    int box_h = inner_h / active;
    if (box_h < 9) {
        box_h = 9;
    }
    int y = 1;
    for (int i = 0; i < active && i < game_state::max_players; ++i) {
        if (y + box_h > getmaxy(left_win) - 1) {
            box_h = getmaxy(left_win) - 1 - y;
        }
        if (box_h < 8) {
            break;
        }
        WINDOW *pwin = derwin(left_win, box_h, inner_w, y, 1);
        char title[64];
        snprintf(title, sizeof(title), "player %d", i + 1);
        draw_entity_box(pwin, title, &snap->players[i], true);
        wrefresh(pwin);
        delwin(pwin);
        y += box_h;
    }
}

void draw_enemy_forces(WINDOW *right_win, const ui_snapshot *snap) {
    draw_window_box(right_win, "enemy_forces", pair_enemy_panel);
    int active = snap->active_enemy_count;
    if (active < 1) {
        active = 1;
    }
    int inner_h = getmaxy(right_win) - 2;
    int inner_w = getmaxx(right_win) - 2;
    int cols = 1;
    if (active > 3 && inner_w >= 54) {
        cols = 2;
    }
    int rows = (active + cols - 1) / cols;
    if (rows < 1) {
        rows = 1;
    }
    int cell_h = inner_h / rows;
    int cell_w = inner_w / cols;
    if (cell_h < 8) {
        cell_h = 8;
    }
    if (cell_w < 24) {
        cell_w = 24;
    }
    for (int i = 0; i < active && i < game_state::max_enemies; ++i) {
        int r = i / cols;
        int c = i % cols;
        int y = 1 + r * cell_h;
        int x = 1 + c * cell_w;
        int h = cell_h;
        int w = cell_w;
        if (y + h > getmaxy(right_win) - 1) {
            h = getmaxy(right_win) - 1 - y;
        }
        if (x + w > getmaxx(right_win) - 1) {
            w = getmaxx(right_win) - 1 - x;
        }
        if (h < 8 || w < 24) {
            continue;
        }
        WINDOW *ewin = derwin(right_win, h, w, y, x);
        char title[64];
        snprintf(title, sizeof(title), "enemy %d", i + 1);
        draw_entity_box(ewin, title, &snap->enemies[i], false);
        wrefresh(ewin);
        delwin(ewin);
    }
}

void draw_action_log_box(WINDOW *log_win) {
    draw_window_box(log_win, "action_log", pair_arena_panel);
    int lines = getmaxy(log_win) - 2;
    int width = getmaxx(log_win) - 2;
    int start = action_log_count - lines;
    if (start < 0) {
        start = 0;
    }
    wattron(log_win, COLOR_PAIR(pair_log));
    for (int i = 0; i < lines; ++i) {
        int idx = start + i;
        if (idx >= action_log_count) {
            break;
        }
        int real = (action_log_start + idx) % 128;
        mvwprintw(log_win, 1 + i, 1, "%.*s", width, action_log_history[real]);
    }
    wattroff(log_win, COLOR_PAIR(pair_log));
}

void holder_text(char *buf, int buf_sz, int holder) {
    if (holder < 0) {
        snprintf(buf, buf_sz, "ground");
    } else if (holder < game_state::max_players) {
        snprintf(buf, buf_sz, "player %d", holder + 1);
    } else {
        snprintf(buf, buf_sz, "enemy %d", holder - game_state::max_players + 1);
    }
}

void draw_system_status_box(WINDOW *status_win, const ui_snapshot *snap) {
    draw_window_box(status_win, "system_status", pair_arena_panel);
    wattron(status_win, COLOR_PAIR(pair_default) | A_BOLD);
    mvwprintw(status_win, 1, 1, "roll %d", snap->roll_number);
    mvwprintw(status_win, 2, 1, "pids a:%d e:%d h:%d", (int)snap->arbiter_pid, (int)snap->asp_pid, (int)snap->hip_pid);
    mvwprintw(status_win, 3, 1, "kills %d total %d", snap->enemy_kills, snap->total_kills);
    if (snap->outcome == game_state::outcome_win) {
        wattron(status_win, COLOR_PAIR(pair_status_ok) | A_BOLD);
        mvwprintw(status_win, 4, 1, "state victory");
        wattroff(status_win, COLOR_PAIR(pair_status_ok) | A_BOLD);
    } else if (snap->outcome == game_state::outcome_lose) {
        wattron(status_win, COLOR_PAIR(pair_status_warn) | A_BOLD);
        mvwprintw(status_win, 4, 1, "state defeat");
        wattroff(status_win, COLOR_PAIR(pair_status_warn) | A_BOLD);
    } else {
        mvwprintw(status_win, 4, 1, "state live");
    }
    wattroff(status_win, COLOR_PAIR(pair_default) | A_BOLD);
}

void draw_artifact_tracker_box(WINDOW *artifact_win, const ui_snapshot *snap) {
    draw_window_box(artifact_win, "artifact_tracker", pair_arena_panel);
    char h1[64];
    char h2[64];
    char h3[64];
    holder_text(h1, sizeof(h1), snap->solar_core_holder);
    holder_text(h2, sizeof(h2), snap->lunar_blade_holder);
    holder_text(h3, sizeof(h3), snap->eclipse_relic_holder);
    wattron(artifact_win, COLOR_PAIR(pair_artifact) | A_BOLD);
    mvwprintw(artifact_win, 1, 1, "solar core : %s", h1);
    mvwprintw(artifact_win, 2, 1, "lunar blade: %s", h2);
    mvwprintw(artifact_win, 3, 1, "eclipse    : %s", h3);
    if (snap->current_dropped_weapon != 0) {
        mvwprintw(artifact_win, 4, 1, "drop       : %s", weapon_name(snap->current_dropped_weapon));
    } else {
        mvwprintw(artifact_win, 4, 1, "drop       : none");
    }
    wattroff(artifact_win, COLOR_PAIR(pair_artifact) | A_BOLD);
}

void draw_combat_arena(WINDOW *center_win, const ui_snapshot *snap) {
    draw_window_box(center_win, "combat_arena", pair_arena_panel);
    int inner_h = getmaxy(center_win) - 2;
    int inner_w = getmaxx(center_win) - 2;
    int log_h = (inner_h * 60) / 100;
    if (log_h < 8) {
        log_h = 8;
    }
    if (log_h > inner_h - 8) {
        log_h = inner_h - 8;
    }
    int bottom_h = inner_h - log_h;
    int status_h = bottom_h / 2;
    if (status_h < 5) {
        status_h = 5;
    }
    int artifact_h = bottom_h - status_h;
    if (artifact_h < 5) {
        artifact_h = 5;
        status_h = bottom_h - artifact_h;
    }
    WINDOW *log_win = derwin(center_win, log_h, inner_w, 1, 1);
    WINDOW *status_win = derwin(center_win, status_h, inner_w, 1 + log_h, 1);
    WINDOW *artifact_win = derwin(center_win, artifact_h, inner_w, 1 + log_h + status_h, 1);
    draw_action_log_box(log_win);
    draw_system_status_box(status_win, snap);
    draw_artifact_tracker_box(artifact_win, snap);
    wrefresh(log_win);
    wrefresh(status_win);
    wrefresh(artifact_win);
    delwin(log_win);
    delwin(status_win);
    delwin(artifact_win);
}

void draw_bottom_command_bar(WINDOW *cmd_win) {
    int w = getmaxx(cmd_win);
    wattron(cmd_win, COLOR_PAIR(pair_command) | A_REVERSE | A_BOLD);
    for (int i = 0; i < w; ++i) {
        mvwaddch(cmd_win, 0, i, ' ');
    }
    const char *text = "[1]strike [2]exhaust [3]heal [5]pickup [6]ult [7]stun [q]quit";
    int len = (int)strlen(text);
    int x = (w - len) / 2;
    if (x < 0) {
        x = 0;
        len = w;
    }
    mvwprintw(cmd_win, 0, x, "%.*s", len, text);
    wattroff(cmd_win, COLOR_PAIR(pair_command) | A_REVERSE | A_BOLD);
}

void render_all(const ui_snapshot *snap) {
    int term_h = 0;
    int term_w = 0;
    getmaxyx(stdscr, term_h, term_w);
    if (resize_pending) {
        resize_pending = 0;
        clear();
        refresh();
    }
    erase();
    if (term_h < 20 || term_w < 90) {
        attron(COLOR_PAIR(pair_status_warn) | A_BOLD);
        mvprintw(0, 0, "terminal too small, need at least 90x20");
        attroff(COLOR_PAIR(pair_status_warn) | A_BOLD);
        refresh();
        return;
    }
    int command_h = 1;
    int main_h = term_h - command_h;
    int left_w = term_w * 30 / 100;
    int center_w = term_w * 40 / 100;
    int right_w = term_w - left_w - center_w;
    if (left_w < 28) {
        left_w = 28;
    }
    if (right_w < 30) {
        right_w = 30;
    }
    center_w = term_w - left_w - right_w;
    if (center_w < 30) {
        center_w = 30;
        left_w = (term_w - center_w) / 2;
        right_w = term_w - center_w - left_w;
    }

    WINDOW *left_win = newwin(main_h, left_w, 0, 0);
    WINDOW *center_win = newwin(main_h, center_w, 0, left_w);
    WINDOW *right_win = newwin(main_h, right_w, 0, left_w + center_w);
    WINDOW *cmd_win = newwin(command_h, term_w, term_h - command_h, 0);

    wbkgd(left_win, COLOR_PAIR(pair_player_panel));
    wbkgd(center_win, COLOR_PAIR(pair_arena_panel));
    wbkgd(right_win, COLOR_PAIR(pair_enemy_panel));
    wbkgd(cmd_win, COLOR_PAIR(pair_command));

    werase(left_win);
    werase(center_win);
    werase(right_win);
    werase(cmd_win);

    draw_player_squad(left_win, snap);
    draw_combat_arena(center_win, snap);
    draw_enemy_forces(right_win, snap);
    draw_bottom_command_bar(cmd_win);

    wrefresh(left_win);
    wrefresh(center_win);
    wrefresh(right_win);
    wrefresh(cmd_win);

    delwin(left_win);
    delwin(center_win);
    delwin(right_win);
    delwin(cmd_win);
}

void *render_loop(void *) {
    while (running) {
        ui_snapshot snap;
        take_snapshot(&snap);
        pthread_mutex_lock(&ui_lock);
        render_all(&snap);
        pthread_mutex_unlock(&ui_lock);
        if (snap.outcome != game_state::outcome_ongoing) {
            running = 0;
            break;
        }
        pause_us(frame_sleep_us);
    }
    return nullptr;
}

void *player_input_loop(void *) {
    while (running) {
        int key = ERR;
        pthread_mutex_lock(&ui_lock);
        key = getch();
        pthread_mutex_unlock(&ui_lock);
        if (key != ERR) {
            handle_input_key(key);
        }
        pause_us(input_sleep_us);
    }
    return nullptr;
}

void handle_resize_signal(int) {
    resize_pending = 1;
}

void handle_exit_signal(int) {
    running = 0;
}

bool register_signals() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_resize_signal;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGWINCH, &sa, nullptr) != 0) {
        print_errno("sigaction sigwinch failed");
        return false;
    }
    sa.sa_handler = handle_exit_signal;
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, nullptr) != 0) {
        print_errno("sigaction sigint failed");
        return false;
    }
    sa.sa_flags = 0;
    if (sigaction(SIGTERM, &sa, nullptr) != 0) {
        print_errno("sigaction sigterm failed");
        return false;
    }
    return true;
}

void init_colors() {
    init_pair(pair_default, COLOR_WHITE, COLOR_BLACK);
    init_pair(pair_border, COLOR_CYAN, COLOR_BLACK);
    init_pair(pair_active, COLOR_GREEN, COLOR_BLACK);
    init_pair(pair_dead, COLOR_RED, COLOR_BLACK);
    init_pair(pair_hp_good, COLOR_GREEN, COLOR_BLACK);
    init_pair(pair_hp_mid, COLOR_CYAN, COLOR_BLACK);
    init_pair(pair_hp_low, COLOR_YELLOW, COLOR_BLACK);
    init_pair(pair_hp_critical, COLOR_RED, COLOR_BLACK);
    init_pair(pair_stamina, COLOR_BLUE, COLOR_BLACK);
    init_pair(pair_stamina_full, COLOR_GREEN, COLOR_BLACK);
    init_pair(pair_title, COLOR_CYAN, COLOR_BLACK);
    init_pair(pair_command, COLOR_WHITE, COLOR_BLACK);
    init_pair(pair_status_ok, COLOR_GREEN, COLOR_BLACK);
    init_pair(pair_status_warn, COLOR_RED, COLOR_BLACK);
    init_pair(pair_inv_empty, COLOR_BLACK, COLOR_BLACK);
    init_pair(pair_inv_splinter, COLOR_GREEN, COLOR_BLACK);
    init_pair(pair_inv_venom, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(pair_inv_obsidian, COLOR_WHITE, COLOR_BLACK);
    init_pair(pair_inv_frost, COLOR_BLUE, COLOR_BLACK);
    init_pair(pair_inv_thunder, COLOR_YELLOW, COLOR_BLACK);
    init_pair(pair_inv_iron, COLOR_WHITE, COLOR_BLACK);
    init_pair(pair_inv_solar, COLOR_YELLOW, COLOR_BLACK);
    init_pair(pair_inv_lunar, COLOR_CYAN, COLOR_BLACK);
    init_pair(pair_inv_eclipse, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(pair_player_panel, COLOR_GREEN, COLOR_BLACK);
    init_pair(pair_arena_panel, COLOR_CYAN, COLOR_BLACK);
    init_pair(pair_enemy_panel, COLOR_BLUE, COLOR_BLACK);
    init_pair(pair_log, COLOR_WHITE, COLOR_BLACK);
    init_pair(pair_artifact, COLOR_YELLOW, COLOR_BLACK);
}

bool init_tui() {
    if (initscr() == nullptr) {
        fprintf(stderr, "initscr failed\n");
        return false;
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_colors();
    }
    bkgd(COLOR_PAIR(pair_default));
    return true;
}

void cleanup_tui_once() {
    endwin();
}

bool open_shared_memory() {
    shared_memory_fd = shm_open(shared_memory_name, O_RDWR, 0600);
    if (shared_memory_fd < 0) {
        print_errno("shm_open failed");
        return false;
    }
    return true;
}

bool map_shared_memory() {
    void *mapped = mmap(nullptr, sizeof(game_state), PROT_READ | PROT_WRITE, MAP_SHARED, shared_memory_fd, 0);
    if (mapped == MAP_FAILED) {
        print_errno("mmap failed");
        return false;
    }
    shared_state = static_cast<game_state *>(mapped);
    return true;
}

void cleanup_shared_memory() {
    if (shared_state != nullptr) {
        munmap(shared_state, sizeof(game_state));
        shared_state = nullptr;
    }
    if (shared_memory_fd >= 0) {
        close(shared_memory_fd);
        shared_memory_fd = -1;
    }
}

bool register_hip_pid() {
    if (!lock_memory()) {
        return false;
    }
    shared_state->hip_pid = getpid();
    unlock_memory();
    return true;
}

int prompt_party_size(int argc, char **argv) {
    int chosen = 0;
    if (argc >= 2) {
        chosen = atoi(argv[1]);
    }
    if (chosen < 1 || chosen > game_state::max_players) {
        const char *env_value = getenv("party_size");
        if (env_value != nullptr) {
            chosen = atoi(env_value);
        }
    }
    if (chosen < 1 || chosen > game_state::max_players) {
        char line[32];
        printf("select party size [1-4]: ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) != nullptr) {
            chosen = atoi(line);
        }
    }
    if (chosen < 1) {
        chosen = 1;
    }
    if (chosen > game_state::max_players) {
        chosen = game_state::max_players;
    }
    return chosen;
}

bool apply_party_size(int party_size) {
    if (!lock_memory()) {
        return false;
    }
    int speed = 100 / party_size;
    if (speed < 1) {
        speed = 1;
    }
    for (int i = 0; i < game_state::max_players; ++i) {
        if (i < party_size) {
            shared_state->player_speed[i] = speed;
            if (shared_state->player_hp[i] <= 0 && shared_state->player_max_hp[i] > 0) {
                shared_state->player_hp[i] = shared_state->player_max_hp[i];
            }
        } else {
            shared_state->player_hp[i] = 0;
            shared_state->player_max_hp[i] = 0;
            shared_state->player_speed[i] = 0;
            shared_state->player_stamina[i] = 0;
            for (int s = 0; s < game_state::inventory_slots; ++s) {
                shared_state->player_primary_inventory[i][s] = 0;
                shared_state->long_term_storage[i][s] = 0;
            }
        }
    }
    shared_state->active_player_count = party_size;
    if (shared_state->active_player_index < 0 || shared_state->active_player_index >= party_size) {
        shared_state->active_player_index = 0;
    }
    unlock_memory();
    return true;
}

bool spawn_threads() {
    if (pthread_create(&render_thread, nullptr, render_loop, nullptr) != 0) {
        print_errno("pthread_create render failed");
        return false;
    }
    if (pthread_create(&input_thread, nullptr, player_input_loop, nullptr) != 0) {
        print_errno("pthread_create input failed");
        running = 0;
        pthread_join(render_thread, nullptr);
        return false;
    }
    return true;
}

void join_threads() {
    pthread_join(render_thread, nullptr);
    pthread_join(input_thread, nullptr);
}

int main(int argc, char **argv) {
    memset(last_action_log_seen, 0, sizeof(last_action_log_seen));
    if (!register_signals()) {
        return 1;
    }
    if (pthread_mutex_init(&ui_lock, nullptr) != 0) {
        print_errno("pthread_mutex_init failed");
        return 1;
    }
    int party_size = prompt_party_size(argc, argv);
    if (!open_shared_memory()) {
        pthread_mutex_destroy(&ui_lock);
        return 1;
    }
    if (!map_shared_memory()) {
        cleanup_shared_memory();
        pthread_mutex_destroy(&ui_lock);
        return 1;
    }
    if (!register_hip_pid()) {
        cleanup_shared_memory();
        pthread_mutex_destroy(&ui_lock);
        return 1;
    }
    if (!apply_party_size(party_size)) {
        cleanup_shared_memory();
        pthread_mutex_destroy(&ui_lock);
        return 1;
    }
    if (!init_tui()) {
        cleanup_shared_memory();
        pthread_mutex_destroy(&ui_lock);
        return 1;
    }
    if (!spawn_threads()) {
        cleanup_tui_once();
        cleanup_shared_memory();
        pthread_mutex_destroy(&ui_lock);
        return 1;
    }
    join_threads();
    cleanup_tui_once();
    cleanup_shared_memory();
    pthread_mutex_destroy(&ui_lock);
    return 0;
}
