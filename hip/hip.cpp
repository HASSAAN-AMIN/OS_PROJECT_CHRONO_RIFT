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
const useconds_t player_idle_sleep_us = 60000;

const int pair_default = 1;
const int pair_border_normal = 2;
const int pair_border_active = 3;
const int pair_border_dead = 4;
const int pair_hp_high = 5;
const int pair_hp_med = 6;
const int pair_hp_low = 7;
const int pair_hp_critical = 8;
const int pair_stamina = 9;
const int pair_stamina_full = 10;
const int pair_log = 11;
const int pair_status_ok = 12;
const int pair_status_warn = 13;
const int pair_artifact_solar = 14;
const int pair_artifact_lunar = 15;
const int pair_artifact_eclipse = 16;
const int pair_artifact_ground = 17;
const int pair_command_bar = 18;
const int pair_inv_empty = 19;
const int pair_inv_splinter = 20;
const int pair_inv_venom = 21;
const int pair_inv_obsidian = 22;
const int pair_inv_frost = 23;
const int pair_inv_thunder = 24;
const int pair_inv_iron = 25;
const int pair_inv_solar = 26;
const int pair_inv_lunar = 27;
const int pair_inv_eclipse = 28;
const int pair_panel_title = 29;
const int pair_arena_title = 30;
const int pair_banner = 31;
const int pair_kill_counter = 32;
const int pair_overlay_win = 33;
const int pair_overlay_lose = 34;
const int pair_overlay_quit = 35;
const int pair_help = 36;
const int pair_canvas = 37;
const int pair_panel_fill = 38;
const int pair_top_hud = 39;

int shared_memory_fd = -1;
game_state *shared_state = nullptr;

volatile sig_atomic_t running = 1;
volatile sig_atomic_t resize_pending = 0;

pthread_mutex_t ncurses_lock;

struct player_input_buffer {
    int pending_key;
    pthread_mutex_t lock;
    pthread_cond_t cv;
};

player_input_buffer player_buffers[game_state::max_players];
pthread_t render_thread;
pthread_t input_dispatcher_thread;
pthread_t player_threads[game_state::max_players];
int player_thread_ids[game_state::max_players];
int chosen_party_size = 0;
int show_help_overlay = 0;
unsigned long render_frame_counter = 0;
time_t last_event_time = 0;
char last_event_label[64] = {0};

struct entity_snapshot {
    int hp;
    int max_hp;
    int stamina;
    int max_stamina;
    int damage;
    time_t stun_end;
    int inventory[game_state::inventory_slots];
    int storage[game_state::inventory_slots];
    bool active;
    bool dead;
    bool turn;
};

struct world_snapshot {
    entity_snapshot players[game_state::max_players];
    entity_snapshot enemies[game_state::max_enemies];
    int active_player_count;
    int active_enemy_count;
    int active_player_index;
    int solar_core_holder;
    int lunar_blade_holder;
    int eclipse_relic_holder;
    int eclipse_relic_present;
    int current_dropped_weapon;
    int enemy_kills;
    int outcome;
    int roll_number;
    char action_log[256];
    char last_event[128];
    pid_t arbiter_pid;
    pid_t asp_pid;
    pid_t hip_pid;
    bool ultimate_active;
    bool stun_active;
    int target_enemy;
};

void print_errno(const char *action) {
    fprintf(stderr, "%s: %s\n", action, strerror(errno));
}

void interruptible_usleep(useconds_t us) {
    while (usleep(us) == -1 && errno == EINTR) {
        if (!running) {
            return;
        }
    }
}

bool open_shared_memory() {
    shared_memory_fd = shm_open(shared_memory_name, O_RDWR, 0600);
    if (shared_memory_fd < 0) {
        print_errno("shm_open failed");
        fprintf(stderr, "start the arbiter process before hip\n");
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

void unmap_shared_memory() {
    if (shared_state == nullptr) {
        return;
    }
    if (munmap(shared_state, sizeof(game_state)) != 0) {
        print_errno("munmap failed");
    }
    shared_state = nullptr;
}

void close_shared_fd() {
    if (shared_memory_fd < 0) {
        return;
    }
    if (close(shared_memory_fd) != 0) {
        print_errno("close failed");
    }
    shared_memory_fd = -1;
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

bool register_hip_pid_immediately() {
    if (!lock_memory()) {
        return false;
    }
    shared_state->hip_pid = getpid();
    unlock_memory();
    return true;
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

char weapon_letter(int weapon_id) {
    switch (weapon_id) {
        case game_state::splinter_stick_id: return 'S';
        case game_state::venom_dagger_id: return 'V';
        case game_state::obsidian_axe_id: return 'A';
        case game_state::frostbow_id: return 'F';
        case game_state::thunderstaff_id: return 'T';
        case game_state::iron_halberd_id: return 'I';
        case game_state::solar_core_id: return 'O';
        case game_state::lunar_blade_id: return 'L';
        case game_state::eclipse_relic_id: return 'E';
        default: return ' ';
    }
}

int weapon_color_pair_for(int weapon_id) {
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

int find_first_living_enemy_locked() {
    for (int i = 0; i < game_state::max_enemies; ++i) {
        if (shared_state->enemy_hp[i] > 0) {
            return i;
        }
    }
    return -1;
}

int find_contiguous_free_in_array(const int *arr, int needed) {
    int run = 0;
    int start = -1;
    for (int s = 0; s < game_state::inventory_slots; ++s) {
        if (arr[s] == 0) {
            if (run == 0) {
                start = s;
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
    for (int s = 0; s < game_state::inventory_slots; ++s) {
        int w = arr[s];
        if (w == 0) {
            continue;
        }
        int size = weapon_slot_size(w);
        if (size <= 0) {
            size = 1;
        }
        *run_size_out = size;
        return s;
    }
    return -1;
}

int find_best_weapon_locked(int player_index, int *out_start, int *out_size) {
    int best_id = 0;
    int best_dmg = 0;
    int best_start = -1;
    int best_size = 0;
    int s = 0;
    while (s < game_state::inventory_slots) {
        int w = shared_state->player_primary_inventory[player_index][s];
        if (w == 0) {
            s++;
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
            best_start = s;
            best_size = size;
        }
        s += size;
    }
    if (out_start != nullptr) {
        *out_start = best_start;
    }
    if (out_size != nullptr) {
        *out_size = best_size;
    }
    return best_id;
}

bool inventory_contains_locked(int player_index, int weapon_id) {
    for (int s = 0; s < game_state::inventory_slots; ++s) {
        if (shared_state->player_primary_inventory[player_index][s] == weapon_id) {
            return true;
        }
    }
    return false;
}

void place_weapon(int *arr, int start, int size, int weapon_id) {
    for (int s = start; s < start + size; ++s) {
        arr[s] = weapon_id;
    }
}

void zero_run(int *arr, int start, int size) {
    for (int s = start; s < start + size; ++s) {
        arr[s] = 0;
    }
}

int allocate_weapon_iterative_locked(int player_index, int weapon_id) {
    int needed = weapon_slot_size(weapon_id);
    if (needed <= 0) {
        return -1;
    }
    int *primary = shared_state->player_primary_inventory[player_index];
    int *storage = shared_state->long_term_storage[player_index];
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
            return -1;
        }
        int evict_weapon = primary[evict_start];
        int storage_start = find_contiguous_free_in_array(storage, evict_size);
        if (storage_start < 0) {
            return -1;
        }
        place_weapon(storage, storage_start, evict_size, evict_weapon);
        zero_run(primary, evict_start, evict_size);
    }
    return -1;
}

int swap_in_from_storage_locked(int player_index) {
    int *storage = shared_state->long_term_storage[player_index];
    int chosen_weapon = 0;
    int chosen_start = -1;
    int chosen_size = 0;
    int chosen_dmg = 0;
    int s = 0;
    while (s < game_state::inventory_slots) {
        int w = storage[s];
        if (w == 0) {
            s++;
            continue;
        }
        int size = weapon_slot_size(w);
        if (size <= 0) {
            size = 1;
        }
        int dmg = weapon_damage_value(w);
        if (dmg > chosen_dmg) {
            chosen_dmg = dmg;
            chosen_weapon = w;
            chosen_start = s;
            chosen_size = size;
        }
        s += size;
    }
    if (chosen_weapon == 0) {
        return 0;
    }
    zero_run(storage, chosen_start, chosen_size);
    int placed = allocate_weapon_iterative_locked(player_index, chosen_weapon);
    if (placed < 0) {
        place_weapon(storage, chosen_start, chosen_size, chosen_weapon);
        return -1;
    }
    return chosen_weapon;
}

void distribute_drop_on_enemy_death_locked(int enemy_index) {
    if (shared_state->current_dropped_weapon != 0) {
        return;
    }
    int drop = 0;
    if (shared_state->solar_core_holder < 0) {
        drop = game_state::solar_core_id;
    } else if (shared_state->lunar_blade_holder < 0) {
        drop = game_state::lunar_blade_id;
    } else {
        int weapons[] = {
            game_state::splinter_stick_id,
            game_state::venom_dagger_id,
            game_state::obsidian_axe_id,
            game_state::frostbow_id,
            game_state::thunderstaff_id,
            game_state::iron_halberd_id
        };
        int count = (int)(sizeof(weapons) / sizeof(weapons[0]));
        drop = weapons[enemy_index % count];
    }
    shared_state->current_dropped_weapon = drop;
}

bool is_player_active_locked(int player_id) {
    if (player_id < 0 || player_id >= game_state::max_players) {
        return false;
    }
    if (shared_state->player_hp[player_id] <= 0) {
        return false;
    }
    if (shared_state->player_stun_end_time[player_id] > time(nullptr)) {
        return false;
    }
    return true;
}

bool require_full_stamina_locked(int player_id) {
    return shared_state->player_stamina[player_id] >= game_state::player_max_stamina;
}

void perform_strike(int player_id) {
    if (!lock_memory()) {
        return;
    }
    if (!is_player_active_locked(player_id)) {
        unlock_memory();
        return;
    }
    if (!require_full_stamina_locked(player_id)) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d strike: needs full stamina (%d/%d)",
                 player_id + 1, shared_state->player_stamina[player_id], game_state::player_max_stamina);
        unlock_memory();
        return;
    }
    int target = find_first_living_enemy_locked();
    if (target < 0) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d strike: no living enemies", player_id + 1);
        unlock_memory();
        return;
    }
    int dmg = shared_state->player_damage[player_id];
    shared_state->player_stamina[player_id] = 0;
    int new_hp = shared_state->enemy_hp[target] - dmg;
    if (new_hp < 0) {
        new_hp = 0;
    }
    shared_state->enemy_hp[target] = new_hp;
    if (new_hp == 0) {
        distribute_drop_on_enemy_death_locked(target);
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d strike: hit enemy %d for %d - enemy died, dropped %s",
                 player_id + 1, target + 1, dmg, weapon_name(shared_state->current_dropped_weapon));
    } else {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d strike: hit enemy %d for %d damage",
                 player_id + 1, target + 1, dmg);
    }
    snprintf(shared_state->last_event, sizeof(shared_state->last_event), "p%d_strike_%d", player_id + 1, target + 1);
    unlock_memory();
}

void perform_use_weapon(int player_id) {
    if (!lock_memory()) {
        return;
    }
    if (!is_player_active_locked(player_id)) {
        unlock_memory();
        return;
    }
    if (!require_full_stamina_locked(player_id)) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d use weapon: needs full stamina", player_id + 1);
        unlock_memory();
        return;
    }
    int weapon_start = -1;
    int weapon_size = 0;
    int weapon_id = find_best_weapon_locked(player_id, &weapon_start, &weapon_size);
    if (weapon_id == 0) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d use weapon: inventory is empty", player_id + 1);
        unlock_memory();
        return;
    }
    int target = find_first_living_enemy_locked();
    if (target < 0) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d use weapon: no living enemies", player_id + 1);
        unlock_memory();
        return;
    }
    int dmg = weapon_damage_value(weapon_id);
    shared_state->player_stamina[player_id] = 0;
    int new_hp = shared_state->enemy_hp[target] - dmg;
    if (new_hp < 0) {
        new_hp = 0;
    }
    shared_state->enemy_hp[target] = new_hp;
    if (new_hp == 0) {
        distribute_drop_on_enemy_death_locked(target);
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d wielded %s: %d damage on enemy %d - enemy died, dropped %s",
                 player_id + 1, weapon_name(weapon_id), dmg, target + 1,
                 weapon_name(shared_state->current_dropped_weapon));
    } else {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d wielded %s: %d damage on enemy %d",
                 player_id + 1, weapon_name(weapon_id), dmg, target + 1);
    }
    snprintf(shared_state->last_event, sizeof(shared_state->last_event), "p%d_weapon_%d", player_id + 1, target + 1);
    unlock_memory();
}

void perform_exhaust(int player_id) {
    if (!lock_memory()) {
        return;
    }
    if (!is_player_active_locked(player_id)) {
        unlock_memory();
        return;
    }
    if (!require_full_stamina_locked(player_id)) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d exhaust: needs full stamina", player_id + 1);
        unlock_memory();
        return;
    }
    int target = find_first_living_enemy_locked();
    if (target < 0) {
        unlock_memory();
        return;
    }
    int dmg = shared_state->player_damage[player_id];
    shared_state->player_stamina[player_id] = 0;
    int new_st = shared_state->enemy_stamina[target] - dmg;
    if (new_st < 0) {
        new_st = 0;
    }
    shared_state->enemy_stamina[target] = new_st;
    snprintf(shared_state->action_log, sizeof(shared_state->action_log),
             "player %d exhaust: drained enemy %d stamina by %d (now %d)",
             player_id + 1, target + 1, dmg, new_st);
    snprintf(shared_state->last_event, sizeof(shared_state->last_event), "p%d_exhaust", player_id + 1);
    unlock_memory();
}

void perform_heal(int player_id) {
    if (!lock_memory()) {
        return;
    }
    if (!is_player_active_locked(player_id)) {
        unlock_memory();
        return;
    }
    if (!require_full_stamina_locked(player_id)) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d heal: needs full stamina", player_id + 1);
        unlock_memory();
        return;
    }
    int max_hp = shared_state->player_max_hp[player_id];
    int restore = max_hp / 10;
    if (restore < 1) {
        restore = 1;
    }
    int new_hp = shared_state->player_hp[player_id] + restore;
    if (new_hp > max_hp) {
        new_hp = max_hp;
    }
    shared_state->player_hp[player_id] = new_hp;
    shared_state->player_stamina[player_id] = 0;
    snprintf(shared_state->action_log, sizeof(shared_state->action_log),
             "player %d heal: restored %d hp (%d/%d)",
             player_id + 1, restore, new_hp, max_hp);
    snprintf(shared_state->last_event, sizeof(shared_state->last_event), "p%d_heal", player_id + 1);
    unlock_memory();
}

void perform_skip(int player_id) {
    if (!lock_memory()) {
        return;
    }
    if (!is_player_active_locked(player_id)) {
        unlock_memory();
        return;
    }
    if (!require_full_stamina_locked(player_id)) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d skip: needs full stamina", player_id + 1);
        unlock_memory();
        return;
    }
    shared_state->player_stamina[player_id] = game_state::player_max_stamina / 2;
    snprintf(shared_state->action_log, sizeof(shared_state->action_log),
             "player %d skip: stamina to 50%%", player_id + 1);
    snprintf(shared_state->last_event, sizeof(shared_state->last_event), "p%d_skip", player_id + 1);
    unlock_memory();
}

void perform_pickup(int player_id) {
    if (!lock_memory()) {
        return;
    }
    if (!is_player_active_locked(player_id)) {
        unlock_memory();
        return;
    }
    int dropped = shared_state->current_dropped_weapon;
    if (dropped == 0) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d pickup: nothing on the ground", player_id + 1);
        unlock_memory();
        return;
    }
    int slot = allocate_weapon_iterative_locked(player_id, dropped);
    if (slot < 0) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d pickup: cannot fit %s anywhere", player_id + 1, weapon_name(dropped));
        unlock_memory();
        return;
    }
    shared_state->current_dropped_weapon = 0;
    if (dropped == game_state::solar_core_id) {
        shared_state->solar_core_holder = player_id;
    } else if (dropped == game_state::lunar_blade_id) {
        shared_state->lunar_blade_holder = player_id;
    } else if (dropped == game_state::eclipse_relic_id) {
        shared_state->eclipse_relic_holder = player_id;
    }
    snprintf(shared_state->action_log, sizeof(shared_state->action_log),
             "player %d pickup: acquired %s at slot %d",
             player_id + 1, weapon_name(dropped), slot + 1);
    snprintf(shared_state->last_event, sizeof(shared_state->last_event), "p%d_pickup", player_id + 1);
    unlock_memory();
}

void perform_swap_in(int player_id) {
    if (!lock_memory()) {
        return;
    }
    if (!is_player_active_locked(player_id)) {
        unlock_memory();
        return;
    }
    if (!require_full_stamina_locked(player_id)) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d swap in: needs full stamina", player_id + 1);
        unlock_memory();
        return;
    }
    int swapped = swap_in_from_storage_locked(player_id);
    shared_state->player_stamina[player_id] = 0;
    if (swapped <= 0) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d swap in: storage empty or no fit", player_id + 1);
    } else {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d swap in: brought back %s (cannot use until next turn)",
                 player_id + 1, weapon_name(swapped));
    }
    snprintf(shared_state->last_event, sizeof(shared_state->last_event), "p%d_swap", player_id + 1);
    unlock_memory();
}

void perform_ultimate(int player_id) {
    if (!lock_memory()) {
        return;
    }
    if (!is_player_active_locked(player_id)) {
        unlock_memory();
        return;
    }
    if (!require_full_stamina_locked(player_id)) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d ultimate: needs full stamina", player_id + 1);
        unlock_memory();
        return;
    }
    bool has_solar = inventory_contains_locked(player_id, game_state::solar_core_id);
    bool has_lunar = inventory_contains_locked(player_id, game_state::lunar_blade_id);
    if (!has_solar || !has_lunar) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d ultimate: requires solar core AND lunar blade", player_id + 1);
        unlock_memory();
        return;
    }
    shared_state->player_stamina[player_id] = 0;
    pid_t asp_pid_local = shared_state->asp_pid;
    pid_t arbiter_pid_local = shared_state->arbiter_pid;
    snprintf(shared_state->action_log, sizeof(shared_state->action_log),
             "player %d ULTIMATE: arena freezing for 10s", player_id + 1);
    snprintf(shared_state->last_event, sizeof(shared_state->last_event), "p%d_ultimate", player_id + 1);
    unlock_memory();
    if (asp_pid_local > 0) {
        kill(asp_pid_local, SIGSTOP);
    }
    if (arbiter_pid_local > 0) {
        kill(arbiter_pid_local, SIGUSR1);
    }
    if (lock_memory()) {
        unlock_memory();
    }
}

void perform_stun(int player_id) {
    if (!lock_memory()) {
        return;
    }
    if (!is_player_active_locked(player_id)) {
        unlock_memory();
        return;
    }
    if (!require_full_stamina_locked(player_id)) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d stun: needs full stamina", player_id + 1);
        unlock_memory();
        return;
    }
    int target = find_first_living_enemy_locked();
    if (target < 0) {
        unlock_memory();
        return;
    }
    int dmg = shared_state->player_damage[player_id];
    shared_state->player_stamina[player_id] = 0;
    shared_state->stun_end_time[target] = time(nullptr) + 3;
    int new_hp = shared_state->enemy_hp[target] - dmg;
    if (new_hp < 0) {
        new_hp = 0;
    }
    shared_state->enemy_hp[target] = new_hp;
    pid_t asp_pid_local = shared_state->asp_pid;
    pid_t arbiter_pid_local = shared_state->arbiter_pid;
    if (new_hp == 0) {
        distribute_drop_on_enemy_death_locked(target);
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d STUN: enemy %d hit for %d - eliminated, dropped %s",
                 player_id + 1, target + 1, dmg, weapon_name(shared_state->current_dropped_weapon));
    } else {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d STUN: enemy %d hit for %d, stunned 3s",
                 player_id + 1, target + 1, dmg);
    }
    snprintf(shared_state->last_event, sizeof(shared_state->last_event), "p%d_stun_%d", player_id + 1, target + 1);
    unlock_memory();
    if (asp_pid_local > 0) {
        kill(asp_pid_local, SIGSTOP);
    }
    if (arbiter_pid_local > 0) {
        kill(arbiter_pid_local, SIGUSR2);
    }
    if (lock_memory()) {
        unlock_memory();
    }
}

void send_quit_signals() {
    pid_t arbiter_pid_local = 0;
    pid_t asp_pid_local = 0;
    if (lock_memory()) {
        arbiter_pid_local = shared_state->arbiter_pid;
        asp_pid_local = shared_state->asp_pid;
        shared_state->outcome = game_state::outcome_quit;
        unlock_memory();
    }
    if (asp_pid_local > 0) {
        kill(asp_pid_local, SIGTERM);
    }
    if (arbiter_pid_local > 0) {
        kill(arbiter_pid_local, SIGTERM);
    }
}

void handle_key_for_player(int player_id, int ch) {
    switch (ch) {
        case '1': perform_strike(player_id); break;
        case '2': perform_exhaust(player_id); break;
        case '3': perform_heal(player_id); break;
        case '4': perform_skip(player_id); break;
        case '5': perform_pickup(player_id); break;
        case '6': perform_ultimate(player_id); break;
        case '7': perform_stun(player_id); break;
        case '8': perform_use_weapon(player_id); break;
        case '9': perform_swap_in(player_id); break;
        default: break;
    }
}

void synth_enemy_inventory(int enemy_index, int *out, int solar_core_holder, int lunar_blade_holder, int eclipse_holder) {
    for (int s = 0; s < game_state::inventory_slots; ++s) {
        out[s] = 0;
    }
    int slot = 0;
    int eid = game_state::max_players + enemy_index;
    if (solar_core_holder == eid && slot + game_state::solar_core_slots <= game_state::inventory_slots) {
        for (int k = 0; k < game_state::solar_core_slots; ++k) {
            out[slot + k] = game_state::solar_core_id;
        }
        slot += game_state::solar_core_slots;
    }
    if (lunar_blade_holder == eid && slot + game_state::lunar_blade_slots <= game_state::inventory_slots) {
        for (int k = 0; k < game_state::lunar_blade_slots; ++k) {
            out[slot + k] = game_state::lunar_blade_id;
        }
        slot += game_state::lunar_blade_slots;
    }
    if (eclipse_holder == eid && slot + game_state::eclipse_relic_slots <= game_state::inventory_slots) {
        for (int k = 0; k < game_state::eclipse_relic_slots; ++k) {
            out[slot + k] = game_state::eclipse_relic_id;
        }
        slot += game_state::eclipse_relic_slots;
    }
    int basics[] = {
        game_state::splinter_stick_id, game_state::venom_dagger_id, game_state::obsidian_axe_id,
        game_state::frostbow_id, game_state::thunderstaff_id, game_state::iron_halberd_id
    };
    int basic_id = basics[enemy_index % 6];
    int basic_size = weapon_slot_size(basic_id);
    if (basic_size > 0 && slot + basic_size <= game_state::inventory_slots) {
        for (int k = 0; k < basic_size; ++k) {
            out[slot + k] = basic_id;
        }
    }
}

void take_world_snapshot(world_snapshot &snap) {
    if (!lock_memory()) {
        memset(&snap, 0, sizeof(snap));
        return;
    }
    snap.active_player_count = shared_state->active_player_count;
    snap.active_enemy_count = shared_state->active_enemy_count;
    snap.active_player_index = shared_state->active_player_index;
    snap.solar_core_holder = shared_state->solar_core_holder;
    snap.lunar_blade_holder = shared_state->lunar_blade_holder;
    snap.eclipse_relic_holder = shared_state->eclipse_relic_holder;
    snap.eclipse_relic_present = shared_state->eclipse_relic_present;
    snap.current_dropped_weapon = shared_state->current_dropped_weapon;
    snap.enemy_kills = shared_state->enemy_kills;
    snap.outcome = shared_state->outcome;
    snap.roll_number = shared_state->roll_number;
    snap.arbiter_pid = shared_state->arbiter_pid;
    snap.asp_pid = shared_state->asp_pid;
    snap.hip_pid = shared_state->hip_pid;
    memcpy(snap.action_log, shared_state->action_log, sizeof(snap.action_log));
    snap.action_log[sizeof(snap.action_log) - 1] = '\0';
    memcpy(snap.last_event, shared_state->last_event, sizeof(snap.last_event));
    snap.last_event[sizeof(snap.last_event) - 1] = '\0';

    snap.target_enemy = find_first_living_enemy_locked();

    time_t now = time(nullptr);
    bool any_player_stunned = false;
    bool any_enemy_stunned = false;

    for (int i = 0; i < game_state::max_players; ++i) {
        snap.players[i].hp = shared_state->player_hp[i];
        snap.players[i].max_hp = shared_state->player_max_hp[i] > 0 ? shared_state->player_max_hp[i] : 1;
        snap.players[i].stamina = shared_state->player_stamina[i];
        snap.players[i].max_stamina = game_state::player_max_stamina;
        snap.players[i].damage = shared_state->player_damage[i];
        snap.players[i].stun_end = shared_state->player_stun_end_time[i];
        snap.players[i].active = (i < snap.active_player_count);
        snap.players[i].dead = snap.players[i].active && snap.players[i].hp <= 0;
        snap.players[i].turn = (i == snap.active_player_index);
        for (int s = 0; s < game_state::inventory_slots; ++s) {
            snap.players[i].inventory[s] = shared_state->player_primary_inventory[i][s];
            snap.players[i].storage[s] = shared_state->long_term_storage[i][s];
        }
        if (snap.players[i].active && snap.players[i].stun_end > now) {
            any_player_stunned = true;
        }
    }

    for (int i = 0; i < game_state::max_enemies; ++i) {
        snap.enemies[i].hp = shared_state->enemy_hp[i];
        snap.enemies[i].max_hp = shared_state->enemy_max_hp[i] > 0 ? shared_state->enemy_max_hp[i] : 1;
        snap.enemies[i].stamina = shared_state->enemy_stamina[i];
        snap.enemies[i].max_stamina = game_state::enemy_max_stamina;
        snap.enemies[i].damage = shared_state->enemy_damage[i];
        snap.enemies[i].stun_end = shared_state->stun_end_time[i];
        snap.enemies[i].active = (i < snap.active_enemy_count);
        snap.enemies[i].dead = snap.enemies[i].active && snap.enemies[i].hp <= 0;
        snap.enemies[i].turn = (i == snap.target_enemy);
        synth_enemy_inventory(i, snap.enemies[i].inventory, snap.solar_core_holder, snap.lunar_blade_holder, snap.eclipse_relic_holder);
        for (int s = 0; s < game_state::inventory_slots; ++s) {
            snap.enemies[i].storage[s] = 0;
        }
        if (snap.enemies[i].active && snap.enemies[i].stun_end > now) {
            any_enemy_stunned = true;
        }
    }

    snap.ultimate_active = (strstr(snap.action_log, "ultimate triggered") != nullptr) ||
                           (strstr(snap.action_log, "ULTIMATE") != nullptr);
    snap.stun_active = any_player_stunned || any_enemy_stunned ||
                       (strstr(snap.action_log, "stun pulse") != nullptr);

    unlock_memory();
}

void draw_box_at(int y, int x, int h, int w, int pair, int extra_attr) {
    if (h < 2 || w < 2) {
        return;
    }
    chtype attr = COLOR_PAIR(pair) | extra_attr;
    mvaddch(y, x, ACS_ULCORNER | attr);
    mvaddch(y, x + w - 1, ACS_URCORNER | attr);
    mvaddch(y + h - 1, x, ACS_LLCORNER | attr);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER | attr);
    for (int i = 1; i < w - 1; ++i) {
        mvaddch(y, x + i, ACS_HLINE | attr);
        mvaddch(y + h - 1, x + i, ACS_HLINE | attr);
    }
    for (int i = 1; i < h - 1; ++i) {
        mvaddch(y + i, x, ACS_VLINE | attr);
        mvaddch(y + i, x + w - 1, ACS_VLINE | attr);
    }
}

void fill_rect(int y, int x, int h, int w, int pair) {
    if (h <= 0 || w <= 0) {
        return;
    }
    attron(COLOR_PAIR(pair));
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            mvaddch(y + yy, x + xx, ' ');
        }
    }
    attroff(COLOR_PAIR(pair));
}

void draw_double_box_at(int y, int x, int h, int w, int pair, int extra_attr) {
    if (h < 2 || w < 2) {
        return;
    }
    chtype attr = COLOR_PAIR(pair) | extra_attr;
    mvaddch(y, x, ACS_ULCORNER | attr | A_BOLD);
    mvaddch(y, x + w - 1, ACS_URCORNER | attr | A_BOLD);
    mvaddch(y + h - 1, x, ACS_LLCORNER | attr | A_BOLD);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER | attr | A_BOLD);
    for (int i = 1; i < w - 1; ++i) {
        mvaddch(y, x + i, ACS_HLINE | attr | A_BOLD);
        mvaddch(y + h - 1, x + i, ACS_HLINE | attr | A_BOLD);
    }
    for (int i = 1; i < h - 1; ++i) {
        mvaddch(y + i, x, ACS_VLINE | attr | A_BOLD);
        mvaddch(y + i, x + w - 1, ACS_VLINE | attr | A_BOLD);
    }
}

void draw_titled_box(int y, int x, int h, int w, const char *title, int pair, int extra_attr) {
    if (h > 2 && w > 2) {
        fill_rect(y + 1, x + 1, h - 2, w - 2, pair_panel_fill);
    }
    draw_box_at(y, x, h, w, pair, extra_attr);
    if (title == nullptr || w < 8) {
        return;
    }
    int title_len = (int)strlen(title);
    int max_title = w - 6;
    if (title_len > max_title) {
        title_len = max_title;
    }
    chtype attr = COLOR_PAIR(pair) | extra_attr | A_BOLD;
    mvaddch(y, x + 2, ' ' | attr);
    attron(attr);
    mvprintw(y, x + 3, "%.*s", title_len, title);
    attroff(attr);
    mvaddch(y, x + 3 + title_len, ' ' | attr);
}

int hp_pair_for_ratio(double ratio) {
    if (ratio < 0.20) {
        return pair_hp_critical;
    }
    if (ratio < 0.40) {
        return pair_hp_low;
    }
    if (ratio < 0.70) {
        return pair_hp_med;
    }
    return pair_hp_high;
}

void draw_bar(int y, int x, int width, int value, int max_value, const char *label, int color_pair, bool blink_when_low) {
    if (width < 8) {
        return;
    }
    if (max_value <= 0) {
        max_value = 1;
    }
    if (value < 0) {
        value = 0;
    }
    if (value > max_value) {
        value = max_value;
    }
    double ratio = (double)value / (double)max_value;
    int pair_to_use = color_pair;
    if (color_pair == 0) {
        pair_to_use = hp_pair_for_ratio(ratio);
    }
    int extra = A_BOLD;
    if (blink_when_low && ratio < 0.20) {
        extra |= A_BLINK;
    }
    int label_w = 4;
    int counts_w = 14;
    int bar_w = width - label_w - counts_w - 4;
    if (bar_w < 4) {
        bar_w = width - label_w - 2;
        counts_w = 0;
    }
    if (bar_w < 4) {
        return;
    }
    attron(COLOR_PAIR(pair_default) | A_BOLD);
    mvprintw(y, x, "%-3s", label);
    attroff(COLOR_PAIR(pair_default) | A_BOLD);
    mvaddch(y, x + label_w - 1, '[');
    int filled = (int)(ratio * bar_w);
    if (filled > bar_w) {
        filled = bar_w;
    }
    attron(COLOR_PAIR(pair_to_use) | extra);
    for (int i = 0; i < filled; ++i) {
        mvaddch(y, x + label_w + i, ACS_CKBOARD);
    }
    attroff(COLOR_PAIR(pair_to_use) | extra);
    attron(COLOR_PAIR(pair_inv_empty));
    for (int i = filled; i < bar_w; ++i) {
        mvaddch(y, x + label_w + i, ACS_BOARD);
    }
    attroff(COLOR_PAIR(pair_inv_empty));
    mvaddch(y, x + label_w + bar_w, ']');
    if (counts_w > 0) {
        attron(COLOR_PAIR(pair_default));
        mvprintw(y, x + label_w + bar_w + 2, "%d/%d", value, max_value);
        attroff(COLOR_PAIR(pair_default));
    }
}

void draw_inventory_strip(int y, int x, int width, const int *inventory, bool dead_overlay) {
    if (width < game_state::inventory_slots + 2) {
        return;
    }
    int slot_w = (width - 2) / game_state::inventory_slots;
    if (slot_w < 1) {
        slot_w = 1;
    }
    if (slot_w > 3) {
        slot_w = 3;
    }
    int total_w = slot_w * game_state::inventory_slots;
    int start_x = x + (width - total_w) / 2;
    int s = 0;
    while (s < game_state::inventory_slots) {
        int weapon = inventory[s];
        int run = 1;
        while (s + run < game_state::inventory_slots && inventory[s + run] == weapon) {
            run++;
        }
        int pair = weapon == 0 ? pair_inv_empty : weapon_color_pair_for(weapon);
        int attrs = COLOR_PAIR(pair) | A_REVERSE | A_BOLD;
        if (dead_overlay) {
            attrs = COLOR_PAIR(pair_border_dead) | A_REVERSE | A_BOLD;
        }
        char letter = weapon == 0 ? ' ' : weapon_letter(weapon);
        int label_pos = start_x + (s + run / 2) * slot_w + (slot_w / 2);
        attron(attrs);
        for (int k = 0; k < run * slot_w; ++k) {
            char ch = ' ';
            int abs_x = start_x + s * slot_w + k;
            if (run > 0 && abs_x == label_pos && weapon != 0) {
                ch = letter;
            }
            mvaddch(y, abs_x, ch);
        }
        attroff(attrs);
        s += run;
    }
}

void render_entity(int y, int x, int h, int w, const char *title, const entity_snapshot &es, bool is_player) {
    int border_pair = pair_border_normal;
    int border_extra = 0;
    if (es.dead) {
        border_pair = pair_border_dead;
        border_extra = A_BOLD;
    } else if (es.turn) {
        border_pair = pair_border_active;
        border_extra = A_BOLD;
        draw_double_box_at(y, x, h, w, border_pair, border_extra);
        int title_len = (int)strlen(title);
        chtype attr = COLOR_PAIR(border_pair) | A_BOLD | A_REVERSE;
        attron(attr);
        mvaddch(y, x + 2, ' ');
        mvprintw(y, x + 3, " %.*s ", title_len, title);
        attroff(attr);
    } else {
        draw_titled_box(y, x, h, w, title, border_pair, border_extra);
    }
    if (es.dead || (!es.turn && !es.dead)) {
        if (!es.turn) {
            draw_titled_box(y, x, h, w, title, border_pair, border_extra);
        }
    }
    int inner_y = y + 1;
    int inner_x = x + 2;
    int inner_w = w - 4;
    int inner_h = h - 2;
    if (inner_h <= 0 || inner_w <= 0) {
        return;
    }
    int row = 0;
    if (inner_h - row >= 1 && inner_w >= 12) {
        int hp_pair = es.dead ? pair_border_dead : 0;
        draw_bar(inner_y + row, inner_x, inner_w, es.hp, es.max_hp, "hp", hp_pair, !es.dead && is_player);
        row++;
    }
    if (inner_h - row >= 1 && inner_w >= 12) {
        int sta_pair = es.dead ? pair_border_dead : pair_stamina;
        if (!es.dead && es.stamina >= es.max_stamina) {
            sta_pair = pair_stamina_full;
        }
        draw_bar(inner_y + row, inner_x, inner_w, es.stamina, es.max_stamina, "sta", sta_pair, false);
        row++;
    }
    if (inner_h - row >= 1) {
        time_t now = time(nullptr);
        if (es.stun_end > now) {
            int seconds_left = (int)(es.stun_end - now);
            attron(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
            mvprintw(inner_y + row, inner_x, "[STUNNED %ds]", seconds_left);
            attroff(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
            if (inner_w > 18 && es.damage > 0) {
                attron(COLOR_PAIR(pair_default));
                mvprintw(inner_y + row, inner_x + 14, "dmg %d", es.damage);
                attroff(COLOR_PAIR(pair_default));
            }
        } else if (es.dead) {
            attron(COLOR_PAIR(pair_border_dead) | A_BOLD | A_BLINK);
            mvprintw(inner_y + row, inner_x, "  -- ELIMINATED --  ");
            attroff(COLOR_PAIR(pair_border_dead) | A_BOLD | A_BLINK);
        } else {
            attron(COLOR_PAIR(pair_default));
            mvprintw(inner_y + row, inner_x, "dmg %d", es.damage);
            attroff(COLOR_PAIR(pair_default));
            if (es.turn) {
                attron(COLOR_PAIR(pair_border_active) | A_BOLD | A_BLINK);
                mvprintw(inner_y + row, inner_x + 8, "<< YOUR TURN >>");
                attroff(COLOR_PAIR(pair_border_active) | A_BOLD | A_BLINK);
            }
        }
        row++;
    }
    if (inner_h - row >= 2) {
        int inv_y = inner_y + inner_h - 2;
        int inv_h = 2;
        draw_box_at(inv_y, inner_x, inv_h, inner_w, border_pair, border_extra);
        draw_inventory_strip(inv_y + inv_h - 1, inner_x + 1, inner_w - 2, es.inventory, es.dead);
        attron(COLOR_PAIR(pair_panel_title) | A_BOLD);
        mvaddch(inv_y, inner_x + 2, ' ');
        mvprintw(inv_y, inner_x + 3, "inventroy tetris");
        mvaddch(inv_y, inner_x + 19, ' ');
        attroff(COLOR_PAIR(pair_panel_title) | A_BOLD);
    } else if (inner_h - row >= 1) {
        draw_inventory_strip(inner_y + row, inner_x, inner_w, es.inventory, es.dead);
    }
}

void render_player_panel(int y, int x, int h, int w, const world_snapshot &snap) {
    draw_titled_box(y, x, h, w, "player squad", pair_panel_title, A_BOLD);
    int inner_y = y + 1;
    int inner_x = x + 1;
    int inner_w = w - 2;
    int inner_h = h - 2;
    int active = snap.active_player_count;
    if (active <= 0) {
        active = 1;
    }
    if (active > game_state::max_players) {
        active = game_state::max_players;
    }
    int per_h = inner_h / active;
    if (per_h < 5) {
        per_h = 5;
    }
    char title_buf[32];
    int drawn_y = inner_y;
    for (int i = 0; i < active; ++i) {
        if (drawn_y + per_h > inner_y + inner_h) {
            break;
        }
        snprintf(title_buf, sizeof(title_buf), "P%d  dmg %d", i + 1, snap.players[i].damage);
        render_entity(drawn_y, inner_x, per_h, inner_w, title_buf, snap.players[i], true);
        drawn_y += per_h;
    }
}

void render_enemy_panel(int y, int x, int h, int w, const world_snapshot &snap) {
    draw_titled_box(y, x, h, w, "enemy forces", pair_panel_title, A_BOLD);
    int inner_y = y + 1;
    int inner_x = x + 1;
    int inner_w = w - 2;
    int inner_h = h - 2;
    int active = snap.active_enemy_count;
    if (active <= 0) {
        active = 1;
    }
    if (active > game_state::max_enemies) {
        active = game_state::max_enemies;
    }
    int columns = 1;
    int min_box_w = 28;
    if (inner_w >= min_box_w * 2 + 2 && active > 4) {
        columns = 2;
    }
    if (inner_w >= min_box_w * 3 + 4 && active > 6) {
        columns = 3;
    }
    int rows = (active + columns - 1) / columns;
    if (rows <= 0) {
        rows = 1;
    }
    int per_h = inner_h / rows;
    if (per_h < 4) {
        per_h = 4;
    }
    int per_w = inner_w / columns;
    char title_buf[32];
    for (int i = 0; i < active; ++i) {
        int col = i % columns;
        int row = i / columns;
        int box_y = inner_y + row * per_h;
        int box_x = inner_x + col * per_w;
        if (box_y + per_h > inner_y + inner_h) {
            break;
        }
        snprintf(title_buf, sizeof(title_buf), "E%d dmg%d", i + 1, snap.enemies[i].damage);
        render_entity(box_y, box_x, per_h, per_w, title_buf, snap.enemies[i], false);
    }
}

void render_banner(int y, int x, int w) {
    const char *title = " CHRONO RIFT ";
    const char *subtitle = "emerald tide arena  |  calm palette mode";
    int tlen = (int)strlen(title);
    int slen = (int)strlen(subtitle);
    int tx = x + (w - tlen) / 2;
    int sx = x + (w - slen) / 2;
    if (tx < x + 1) {
        tx = x + 1;
    }
    if (sx < x + 1) {
        sx = x + 1;
    }
    attron(COLOR_PAIR(pair_banner) | A_BOLD | A_REVERSE);
    mvprintw(y, tx, "%s", title);
    attroff(COLOR_PAIR(pair_banner) | A_BOLD | A_REVERSE);
    attron(COLOR_PAIR(pair_banner) | A_BOLD);
    mvprintw(y + 1, sx, "%.*s", w - 2, subtitle);
    attroff(COLOR_PAIR(pair_banner) | A_BOLD);
}

void render_action_log_box(int y, int x, int h, int w, const world_snapshot &snap) {
    draw_titled_box(y, x, h, w, "action log", pair_arena_title, A_BOLD);
    int inner_y = y + 1;
    int inner_x = x + 2;
    int inner_w = w - 4;
    int inner_h = h - 2;
    if (inner_h <= 0 || inner_w <= 4) {
        return;
    }
    attron(COLOR_PAIR(pair_log));
    int len = (int)strlen(snap.action_log);
    int line = 0;
    int pos = 0;
    while (pos < len && line < inner_h) {
        int chunk = inner_w;
        if (pos + chunk > len) {
            chunk = len - pos;
        }
        mvprintw(inner_y + line, inner_x, "%.*s", chunk, snap.action_log + pos);
        pos += chunk;
        line++;
    }
    attroff(COLOR_PAIR(pair_log));
}

void render_kill_counter(int y, int x, int w, const world_snapshot &snap) {
    if (w < 10) {
        return;
    }
    int filled = snap.enemy_kills;
    int total = game_state::kills_required_to_win;
    int bar_w = w - 18;
    if (bar_w < 5) {
        bar_w = w - 12;
    }
    if (bar_w < 5) {
        return;
    }
    attron(COLOR_PAIR(pair_kill_counter) | A_BOLD);
    mvprintw(y, x, "KILLS %2d/%2d", filled, total);
    attroff(COLOR_PAIR(pair_kill_counter) | A_BOLD);
    int bx = x + 12;
    mvaddch(y, bx, '[');
    int hits = (filled * bar_w) / total;
    if (hits > bar_w) {
        hits = bar_w;
    }
    attron(COLOR_PAIR(pair_kill_counter) | A_BOLD | A_REVERSE);
    for (int i = 0; i < hits; ++i) {
        mvaddch(y, bx + 1 + i, '#');
    }
    attroff(COLOR_PAIR(pair_kill_counter) | A_BOLD | A_REVERSE);
    attron(COLOR_PAIR(pair_inv_empty));
    for (int i = hits; i < bar_w; ++i) {
        mvaddch(y, bx + 1 + i, '-');
    }
    attroff(COLOR_PAIR(pair_inv_empty));
    mvaddch(y, bx + 1 + bar_w, ']');
}

void render_system_status_box(int y, int x, int h, int w, const world_snapshot &snap) {
    draw_titled_box(y, x, h, w, "system status", pair_arena_title, A_BOLD);
    int inner_y = y + 1;
    int inner_x = x + 2;
    int inner_w = w - 4;
    if (inner_w <= 0) {
        return;
    }
    int row = 0;
    int max_rows = h - 2;
    if (row < max_rows) {
        attron(COLOR_PAIR(pair_default));
        mvprintw(inner_y + row, inner_x, "roll: %d  party: %d  enemies: %d", snap.roll_number, snap.active_player_count, snap.active_enemy_count);
        attroff(COLOR_PAIR(pair_default));
        row++;
    }
    if (row < max_rows) {
        attron(COLOR_PAIR(pair_default));
        mvprintw(inner_y + row, inner_x, "pids   arb:%d  asp:%d  hip:%d", (int)snap.arbiter_pid, (int)snap.asp_pid, (int)snap.hip_pid);
        attroff(COLOR_PAIR(pair_default));
        row++;
    }
    if (row < max_rows) {
        if (snap.outcome == game_state::outcome_win) {
            attron(COLOR_PAIR(pair_overlay_win) | A_BOLD | A_BLINK);
            mvprintw(inner_y + row, inner_x, "STATE: VICTORY");
            attroff(COLOR_PAIR(pair_overlay_win) | A_BOLD | A_BLINK);
        } else if (snap.outcome == game_state::outcome_lose) {
            attron(COLOR_PAIR(pair_overlay_lose) | A_BOLD | A_BLINK);
            mvprintw(inner_y + row, inner_x, "STATE: DEFEAT");
            attroff(COLOR_PAIR(pair_overlay_lose) | A_BOLD | A_BLINK);
        } else if (snap.ultimate_active) {
            attron(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
            mvprintw(inner_y + row, inner_x, "STATE: ULTIMATE FROZEN");
            attroff(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
        } else if (snap.stun_active) {
            attron(COLOR_PAIR(pair_status_warn) | A_BOLD);
            mvprintw(inner_y + row, inner_x, "STATE: stun in progress");
            attroff(COLOR_PAIR(pair_status_warn) | A_BOLD);
        } else {
            attron(COLOR_PAIR(pair_status_ok) | A_BOLD);
            mvprintw(inner_y + row, inner_x, "STATE: live combat");
            attroff(COLOR_PAIR(pair_status_ok) | A_BOLD);
        }
        row++;
    }
    if (row < max_rows) {
        if (strstr(snap.action_log, "deadlock") != nullptr) {
            attron(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
            mvprintw(inner_y + row, inner_x, "DEADLOCK BROKEN BY ARBITER");
            attroff(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
            row++;
        } else if (snap.active_player_index >= 0) {
            attron(COLOR_PAIR(pair_status_ok) | A_BOLD);
            mvprintw(inner_y + row, inner_x, "active turn: P%d", snap.active_player_index + 1);
            attroff(COLOR_PAIR(pair_status_ok) | A_BOLD);
            row++;
        }
    }
    if (row < max_rows && inner_w >= 16) {
        render_kill_counter(inner_y + row, inner_x, inner_w, snap);
    }
}

void render_artifact_tracker_box(int y, int x, int h, int w, const world_snapshot &snap) {
    draw_titled_box(y, x, h, w, "artifact tracker", pair_arena_title, A_BOLD);
    int inner_y = y + 1;
    int inner_x = x + 2;
    int inner_w = w - 4;
    int max_rows = h - 2;
    if (max_rows <= 0 || inner_w <= 0) {
        return;
    }
    char buffer[64];
    int row = 0;

    struct artifact_entry {
        int holder;
        int dropped_id;
        int color_pair;
        int icon_pair;
        char letter;
        const char *name;
    };

    artifact_entry entries[3] = {
        {snap.solar_core_holder, game_state::solar_core_id, pair_artifact_solar, pair_inv_solar, 'O', "solar core"},
        {snap.lunar_blade_holder, game_state::lunar_blade_id, pair_artifact_lunar, pair_inv_lunar, 'L', "lunar blade"},
        {snap.eclipse_relic_holder, game_state::eclipse_relic_id, pair_artifact_eclipse, pair_inv_eclipse, 'E', "eclipse relic"}
    };
    int entry_count = snap.eclipse_relic_present ? 3 : 2;

    for (int e = 0; e < entry_count && row < max_rows; ++e) {
        artifact_entry &art = entries[e];
        const char *label;
        int color = art.color_pair;
        if (snap.current_dropped_weapon == art.dropped_id) {
            label = "on ground";
            color = pair_artifact_ground;
        } else if (art.holder < 0) {
            label = (e == 2 && !snap.eclipse_relic_present) ? "not yet introduced" : "missing";
            color = pair_artifact_ground;
        } else if (art.holder < game_state::max_players) {
            snprintf(buffer, sizeof(buffer), "player %d", art.holder + 1);
            label = buffer;
        } else {
            snprintf(buffer, sizeof(buffer), "enemy %d", art.holder - game_state::max_players + 1);
            label = buffer;
            color = pair_border_dead;
        }
        attron(COLOR_PAIR(art.icon_pair) | A_REVERSE | A_BOLD);
        mvaddch(inner_y + row, inner_x, ' ');
        mvaddch(inner_y + row, inner_x + 1, art.letter);
        mvaddch(inner_y + row, inner_x + 2, ' ');
        attroff(COLOR_PAIR(art.icon_pair) | A_REVERSE | A_BOLD);
        attron(COLOR_PAIR(color) | A_BOLD);
        mvprintw(inner_y + row, inner_x + 4, "%s: %s", art.name, label);
        attroff(COLOR_PAIR(color) | A_BOLD);
        row++;
    }

    if (row < max_rows && snap.current_dropped_weapon != 0) {
        const char *name = weapon_name(snap.current_dropped_weapon);
        int icon_pair = weapon_color_pair_for(snap.current_dropped_weapon);
        char icon_letter = weapon_letter(snap.current_dropped_weapon);
        attron(COLOR_PAIR(icon_pair) | A_REVERSE | A_BOLD | A_BLINK);
        mvaddch(inner_y + row, inner_x, ' ');
        mvaddch(inner_y + row, inner_x + 1, icon_letter);
        mvaddch(inner_y + row, inner_x + 2, ' ');
        attroff(COLOR_PAIR(icon_pair) | A_REVERSE | A_BOLD | A_BLINK);
        attron(COLOR_PAIR(pair_artifact_ground) | A_BOLD);
        mvprintw(inner_y + row, inner_x + 4, "ground: %s [press 5]", name);
        attroff(COLOR_PAIR(pair_artifact_ground) | A_BOLD);
    }
}

void render_arena_panel(int y, int x, int h, int w, const world_snapshot &snap) {
    draw_titled_box(y, x, h, w, "combat arena", pair_arena_title, A_BOLD);
    int inner_y = y + 1;
    int inner_x = x + 1;
    int inner_w = w - 2;
    int inner_h = h - 2;
    if (inner_h < 8 || inner_w < 10) {
        return;
    }
    int banner_h = 3;
    if (banner_h > inner_h - 7) {
        banner_h = 1;
    }
    render_banner(inner_y, inner_x, inner_w);
    int below_banner_y = inner_y + banner_h;
    int remaining_h = inner_h - banner_h;
    int log_h = (remaining_h * 55) / 100;
    if (log_h < 4) {
        log_h = 4;
    }
    int rest = remaining_h - log_h;
    int status_h = rest / 2;
    if (status_h < 5) {
        status_h = 5;
    }
    int artifact_h = rest - status_h;
    if (artifact_h < 4) {
        artifact_h = 4;
        status_h = rest - artifact_h;
    }
    if (status_h < 4) {
        status_h = rest;
        artifact_h = 0;
    }
    render_action_log_box(below_banner_y, inner_x, log_h, inner_w, snap);
    if (status_h > 0) {
        render_system_status_box(below_banner_y + log_h, inner_x, status_h, inner_w, snap);
    }
    if (artifact_h > 0) {
        render_artifact_tracker_box(below_banner_y + log_h + status_h, inner_x, artifact_h, inner_w, snap);
    }
}

void render_top_hud(int y, int term_w, const world_snapshot &snap) {
    draw_double_box_at(y, 0, 3, term_w, pair_top_hud, A_BOLD);
    attron(COLOR_PAIR(pair_top_hud) | A_BOLD | A_REVERSE);
    mvprintw(y + 1, 2, " chrono rift ");
    attroff(COLOR_PAIR(pair_top_hud) | A_BOLD | A_REVERSE);
    attron(COLOR_PAIR(pair_top_hud) | A_BOLD);
    mvprintw(y + 1, 18, "party %d  enemies %d  kills %d/%d  roll %d",
             snap.active_player_count, snap.active_enemy_count, snap.enemy_kills,
             game_state::kills_required_to_win, snap.roll_number);
    if (snap.active_player_index >= 0) {
        mvprintw(y + 1, term_w - 20, "turn P%d", snap.active_player_index + 1);
    }
    attroff(COLOR_PAIR(pair_top_hud) | A_BOLD);
}

void render_command_bar(int y, int term_w, int h) {
    attron(COLOR_PAIR(pair_command_bar) | A_REVERSE | A_BOLD);
    for (int row = 0; row < h; ++row) {
        for (int i = 0; i < term_w; ++i) {
            mvaddch(y + row, i, ' ');
        }
    }
    const char *line1 = " [1]strike [2]exhaust [3]heal [4]skip [5]pickup [8]weapon [9]swap ";
    const char *line2 = " [6]ultimate [7]stun [?]help [q]quit ";
    int l1 = (int)strlen(line1);
    int l2 = (int)strlen(line2);
    int s1 = (term_w - l1) / 2;
    int s2 = (term_w - l2) / 2;
    if (s1 < 0) {
        s1 = 0;
        l1 = term_w;
    }
    if (s2 < 0) {
        s2 = 0;
        l2 = term_w;
    }
    mvprintw(y, s1, "%.*s", l1, line1);
    if (h > 1) {
        mvprintw(y + 1, s2, "%.*s", l2, line2);
    }
    attroff(COLOR_PAIR(pair_command_bar) | A_REVERSE | A_BOLD);
}

void render_overlay(int term_h, int term_w, const char *title, const char *line1, const char *line2, int color) {
    int box_w = 56;
    int box_h = 11;
    if (box_w > term_w - 4) {
        box_w = term_w - 4;
    }
    if (box_h > term_h - 4) {
        box_h = term_h - 4;
    }
    int box_y = (term_h - box_h) / 2;
    int box_x = (term_w - box_w) / 2;
    for (int yy = box_y; yy < box_y + box_h; ++yy) {
        attron(COLOR_PAIR(color) | A_REVERSE);
        for (int xx = box_x; xx < box_x + box_w; ++xx) {
            mvaddch(yy, xx, ' ');
        }
        attroff(COLOR_PAIR(color) | A_REVERSE);
    }
    draw_double_box_at(box_y, box_x, box_h, box_w, color, A_BOLD);
    int title_len = (int)strlen(title);
    attron(COLOR_PAIR(color) | A_BOLD | A_REVERSE | A_BLINK);
    mvprintw(box_y + 2, box_x + (box_w - title_len) / 2, "%s", title);
    attroff(COLOR_PAIR(color) | A_BOLD | A_REVERSE | A_BLINK);
    attron(COLOR_PAIR(color) | A_BOLD);
    int l1 = (int)strlen(line1);
    mvprintw(box_y + 5, box_x + (box_w - l1) / 2, "%s", line1);
    int l2 = (int)strlen(line2);
    mvprintw(box_y + 7, box_x + (box_w - l2) / 2, "%s", line2);
    attroff(COLOR_PAIR(color) | A_BOLD);
    const char *exit_hint = "press q to exit";
    int el = (int)strlen(exit_hint);
    attron(COLOR_PAIR(color) | A_REVERSE | A_BOLD | A_BLINK);
    mvprintw(box_y + box_h - 2, box_x + (box_w - el) / 2, "%s", exit_hint);
    attroff(COLOR_PAIR(color) | A_REVERSE | A_BOLD | A_BLINK);
}

void render_help_overlay(int term_h, int term_w) {
    int box_w = 60;
    int box_h = 18;
    if (box_w > term_w - 4) {
        box_w = term_w - 4;
    }
    if (box_h > term_h - 4) {
        box_h = term_h - 4;
    }
    int box_y = (term_h - box_h) / 2;
    int box_x = (term_w - box_w) / 2;
    for (int yy = box_y; yy < box_y + box_h; ++yy) {
        attron(COLOR_PAIR(pair_help));
        for (int xx = box_x; xx < box_x + box_w; ++xx) {
            mvaddch(yy, xx, ' ');
        }
        attroff(COLOR_PAIR(pair_help));
    }
    draw_double_box_at(box_y, box_x, box_h, box_w, pair_help, A_BOLD);
    attron(COLOR_PAIR(pair_help) | A_BOLD | A_REVERSE);
    mvprintw(box_y + 1, box_x + 2, "  CHRONO RIFT - controls  ");
    attroff(COLOR_PAIR(pair_help) | A_BOLD | A_REVERSE);
    const char *lines[] = {
        "1  strike      - basic attack, player damage stat",
        "2  exhaust     - drain enemy stamina",
        "3  heal        - restore 10% max hp",
        "4  skip        - drop stamina to 50%",
        "5  pickup      - take ground drop",
        "6  ultimate    - need solar core + lunar blade",
        "7  stun        - hit + freeze enemy 3s",
        "8  use weapon  - hit with best weapon in inventory",
        "9  swap in     - bring weapon back from storage",
        "?  toggle this help",
        "q  quit (sigterm to arbiter & asp)"
    };
    int n = (int)(sizeof(lines) / sizeof(lines[0]));
    attron(COLOR_PAIR(pair_help));
    for (int i = 0; i < n && box_y + 3 + i < box_y + box_h - 1; ++i) {
        mvprintw(box_y + 3 + i, box_x + 3, "%s", lines[i]);
    }
    attroff(COLOR_PAIR(pair_help));
}

void render_frame(const world_snapshot &snap) {
    pthread_mutex_lock(&ncurses_lock);
    if (resize_pending) {
        resize_pending = 0;
        endwin();
        refresh();
        clear();
    }
    erase();
    int term_h, term_w;
    getmaxyx(stdscr, term_h, term_w);
    if (term_h < 14 || term_w < 70) {
        attron(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
        mvprintw(0, 0, "terminal too small (need at least 70x14)");
        attroff(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
        refresh();
        pthread_mutex_unlock(&ncurses_lock);
        return;
    }
    int hud_h = 3;
    int command_h = 2;
    int main_h = term_h - command_h - hud_h;
    int left_w = term_w * 27 / 100;
    int right_w = term_w * 33 / 100;
    int center_w = term_w - left_w - right_w;
    if (left_w < 24) {
        left_w = 24;
    }
    if (right_w < 28) {
        right_w = 28;
    }
    if (left_w + right_w > term_w - 24) {
        left_w = (term_w - 24) * 28 / 66;
        right_w = (term_w - 24) - left_w;
    }
    center_w = term_w - left_w - right_w;
    render_top_hud(0, term_w, snap);
    render_player_panel(hud_h, 0, main_h, left_w, snap);
    render_arena_panel(hud_h, left_w, main_h, center_w, snap);
    render_enemy_panel(hud_h, left_w + center_w, main_h, right_w, snap);
    render_command_bar(term_h - command_h, term_w, command_h);

    if (snap.outcome == game_state::outcome_win) {
        char line1[64];
        char line2[64];
        snprintf(line1, sizeof(line1), "%d enemies vanquished", snap.enemy_kills);
        snprintf(line2, sizeof(line2), "roll seed %d guided your hand", snap.roll_number);
        render_overlay(term_h, term_w, "  V I C T O R Y  ", line1, line2, pair_overlay_win);
    } else if (snap.outcome == game_state::outcome_lose) {
        char line1[64];
        char line2[64];
        snprintf(line1, sizeof(line1), "%d enemies eliminated before defeat", snap.enemy_kills);
        snprintf(line2, sizeof(line2), "the rift consumed your party");
        render_overlay(term_h, term_w, "  D E F E A T  ", line1, line2, pair_overlay_lose);
    } else if (snap.outcome == game_state::outcome_quit) {
        render_overlay(term_h, term_w, "  S H U T D O W N  ", "session terminated", "thanks for playing chrono rift", pair_overlay_quit);
    } else if (show_help_overlay) {
        render_help_overlay(term_h, term_w);
    }

    refresh();
    pthread_mutex_unlock(&ncurses_lock);
    render_frame_counter++;
}

void *render_loop(void *) {
    while (running) {
        world_snapshot snap;
        take_world_snapshot(snap);
        render_frame(snap);
        if (snap.outcome != game_state::outcome_ongoing) {
            interruptible_usleep(frame_sleep_us * 4);
        } else {
            interruptible_usleep(frame_sleep_us);
        }
    }
    return nullptr;
}

int read_active_player_index() {
    int idx = -1;
    if (lock_memory()) {
        idx = shared_state->active_player_index;
        unlock_memory();
    }
    return idx;
}

int read_outcome() {
    int o = game_state::outcome_ongoing;
    if (lock_memory()) {
        o = shared_state->outcome;
        unlock_memory();
    }
    return o;
}

void *input_dispatcher_loop(void *) {
    while (running) {
        int ch = ERR;
        pthread_mutex_lock(&ncurses_lock);
        ch = getch();
        pthread_mutex_unlock(&ncurses_lock);
        if (ch != ERR) {
            if (ch == 'q' || ch == 'Q') {
                running = 0;
                send_quit_signals();
                for (int i = 0; i < game_state::max_players; ++i) {
                    pthread_mutex_lock(&player_buffers[i].lock);
                    pthread_cond_broadcast(&player_buffers[i].cv);
                    pthread_mutex_unlock(&player_buffers[i].lock);
                }
                break;
            }
            if (ch == '?' || ch == 'h' || ch == 'H') {
                show_help_overlay = !show_help_overlay;
                continue;
            }
            int outcome_value = read_outcome();
            if (outcome_value != game_state::outcome_ongoing) {
                continue;
            }
            int active = read_active_player_index();
            if (active >= 0 && active < game_state::max_players) {
                pthread_mutex_lock(&player_buffers[active].lock);
                player_buffers[active].pending_key = ch;
                pthread_cond_signal(&player_buffers[active].cv);
                pthread_mutex_unlock(&player_buffers[active].lock);
            }
        }
        interruptible_usleep(input_sleep_us);
    }
    return nullptr;
}

void *player_action_loop(void *arg) {
    int my_id = *static_cast<int *>(arg);
    while (running) {
        pthread_mutex_lock(&player_buffers[my_id].lock);
        while (running && player_buffers[my_id].pending_key == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&player_buffers[my_id].cv, &player_buffers[my_id].lock, &ts);
        }
        int ch = player_buffers[my_id].pending_key;
        player_buffers[my_id].pending_key = 0;
        pthread_mutex_unlock(&player_buffers[my_id].lock);
        if (!running) {
            break;
        }
        if (ch == 0) {
            interruptible_usleep(player_idle_sleep_us);
            continue;
        }
        int active = read_active_player_index();
        if (active != my_id) {
            continue;
        }
        handle_key_for_player(my_id, ch);
    }
    return nullptr;
}

void handle_winch(int) {
    resize_pending = 1;
}

void handle_exit_signal(int) {
    running = 0;
}

bool register_signals() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_winch;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGWINCH, &sa, nullptr) != 0) {
        print_errno("sigaction sigwinch failed");
        return false;
    }
    sa.sa_handler = handle_exit_signal;
    if (sigaction(SIGINT, &sa, nullptr) != 0) {
        print_errno("sigaction sigint failed");
        return false;
    }
    if (sigaction(SIGTERM, &sa, nullptr) != 0) {
        print_errno("sigaction sigterm failed");
        return false;
    }
    return true;
}

void init_color_pairs() {
    short theme_bg = COLOR_BLACK;
    short panel_bg = COLOR_BLACK;
    short hud_bg = COLOR_BLACK;
    short text_fg = COLOR_CYAN;
    short accent_fg = COLOR_GREEN;
    if (can_change_color() && COLORS >= 64) {
        const short c_bg = 40;
        const short c_panel = 41;
        const short c_hud = 42;
        const short c_text = 43;
        const short c_accent = 44;
        const short c_water = 45;
        init_color(c_bg, 20, 130, 90);
        init_color(c_panel, 30, 210, 150);
        init_color(c_hud, 40, 260, 180);
        init_color(c_text, 870, 980, 940);
        init_color(c_accent, 600, 980, 760);
        init_color(c_water, 560, 920, 980);
        theme_bg = c_bg;
        panel_bg = c_panel;
        hud_bg = c_hud;
        text_fg = c_text;
        accent_fg = c_accent;
        init_pair(pair_canvas, text_fg, theme_bg);
        init_pair(pair_panel_fill, text_fg, panel_bg);
        init_pair(pair_top_hud, text_fg, hud_bg);
        init_pair(pair_default, text_fg, theme_bg);
        init_pair(pair_border_normal, c_water, panel_bg);
        init_pair(pair_border_active, c_accent, panel_bg);
    } else {
        init_pair(pair_canvas, COLOR_CYAN, COLOR_BLACK);
        init_pair(pair_panel_fill, COLOR_CYAN, COLOR_BLACK);
        init_pair(pair_top_hud, COLOR_WHITE, COLOR_GREEN);
        init_pair(pair_default, COLOR_CYAN, COLOR_BLACK);
        init_pair(pair_border_normal, COLOR_CYAN, COLOR_BLACK);
        init_pair(pair_border_active, COLOR_GREEN, COLOR_BLACK);
    }
    init_pair(pair_border_dead, COLOR_RED, panel_bg);
    init_pair(pair_hp_high, COLOR_GREEN, panel_bg);
    init_pair(pair_hp_med, COLOR_CYAN, panel_bg);
    init_pair(pair_hp_low, COLOR_YELLOW, panel_bg);
    init_pair(pair_hp_critical, COLOR_RED, panel_bg);
    init_pair(pair_stamina, COLOR_BLUE, panel_bg);
    init_pair(pair_stamina_full, COLOR_GREEN, panel_bg);
    init_pair(pair_log, text_fg, panel_bg);
    init_pair(pair_status_ok, accent_fg, panel_bg);
    init_pair(pair_status_warn, COLOR_RED, panel_bg);
    init_pair(pair_artifact_solar, COLOR_YELLOW, panel_bg);
    init_pair(pair_artifact_lunar, COLOR_CYAN, panel_bg);
    init_pair(pair_artifact_eclipse, COLOR_MAGENTA, panel_bg);
    init_pair(pair_artifact_ground, COLOR_BLUE, panel_bg);
    init_pair(pair_command_bar, COLOR_WHITE, COLOR_GREEN);
    init_pair(pair_inv_empty, COLOR_BLACK, COLOR_BLACK);
    init_pair(pair_inv_splinter, COLOR_GREEN, panel_bg);
    init_pair(pair_inv_venom, COLOR_MAGENTA, panel_bg);
    init_pair(pair_inv_obsidian, COLOR_WHITE, panel_bg);
    init_pair(pair_inv_frost, COLOR_BLUE, panel_bg);
    init_pair(pair_inv_thunder, COLOR_YELLOW, panel_bg);
    init_pair(pair_inv_iron, COLOR_WHITE, panel_bg);
    init_pair(pair_inv_solar, COLOR_YELLOW, panel_bg);
    init_pair(pair_inv_lunar, COLOR_CYAN, panel_bg);
    init_pair(pair_inv_eclipse, COLOR_MAGENTA, panel_bg);
    init_pair(pair_panel_title, accent_fg, panel_bg);
    init_pair(pair_arena_title, COLOR_CYAN, panel_bg);
    init_pair(pair_banner, COLOR_WHITE, panel_bg);
    init_pair(pair_kill_counter, accent_fg, panel_bg);
    init_pair(pair_overlay_win, COLOR_GREEN, panel_bg);
    init_pair(pair_overlay_lose, COLOR_RED, panel_bg);
    init_pair(pair_overlay_quit, COLOR_CYAN, panel_bg);
    init_pair(pair_help, COLOR_WHITE, panel_bg);
}

bool init_tui() {
    if (initscr() == nullptr) {
        fprintf(stderr, "initscr failed\n");
        return false;
    }
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_color_pairs();
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    bkgd(COLOR_PAIR(pair_canvas));
    return true;
}

bool init_locks() {
    if (pthread_mutex_init(&ncurses_lock, nullptr) != 0) {
        print_errno("pthread_mutex_init ncurses_lock failed");
        return false;
    }
    for (int i = 0; i < game_state::max_players; ++i) {
        player_buffers[i].pending_key = 0;
        if (pthread_mutex_init(&player_buffers[i].lock, nullptr) != 0) {
            return false;
        }
        if (pthread_cond_init(&player_buffers[i].cv, nullptr) != 0) {
            return false;
        }
    }
    return true;
}

void destroy_locks() {
    pthread_mutex_destroy(&ncurses_lock);
    for (int i = 0; i < game_state::max_players; ++i) {
        pthread_mutex_destroy(&player_buffers[i].lock);
        pthread_cond_destroy(&player_buffers[i].cv);
    }
}

bool apply_party_size_to_state(int party_size) {
    if (!lock_memory()) {
        return false;
    }
    if (party_size < 1) {
        party_size = 1;
    }
    if (party_size > game_state::max_players) {
        party_size = game_state::max_players;
    }
    int new_speed = 100 / party_size;
    if (new_speed < 1) {
        new_speed = 1;
    }
    for (int i = 0; i < game_state::max_players; ++i) {
        if (i < party_size) {
            shared_state->player_speed[i] = new_speed;
            if (shared_state->player_hp[i] <= 0 && shared_state->player_max_hp[i] > 0) {
                shared_state->player_hp[i] = shared_state->player_max_hp[i];
            }
        } else {
            shared_state->player_hp[i] = 0;
            shared_state->player_max_hp[i] = 0;
            shared_state->player_speed[i] = 0;
            shared_state->player_stamina[i] = 0;
            shared_state->player_damage[i] = 0;
            for (int s = 0; s < game_state::inventory_slots; ++s) {
                shared_state->player_primary_inventory[i][s] = 0;
                shared_state->long_term_storage[i][s] = 0;
            }
        }
    }
    shared_state->active_player_count = party_size;
    if (shared_state->active_player_index >= party_size) {
        shared_state->active_player_index = 0;
    }
    snprintf(shared_state->action_log, sizeof(shared_state->action_log),
             "party of %d ready (speed %d). slay %d enemies to win.",
             party_size, new_speed, game_state::kills_required_to_win);
    unlock_memory();
    return true;
}

int prompt_party_size(int argc, char **argv) {
    int chosen = 0;
    if (argc >= 2) {
        chosen = atoi(argv[1]);
    }
    if (chosen < 1 || chosen > game_state::max_players) {
        const char *env_value = getenv("PARTY_SIZE");
        if (env_value != nullptr) {
            chosen = atoi(env_value);
        }
    }
    if (chosen < 1 || chosen > game_state::max_players) {
        printf("\n");
        printf("============================================================\n");
        printf("                    C H R O N O   R I F T                   \n");
        printf("============================================================\n");
        printf("\n");
        printf("select party size [1-4]: ");
        fflush(stdout);
        char buffer[32];
        if (fgets(buffer, sizeof(buffer), stdin) != nullptr) {
            chosen = atoi(buffer);
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

bool spawn_threads() {
    if (pthread_create(&render_thread, nullptr, render_loop, nullptr) != 0) {
        print_errno("pthread_create render failed");
        return false;
    }
    if (pthread_create(&input_dispatcher_thread, nullptr, input_dispatcher_loop, nullptr) != 0) {
        print_errno("pthread_create dispatcher failed");
        running = 0;
        pthread_join(render_thread, nullptr);
        return false;
    }
    for (int i = 0; i < chosen_party_size; ++i) {
        player_thread_ids[i] = i;
        if (pthread_create(&player_threads[i], nullptr, player_action_loop, &player_thread_ids[i]) != 0) {
            print_errno("pthread_create player thread failed");
            running = 0;
            for (int j = 0; j < i; ++j) {
                pthread_mutex_lock(&player_buffers[j].lock);
                pthread_cond_broadcast(&player_buffers[j].cv);
                pthread_mutex_unlock(&player_buffers[j].lock);
                pthread_join(player_threads[j], nullptr);
            }
            pthread_join(input_dispatcher_thread, nullptr);
            pthread_join(render_thread, nullptr);
            return false;
        }
    }
    return true;
}

void join_threads() {
    pthread_join(render_thread, nullptr);
    pthread_join(input_dispatcher_thread, nullptr);
    for (int i = 0; i < chosen_party_size; ++i) {
        pthread_mutex_lock(&player_buffers[i].lock);
        pthread_cond_broadcast(&player_buffers[i].cv);
        pthread_mutex_unlock(&player_buffers[i].lock);
        pthread_join(player_threads[i], nullptr);
    }
}

void cleanup_tui_once() {
    endwin();
}

void cleanup_resources() {
    destroy_locks();
    unmap_shared_memory();
    close_shared_fd();
}

void *outcome_watch_loop(void *) {
    while (running) {
        int o = read_outcome();
        if (o != game_state::outcome_ongoing) {
            running = 0;
            for (int i = 0; i < game_state::max_players; ++i) {
                pthread_mutex_lock(&player_buffers[i].lock);
                pthread_cond_broadcast(&player_buffers[i].cv);
                pthread_mutex_unlock(&player_buffers[i].lock);
            }
            break;
        }
        interruptible_usleep(500000);
    }
    return nullptr;
}

int main(int argc, char **argv) {
    chosen_party_size = prompt_party_size(argc, argv);
    if (!register_signals()) {
        return 1;
    }
    if (!init_locks()) {
        return 1;
    }
    if (!open_shared_memory()) {
        cleanup_resources();
        return 1;
    }
    if (!map_shared_memory()) {
        cleanup_resources();
        return 1;
    }
    if (!register_hip_pid_immediately()) {
        cleanup_resources();
        return 1;
    }
    if (!apply_party_size_to_state(chosen_party_size)) {
        cleanup_resources();
        return 1;
    }
    if (!init_tui()) {
        cleanup_resources();
        return 1;
    }
    if (!spawn_threads()) {
        cleanup_tui_once();
        cleanup_resources();
        return 1;
    }
    pthread_t outcome_thread;
    if (pthread_create(&outcome_thread, nullptr, outcome_watch_loop, nullptr) != 0) {
        print_errno("pthread_create outcome watch failed");
    }
    join_threads();
    pthread_join(outcome_thread, nullptr);
    cleanup_tui_once();
    cleanup_resources();
    return 0;
}
