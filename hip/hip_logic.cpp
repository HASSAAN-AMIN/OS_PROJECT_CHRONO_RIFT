#include "hip_logic.h"

const char *shared_memory_name = "/chrono_rift_game_state";

const useconds_t frame_sleep_us = 1000000 / 60;
const useconds_t input_sleep_us = 16000;
const useconds_t player_idle_sleep_us = 60000;
const int pending_target_auto = -999999;
const long long hit_flash_duration = 6;
const long long death_flash_duration = 48;
const long long shake_duration = 4;
const long long full_stamina_flash_duration = 3;
const long long drop_flash_duration = 10;

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
const int pair_enemy_dead_purple = 37;
const int pair_bg_canvas = 38;
const int pair_bg_player = 39;
const int pair_bg_arena = 40;
const int pair_bg_enemy = 41;
const int pair_bg_footer = 42;
const int pair_title_player = 43;
const int pair_title_enemy = 44;
const int pair_title_arena = 45;
const int pair_bg_player_box = 46;
const int pair_bg_enemy_box = 47;
const int pair_bg_timeline = 48;
const int pair_bg_log = 49;
const int pair_bg_status = 50;
const int pair_bg_artifact = 51;
const int pair_bg_banner_1 = 52;
const int pair_bg_banner_2 = 53;
const int pair_bg_banner_3 = 54;
const int pair_bg_banner_4 = 55;
const int pair_bg_banner_5 = 56;
const int pair_border_v1 = 57;
const int pair_border_v2 = 58;
const int pair_border_v3 = 59;
const int pair_border_v4 = 60;
const int pair_border_v5 = 61;
const int pair_border_v6 = 62;
const int pair_border_v7 = 63;

int shared_memory_fd = -1;
game_state *shared_state = nullptr;

volatile sig_atomic_t running = 1;
volatile sig_atomic_t resize_pending = 0;

pthread_mutex_t ncurses_lock;

player_input_buffer player_buffers[game_state::max_players];
pthread_t render_thread;
pthread_t input_dispatcher_thread;
pthread_t player_threads[game_state::max_players];
int player_thread_ids[game_state::max_players];
int chosen_party_size = 0;
int show_help_overlay = 0;
int show_swap_overlay = 0;
int swap_overlay_player_id = -1;
int swap_overlay_option_count = 0;
int swap_overlay_weapon_ids[game_state::weapon_id_max] = {0};
char swap_overlay_option_keys[game_state::weapon_id_max] = {0};
int pending_swap_weapon_id[game_state::max_players] = {-1, -1, -1, -1};
unsigned long render_frame_counter = 0;
long long current_frame = 0;
long long hit_flash_frame[13] = {0};
long long death_flash_frame[13] = {0};
long long full_stamina_flash_frame[13] = {0};
long long stamina_ready_pulse_frame[game_state::max_players] = {0};
long long enemy_spawn_flash_frame[game_state::max_enemies] = {0};
long long kill_pop_frame = -1000000;
long long deadlock_log_frame = -1000000;
long long ultimate_shockwave_frame = -1000000;
long long pickup_sweep_frame[game_state::max_players] = {0};
int prev_hp[13] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
int prev_stamina[13] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
int prev_enemy_dead_count[game_state::max_enemies] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
int prev_enemy_display_id[game_state::max_enemies] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
int prev_player_inventory_fill[game_state::max_players] = {-1, -1, -1, -1};
int prev_total_kills = -1;
int prev_ultimate_active = 0;
int prev_stun_active = 0;
int prev_dropped_weapon = 0;
long long weapon_drop_flash_frame = -1000;
time_t last_event_time = 0;
char last_event_label[64] = {0};
queue<int> enemy_target_queue;
int cheat_drop_cursor = 0;
int entity_hit_frames[13] = {0};
int inventory_new_item_frames[13][game_state::inventory_slots] = {{0}};
int previous_inventory_items[13][game_state::inventory_slots] = {{0}};
int action_banner_frames = 0;
int action_banner_pair = pair_status_warn;
char action_banner_text[128] = {0};
void initialize_animation_state() {
    for (int i = 0; i < 13; ++i) {
        hit_flash_frame[i] = -1000000;
        death_flash_frame[i] = -1000000;
        full_stamina_flash_frame[i] = -1000000;
        prev_hp[i] = -1;
        prev_stamina[i] = -1;
        entity_hit_frames[i] = 0;
        for (int s = 0; s < game_state::inventory_slots; ++s) {
            inventory_new_item_frames[i][s] = 0;
            previous_inventory_items[i][s] = 0;
        }
    }
    for (int i = 0; i < game_state::max_players; ++i) {
        stamina_ready_pulse_frame[i] = -1000000;
        pickup_sweep_frame[i] = -1000000;
        prev_player_inventory_fill[i] = -1;
    }
    for (int i = 0; i < game_state::max_enemies; ++i) {
        prev_enemy_dead_count[i] = -1;
        prev_enemy_display_id[i] = -1;
        enemy_spawn_flash_frame[i] = -1000000;
    }
    current_frame = 0;
    kill_pop_frame = -1000000;
    deadlock_log_frame = -1000000;
    ultimate_shockwave_frame = -1000000;
    prev_total_kills = -1;
    prev_ultimate_active = 0;
    prev_stun_active = 0;
    prev_dropped_weapon = 0;
    weapon_drop_flash_frame = -1000000;
    action_banner_frames = 0;
    action_banner_pair = pair_status_warn;
    memset(action_banner_text, 0, sizeof(action_banner_text));
}


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

vector<int> collect_living_enemy_slots_locked() {
    vector<int> slots;
    int enemy_count = shared_state->active_enemy_count;
    if (enemy_count < 0) {
        enemy_count = 0;
    }
    if (enemy_count > game_state::max_enemies) {
        enemy_count = game_state::max_enemies;
    }
    for (int i = 0; i < enemy_count; ++i) {
        if (shared_state->enemy_hp[i] > 0) {
            slots.push_back(i);
        }
    }
    return slots;
}

void clear_enemy_target_queue_locked() {
    queue<int> empty;
    enemy_target_queue.swap(empty);
}

void normalize_enemy_target_queue_locked() {
    vector<int> living = collect_living_enemy_slots_locked();
    if (living.empty()) {
        clear_enemy_target_queue_locked();
        return;
    }
    vector<int> alive_map(game_state::max_enemies, 0);
    for (int idx : living) {
        alive_map[idx] = 1;
    }
    vector<int> ordered;
    vector<int> seen(game_state::max_enemies, 0);
    queue<int> current = enemy_target_queue;
    while (!current.empty()) {
        int v = current.front();
        current.pop();
        if (v >= 0 && v < game_state::max_enemies && alive_map[v] && !seen[v]) {
            ordered.push_back(v);
            seen[v] = 1;
        }
    }
    for (int idx : living) {
        if (!seen[idx]) {
            ordered.push_back(idx);
            seen[idx] = 1;
        }
    }
    clear_enemy_target_queue_locked();
    for (int idx : ordered) {
        enemy_target_queue.push(idx);
    }
}

int peek_target_enemy_locked() {
    normalize_enemy_target_queue_locked();
    if (enemy_target_queue.empty()) {
        return -1;
    }
    return enemy_target_queue.front();
}

int next_target_enemy_locked() {
    normalize_enemy_target_queue_locked();
    if (enemy_target_queue.empty()) {
        return -1;
    }
    int target = enemy_target_queue.front();
    enemy_target_queue.pop();
    enemy_target_queue.push(target);
    return target;
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
    vector<int> primary_backup(game_state::inventory_slots);
    vector<int> storage_backup(game_state::inventory_slots);
    for (int i = 0; i < game_state::inventory_slots; ++i) {
        primary_backup[i] = primary[i];
        storage_backup[i] = storage[i];
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
            return -1;
        }
        int evict_weapon = primary[evict_start];
        int storage_start = find_contiguous_free_in_array(storage, evict_size);
        if (storage_start < 0) {
            for (int i = 0; i < game_state::inventory_slots; ++i) {
                primary[i] = primary_backup[i];
                storage[i] = storage_backup[i];
            }
            return -1;
        }
        place_weapon(storage, storage_start, evict_size, evict_weapon);
        zero_run(primary, evict_start, evict_size);
    }
    for (int i = 0; i < game_state::inventory_slots; ++i) {
        primary[i] = primary_backup[i];
        storage[i] = storage_backup[i];
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

int pending_target_for_action_locked(int action) {
    if (action == game_state::action_strike || action == game_state::action_exhaust ||
        action == game_state::action_use_weapon) {
        return next_target_enemy_locked();
    }
    return -1;
}

void submit_player_action_request(int player_id, int action, int forced_target = pending_target_auto) {
    if (!lock_memory()) {
        return;
    }
    if (player_id < 0 || player_id >= game_state::max_players) {
        unlock_memory();
        return;
    }
    if (player_id >= shared_state->active_player_count) {
        unlock_memory();
        return;
    }
    if (shared_state->player_stamina[player_id] < game_state::player_max_stamina) {
        unlock_memory();
        return;
    }
    int target = forced_target;
    if (target == pending_target_auto) {
        target = pending_target_for_action_locked(action);
    }
    shared_state->player_pending_action[player_id] = action;
    shared_state->player_pending_target[player_id] = target;
    snprintf(shared_state->last_event, sizeof(shared_state->last_event), "p%d_action_%d", player_id + 1, action);
    unlock_memory();
}

void perform_strike(int player_id) {
    submit_player_action_request(player_id, game_state::action_strike);
}

void perform_use_weapon(int player_id) {
    submit_player_action_request(player_id, game_state::action_use_weapon);
}

void cycle_target_enemy_for_player(int player_id) {
    if (!lock_memory()) {
        return;
    }
    if (player_id < 0 || player_id >= game_state::max_players || player_id >= shared_state->active_player_count) {
        unlock_memory();
        return;
    }
    int active = shared_state->active_player_index;
    if (active != player_id) {
        unlock_memory();
        return;
    }
    int next = next_target_enemy_locked();
    if (next >= 0) {
        snprintf(shared_state->last_event, sizeof(shared_state->last_event), "target_enemy_%d", next + 1);
    }
    unlock_memory();
}

void perform_cheat_damage(int player_id) {
    if (!lock_memory()) {
        return;
    }
    if (player_id < 0 || player_id >= game_state::max_players || player_id >= shared_state->active_player_count) {
        unlock_memory();
        return;
    }
    int target = peek_target_enemy_locked();
    if (target < 0 || target >= game_state::max_enemies || shared_state->enemy_hp[target] <= 0) {
        target = find_first_living_enemy_locked();
    }
    if (target >= 0 && target < game_state::max_enemies) {
        shared_state->enemy_hp[target] -= 50;
        snprintf(shared_state->last_event, sizeof(shared_state->last_event), "cheat_k_enemy_%d", target + 1);
    }
    unlock_memory();
}

void perform_cheat_drop_collectible(int player_id) {
    if (!lock_memory()) {
        return;
    }
    if (player_id < 0 || player_id >= game_state::max_players || player_id >= shared_state->active_player_count) {
        unlock_memory();
        return;
    }
    if (shared_state->current_dropped_weapon != 0) {
        unlock_memory();
        return;
    }
    int pool[] = {
        game_state::splinter_stick_id,
        game_state::venom_dagger_id,
        game_state::obsidian_axe_id,
        game_state::frostbow_id,
        game_state::thunderstaff_id,
        game_state::iron_halberd_id,
        game_state::solar_core_id,
        game_state::lunar_blade_id,
        game_state::eclipse_relic_id
    };
    int pool_size = (int)(sizeof(pool) / sizeof(pool[0]));
    if (cheat_drop_cursor < 0 || cheat_drop_cursor >= pool_size) {
        cheat_drop_cursor = 0;
    }
    int weapon_id = pool[cheat_drop_cursor];
    cheat_drop_cursor = (cheat_drop_cursor + 1) % pool_size;
    shared_state->current_dropped_weapon = weapon_id;
    if (weapon_id == game_state::solar_core_id) {
        shared_state->solar_core_holder = -1;
    } else if (weapon_id == game_state::lunar_blade_id) {
        shared_state->lunar_blade_holder = -1;
    } else if (weapon_id == game_state::eclipse_relic_id) {
        shared_state->eclipse_relic_holder = -1;
        shared_state->eclipse_relic_present = 1;
    }
    snprintf(shared_state->last_event, sizeof(shared_state->last_event), "cheat_j_drop_%d", weapon_id);
    unlock_memory();
}

void perform_cheat_ultimate_loadout(int player_id) {
    if (!lock_memory()) {
        return;
    }
    if (player_id < 0 || player_id >= game_state::max_players || player_id >= shared_state->active_player_count) {
        unlock_memory();
        return;
    }
    int active = shared_state->active_player_index;
    if (active != player_id) {
        unlock_memory();
        return;
    }
    for (int s = 0; s < game_state::inventory_slots; ++s) {
        shared_state->player_primary_inventory[player_id][s] = 0;
    }
    for (int s = 0; s < game_state::solar_core_slots && s < game_state::inventory_slots; ++s) {
        shared_state->player_primary_inventory[player_id][s] = game_state::solar_core_id;
    }
    int lunar_start = game_state::solar_core_slots;
    for (int s = 0; s < game_state::lunar_blade_slots; ++s) {
        int idx = lunar_start + s;
        if (idx >= 0 && idx < game_state::inventory_slots) {
            shared_state->player_primary_inventory[player_id][idx] = game_state::lunar_blade_id;
        }
    }
    shared_state->solar_core_holder = player_id;
    shared_state->lunar_blade_holder = player_id;
    snprintf(shared_state->last_event, sizeof(shared_state->last_event), "cheat_h_ultimate_p%d", player_id + 1);
    unlock_memory();
}

void perform_exhaust(int player_id) {
    submit_player_action_request(player_id, game_state::action_exhaust);
}

void perform_heal(int player_id) {
    submit_player_action_request(player_id, game_state::action_heal);
}

void perform_skip(int player_id) {
    submit_player_action_request(player_id, game_state::action_skip);
}

void perform_pickup(int player_id) {
    submit_player_action_request(player_id, game_state::action_pickup);
}

void perform_swap_in(int player_id) {
    int chosen_weapon = pending_swap_weapon_id[player_id];
    if (chosen_weapon <= 0) {
        return;
    }
    submit_player_action_request(player_id, game_state::action_swap_in, chosen_weapon);
    pending_swap_weapon_id[player_id] = -1;
}

void clear_swap_overlay_state() {
    show_swap_overlay = 0;
    swap_overlay_player_id = -1;
    swap_overlay_option_count = 0;
    for (int i = 0; i < game_state::weapon_id_max; ++i) {
        swap_overlay_weapon_ids[i] = 0;
        swap_overlay_option_keys[i] = 0;
    }
}

void open_swap_overlay_for_player(int player_id) {
    if (!lock_memory()) {
        return;
    }
    if (player_id < 0 || player_id >= game_state::max_players || player_id >= shared_state->active_player_count) {
        unlock_memory();
        return;
    }
    if (shared_state->player_stamina[player_id] < game_state::player_max_stamina) {
        unlock_memory();
        return;
    }
    bool seen[game_state::weapon_id_max + 1] = {false};
    int count = 0;
    for (int i = 0; i < game_state::inventory_slots && count < game_state::weapon_id_max; ++i) {
        int weapon_id = shared_state->long_term_storage[player_id][i];
        if (weapon_id <= 0 || weapon_id > game_state::weapon_id_max) {
            continue;
        }
        if (seen[weapon_id]) {
            continue;
        }
        seen[weapon_id] = true;
        swap_overlay_weapon_ids[count] = weapon_id;
        swap_overlay_option_keys[count] = static_cast<char>('a' + count);
        count++;
    }
    unlock_memory();
    pending_swap_weapon_id[player_id] = -1;
    swap_overlay_option_count = count;
    swap_overlay_player_id = player_id;
    show_swap_overlay = (count > 0) ? 1 : 0;
    if (show_swap_overlay) {
        show_help_overlay = 0;
    }
}

bool handle_swap_overlay_key_for_player(int player_id, int ch) {
    if (!show_swap_overlay || player_id != swap_overlay_player_id) {
        return false;
    }
    if (ch == 27 || ch == '0' || ch == 'x' || ch == 'X') {
        clear_swap_overlay_state();
        return true;
    }
    int normalized = ch;
    if (normalized >= 'A' && normalized <= 'Z') {
        normalized = normalized - 'A' + 'a';
    }
    for (int i = 0; i < swap_overlay_option_count; ++i) {
        if (normalized == swap_overlay_option_keys[i]) {
            pending_swap_weapon_id[player_id] = swap_overlay_weapon_ids[i];
            clear_swap_overlay_state();
            perform_swap_in(player_id);
            return true;
        }
    }
    return true;
}

void perform_ultimate(int player_id) {
    submit_player_action_request(player_id, game_state::action_ultimate);
}

void send_quit_signals() {
    pid_t arbiter_pid_local = 0;
    pid_t asp_pid_local = 0;
    if (lock_memory()) {
        arbiter_pid_local = shared_state->arbiter_pid;
        asp_pid_local = shared_state->asp_pid;
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
    if (handle_swap_overlay_key_for_player(player_id, ch)) {
        return;
    }
    switch (ch) {
        case '1': perform_strike(player_id); break;
        case '2': perform_exhaust(player_id); break;
        case '3': perform_heal(player_id); break;
        case '4': perform_skip(player_id); break;
        case '5': perform_pickup(player_id); break;
        case '6': perform_ultimate(player_id); break;
        case '7': break;
        case '8': perform_use_weapon(player_id); break;
        case '9': open_swap_overlay_for_player(player_id); break;
        case 't':
        case 'T':
        case '\t': cycle_target_enemy_for_player(player_id); break;
        case 'h':
        case 'H': perform_cheat_ultimate_loadout(player_id); break;
        case 'j':
        case 'J': perform_cheat_drop_collectible(player_id); break;
        case 'k':
        case 'K': perform_cheat_damage(player_id); break;
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
    snap.total_kills = shared_state->total_kills;
    snap.outcome = shared_state->outcome;
    snap.roll_number = shared_state->roll_number;
    snap.arbiter_pid = shared_state->arbiter_pid;
    snap.asp_pid = shared_state->asp_pid;
    snap.hip_pid = shared_state->hip_pid;
    memcpy(snap.action_log, shared_state->action_log, sizeof(snap.action_log));
    snap.action_log[sizeof(snap.action_log) - 1] = '\0';
    memcpy(snap.last_event, shared_state->last_event, sizeof(snap.last_event));
    snap.last_event[sizeof(snap.last_event) - 1] = '\0';

    snap.target_enemy = peek_target_enemy_locked();

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
        snap.enemy_display_id[i] = shared_state->enemy_display_id[i];
        snap.enemy_dead_count[i] = shared_state->enemy_dead_count[i];
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
            if (ch == '?' && !show_swap_overlay) {
                show_help_overlay = !show_help_overlay;
                continue;
            }
            int outcome_value = read_outcome();
            if (outcome_value != game_state::outcome_ongoing) {
                continue;
            }
            int active = read_active_player_index();
            if (show_swap_overlay && active != swap_overlay_player_id) {
                clear_swap_overlay_state();
            }
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
