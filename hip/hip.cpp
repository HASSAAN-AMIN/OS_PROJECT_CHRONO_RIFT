#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <ncurses.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../gamestate.h"

using namespace std;

const char *shared_memory_name = "/chrono_rift_game_state";
const int max_stat_value = 100;
const useconds_t frame_sleep_us = 16666;
const int color_border = 1;
const int color_player_hp = 2;
const int color_player_stamina = 3;
const int color_enemy_hp = 4;
const int color_enemy_stamina = 5;
const int color_artifact = 6;
const int color_empty_slot = 7;
const int color_solar_slot = 8;
const int color_lunar_slot = 9;
const int color_weapon_slot = 10;
const int player_turn_stamina = 100;
const int enemy_stamina_cap = 150;
const int player_base_damage = 10;
const int player_exhaust_damage = 10;
const int player_skip_stamina = 50;
const int player_heal_amount = 178;
const int player_max_hp = 1780;
const int splinter_stick_damage = 18;
const int venom_dagger_damage = 32;
const int iron_halberd_damage = 48;
const int solar_core_damage = 95;
const int lunar_blade_damage = 85;
const int action_none = 0;
const int action_send_ultimate_signal = 1;
const int action_send_stun_signal = 2;
const int solar_core_inventory_id = game_state::solar_core_id;
const int lunar_blade_inventory_id = game_state::lunar_blade_id;
const int player_stun_damage = 15;
bool swap_happened_last = false;

struct hip_snapshot {
    int player_hp[game_state::max_players];
    int player_stamina[game_state::max_players];
    time_t player_stun_end_time[game_state::max_players];
    int enemy_hp[game_state::max_enemies];
    int enemy_stamina[game_state::max_enemies];
    time_t stun_end_time[game_state::max_enemies];
    int player_primary_inventory[game_state::max_players][game_state::inventory_slots];
    int long_term_storage[game_state::max_players][game_state::inventory_slots];
    int solar_core_holder;
    int lunar_blade_holder;
    int eclipse_relic_holder;
    int current_dropped_weapon;
    char action_log[256];
};

struct window_set {
    WINDOW *left_panel;
    WINDOW *center_panel;
    WINDOW *right_panel;
    WINDOW *bottom_bar;
    int rows;
    int cols;
};

int shared_memory_fd = -1;
game_state *shared_state = nullptr;
volatile sig_atomic_t running = 1;
volatile sig_atomic_t full_redraw_requested = 0;
volatile sig_atomic_t resize_requested = 0;
pthread_t render_thread;
pthread_t input_thread;
bool ncurses_ready = false;
bool endwin_called = false;
window_set windows = {nullptr, nullptr, nullptr, nullptr, 0, 0};
pthread_mutex_t ncurses_lock = PTHREAD_MUTEX_INITIALIZER;
const int action_history_capacity = 128;
char action_history[action_history_capacity][256];
int action_history_start = 0;
int action_history_count = 0;
int action_scroll_offset = 0;
char last_action_seen[256] = "";

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

bool register_hip_pid() {
    while (sem_wait(&shared_state->state_lock) != 0) {
        if (errno == EINTR) {
            continue;
        }
        print_errno("sem_wait failed");
        return false;
    }
    shared_state->hip_pid = getpid();
    if (sem_post(&shared_state->state_lock) != 0) {
        print_errno("sem_post failed");
        return false;
    }
    return true;
}

void close_shared_memory_fd() {
    if (shared_memory_fd >= 0) {
        if (close(shared_memory_fd) != 0) {
            print_errno("close failed");
        }
        shared_memory_fd = -1;
    }
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

bool lock_memory() {
    if (shared_state == nullptr) {
        return false;
    }
    while (sem_wait(&shared_state->state_lock) != 0) {
        if (errno == EINTR) {
            if (!running) {
                return false;
            }
            continue;
        }
        print_errno("sem_wait failed");
        return false;
    }
    return true;
}

bool unlock_memory() {
    if (shared_state == nullptr) {
        return false;
    }
    if (sem_post(&shared_state->state_lock) != 0) {
        print_errno("sem_post failed");
        return false;
    }
    return true;
}

bool copy_snapshot(hip_snapshot *snapshot) {
    if (snapshot == nullptr) {
        return false;
    }
    if (!lock_memory()) {
        return false;
    }
    for (int i = 0; i < game_state::max_players; ++i) {
        snapshot->player_hp[i] = shared_state->player_hp[i];
        snapshot->player_stamina[i] = shared_state->player_stamina[i];
        snapshot->player_stun_end_time[i] = shared_state->player_stun_end_time[i];
        for (int j = 0; j < game_state::inventory_slots; ++j) {
            snapshot->player_primary_inventory[i][j] = shared_state->player_primary_inventory[i][j];
            snapshot->long_term_storage[i][j] = shared_state->long_term_storage[i][j];
        }
    }
    for (int i = 0; i < game_state::max_enemies; ++i) {
        snapshot->enemy_hp[i] = shared_state->enemy_hp[i];
        snapshot->enemy_stamina[i] = shared_state->enemy_stamina[i];
        snapshot->stun_end_time[i] = shared_state->stun_end_time[i];
    }
    snapshot->solar_core_holder = shared_state->solar_core_holder;
    snapshot->lunar_blade_holder = shared_state->lunar_blade_holder;
    snapshot->eclipse_relic_holder = shared_state->eclipse_relic_holder;
    snapshot->current_dropped_weapon = shared_state->current_dropped_weapon;
    snprintf(snapshot->action_log, sizeof(snapshot->action_log), "%s", shared_state->action_log);
    if (!unlock_memory()) {
        return false;
    }
    return true;
}

int find_turn_player_locked() {
    for (int i = 0; i < game_state::max_players; ++i) {
        if (shared_state->player_hp[i] > 0 && shared_state->player_stamina[i] >= player_turn_stamina) {
            return i;
        }
    }
    return -1;
}

int collect_living_enemies_locked(int *enemy_indices, int capacity) {
    int count = 0;
    for (int i = 0; i < game_state::max_enemies && count < capacity; ++i) {
        if (shared_state->enemy_hp[i] > 0) {
            enemy_indices[count++] = i;
        }
    }
    return count;
}

int pick_random_enemy_locked(int *enemy_indices, int enemy_count) {
    if (enemy_count <= 0) {
        return -1;
    }
    int random_index = rand() % enemy_count;
    return enemy_indices[random_index];
}

int weapon_size_from_id(int weapon_id) {
    if (weapon_id == game_state::splinter_stick_id) {
        return game_state::splinter_stick;
    }
    if (weapon_id == game_state::venom_dagger_id) {
        return game_state::venom_dagger;
    }
    if (weapon_id == game_state::iron_halberd_id) {
        return game_state::iron_halberd;
    }
    if (weapon_id == game_state::solar_core_id) {
        return game_state::solar_core;
    }
    if (weapon_id == game_state::lunar_blade_id) {
        return game_state::lunar_blade;
    }
    return 0;
}

const char *weapon_name_from_id(int weapon_id) {
    if (weapon_id == game_state::splinter_stick_id) {
        return "splinter";
    }
    if (weapon_id == game_state::venom_dagger_id) {
        return "venom";
    }
    if (weapon_id == game_state::iron_halberd_id) {
        return "halberd";
    }
    if (weapon_id == game_state::solar_core_id) {
        return "solar_core";
    }
    if (weapon_id == game_state::lunar_blade_id) {
        return "lunar_blade";
    }
    return "unknown";
}

const char *weapon_label_from_id(int weapon_id) {
    if (weapon_id == game_state::weapon_none) {
        return "[ empty  ]";
    }
    if (weapon_id == game_state::splinter_stick_id) {
        return "[splinter]";
    }
    if (weapon_id == game_state::solar_core_id) {
        return "[solar_core]";
    }
    if (weapon_id == game_state::lunar_blade_id) {
        return "[lunar_blade]";
    }
    if (weapon_id == game_state::venom_dagger_id) {
        return "[venomdagr]";
    }
    if (weapon_id == game_state::iron_halberd_id) {
        return "[halberd  ]";
    }
    return "[unknown  ]";
}

int weapon_color_from_id(int weapon_id) {
    if (weapon_id == game_state::weapon_none) {
        return color_empty_slot;
    }
    if (weapon_id == game_state::solar_core_id) {
        return color_solar_slot;
    }
    if (weapon_id == game_state::lunar_blade_id) {
        return color_lunar_slot;
    }
    if (weapon_id == game_state::splinter_stick_id) {
        return color_weapon_slot;
    }
    if (weapon_id == game_state::venom_dagger_id) {
        return color_weapon_slot;
    }
    if (weapon_id == game_state::iron_halberd_id) {
        return color_weapon_slot;
    }
    return color_weapon_slot;
}

int weapon_attr_from_id(int weapon_id) {
    if (weapon_id == game_state::weapon_none) {
        return A_DIM;
    }
    if (weapon_id == game_state::solar_core_id) {
        return A_BOLD;
    }
    if (weapon_id == game_state::lunar_blade_id) {
        return A_BOLD;
    }
    return A_NORMAL;
}

int find_contiguous_empty_slots(const int *row, int slot_count, int required_slots) {
    int run = 0;
    for (int i = 0; i < slot_count; ++i) {
        if (row[i] == 0) {
            run++;
            if (run >= required_slots) {
                return i - required_slots + 1;
            }
        } else {
            run = 0;
        }
    }
    return -1;
}

void write_weapon_slots(int *row, int start_index, int slot_count, int weapon_id) {
    for (int i = 0; i < slot_count; ++i) {
        row[start_index + i] = weapon_id;
    }
}

void clear_weapon_slots(int *row, int start_index, int slot_count) {
    for (int i = 0; i < slot_count; ++i) {
        row[start_index + i] = 0;
    }
}

bool find_oldest_weapon_block(int player_id, int *weapon_id, int *start_index, int *slot_count) {
    for (int i = 0; i < game_state::inventory_slots; ++i) {
        int value = shared_state->player_primary_inventory[player_id][i];
        if (value == 0) {
            continue;
        }
        int run = 1;
        int j = i + 1;
        while (j < game_state::inventory_slots && shared_state->player_primary_inventory[player_id][j] == value) {
            run++;
            j++;
        }
        *weapon_id = value;
        *start_index = i;
        *slot_count = run;
        return true;
    }
    return false;
}

bool move_weapon_to_long_term(int player_id, int weapon_id, int slot_count) {
    int target_index = find_contiguous_empty_slots(
        shared_state->long_term_storage[player_id],
        game_state::inventory_slots,
        slot_count
    );
    if (target_index < 0) {
        return false;
    }
    write_weapon_slots(shared_state->long_term_storage[player_id], target_index, slot_count, weapon_id);
    return true;
}

bool swap_to_long_term(int player_id) {
    int weapon_id = 0;
    int start_index = 0;
    int slot_count = 0;
    if (!find_oldest_weapon_block(player_id, &weapon_id, &start_index, &slot_count)) {
        return false;
    }
    if (!move_weapon_to_long_term(player_id, weapon_id, slot_count)) {
        return false;
    }
    clear_weapon_slots(shared_state->player_primary_inventory[player_id], start_index, slot_count);
    swap_happened_last = true;
    snprintf(
        shared_state->action_log,
        sizeof(shared_state->action_log),
        "swapped [%s] to storage",
        weapon_name_from_id(weapon_id)
    );
    return true;
}

bool allocate_inventory_iterative(int player_id, int weapon_id) {
    if (player_id < 0 || player_id >= game_state::max_players) {
        return false;
    }
    int weapon_size = weapon_size_from_id(weapon_id);
    if (weapon_size <= 0) {
        return false;
    }
    for (int attempt = 0; attempt < game_state::inventory_slots; ++attempt) {
        int start_index = find_contiguous_empty_slots(
            shared_state->player_primary_inventory[player_id],
            game_state::inventory_slots,
            weapon_size
        );
        if (start_index >= 0) {
            write_weapon_slots(shared_state->player_primary_inventory[player_id], start_index, weapon_size, weapon_id);
            return true;
        }
        if (!swap_to_long_term(player_id)) {
            return false;
        }
    }
    return false;
}

bool allocate_inventory(int player_id, int weapon_id) {
    swap_happened_last = false;
    return allocate_inventory_iterative(player_id, weapon_id);
}

void write_pickup_log(int player_id, int weapon_id, bool success) {
    if (success) {
        snprintf(
            shared_state->action_log,
            sizeof(shared_state->action_log),
            "player %d picked weapon %d",
            player_id + 1,
            weapon_id
        );
    } else {
        snprintf(
            shared_state->action_log,
            sizeof(shared_state->action_log),
            "player %d failed pickup weapon %d",
            player_id + 1,
            weapon_id
        );
    }
}

int random_drop_weapon_id() {
    int roll = rand() % 3;
    if (roll == 0) {
        return game_state::splinter_stick_id;
    }
    if (roll == 1) {
        return game_state::venom_dagger_id;
    }
    return game_state::iron_halberd_id;
}

void assign_drop_on_enemy_death_locked(int enemy_id) {
    if (enemy_id < 0 || enemy_id >= game_state::max_enemies) {
        return;
    }
    if (shared_state->enemy_hp[enemy_id] > 0) {
        return;
    }
    shared_state->current_dropped_weapon = random_drop_weapon_id();
}

int weapon_damage_from_id(int weapon_id) {
    if (weapon_id == game_state::splinter_stick_id) {
        return splinter_stick_damage;
    }
    if (weapon_id == game_state::venom_dagger_id) {
        return venom_dagger_damage;
    }
    if (weapon_id == game_state::iron_halberd_id) {
        return iron_halberd_damage;
    }
    if (weapon_id == game_state::solar_core_id) {
        return solar_core_damage;
    }
    if (weapon_id == game_state::lunar_blade_id) {
        return lunar_blade_damage;
    }
    return player_base_damage;
}

int highest_equipped_weapon_damage_locked(int player_id) {
    int highest_damage = player_base_damage;
    for (int i = 0; i < game_state::inventory_slots; ++i) {
        int weapon_id = shared_state->player_primary_inventory[player_id][i];
        int weapon_damage = weapon_damage_from_id(weapon_id);
        if (weapon_damage > highest_damage) {
            highest_damage = weapon_damage;
        }
    }
    return highest_damage;
}

void apply_strike_locked(int player_id) {
    int enemy_indices[game_state::max_enemies];
    int enemy_count = collect_living_enemies_locked(enemy_indices, game_state::max_enemies);
    int target_enemy = pick_random_enemy_locked(enemy_indices, enemy_count);
    if (target_enemy < 0) {
        return;
    }
    int next_hp = shared_state->enemy_hp[target_enemy] - player_base_damage;
    shared_state->enemy_hp[target_enemy] = clamp_value(next_hp, 0, 99999);
    assign_drop_on_enemy_death_locked(target_enemy);
    shared_state->player_stamina[player_id] = 0;
}

void apply_weapon_attack_locked(int player_id) {
    int enemy_indices[game_state::max_enemies];
    int enemy_count = collect_living_enemies_locked(enemy_indices, game_state::max_enemies);
    int target_enemy = pick_random_enemy_locked(enemy_indices, enemy_count);
    if (target_enemy < 0) {
        return;
    }
    int attack_damage = highest_equipped_weapon_damage_locked(player_id);
    int next_hp = shared_state->enemy_hp[target_enemy] - attack_damage;
    shared_state->enemy_hp[target_enemy] = clamp_value(next_hp, 0, 99999);
    assign_drop_on_enemy_death_locked(target_enemy);
    snprintf(
        shared_state->action_log,
        sizeof(shared_state->action_log),
        "player %d weapon hit enemy %d for %d",
        player_id + 1,
        target_enemy + 1,
        attack_damage
    );
    shared_state->player_stamina[player_id] = 0;
}

void apply_exhaust_locked(int player_id) {
    int enemy_indices[game_state::max_enemies];
    int enemy_count = collect_living_enemies_locked(enemy_indices, game_state::max_enemies);
    int target_enemy = pick_random_enemy_locked(enemy_indices, enemy_count);
    if (target_enemy < 0) {
        return;
    }
    int next_stamina = shared_state->enemy_stamina[target_enemy] - player_exhaust_damage;
    shared_state->enemy_stamina[target_enemy] = clamp_value(next_stamina, 0, enemy_stamina_cap);
    shared_state->player_stamina[player_id] = 0;
}

void apply_heal_locked(int player_id) {
    int next_hp = shared_state->player_hp[player_id] + player_heal_amount;
    shared_state->player_hp[player_id] = clamp_value(next_hp, 0, player_max_hp);
    shared_state->player_stamina[player_id] = 0;
}

void apply_skip_locked(int player_id) {
    shared_state->player_stamina[player_id] = player_skip_stamina;
}

void apply_pickup_drop_locked(int player_id) {
    int dropped_weapon = shared_state->current_dropped_weapon;
    bool success = allocate_inventory(player_id, dropped_weapon);
    shared_state->current_dropped_weapon = 0;
    shared_state->player_stamina[player_id] = 0;
    if (!swap_happened_last) {
        write_pickup_log(player_id, dropped_weapon, success);
    }
}

bool player_has_artifact_locked(int player_id, int artifact_id) {
    for (int i = 0; i < game_state::inventory_slots; ++i) {
        if (shared_state->player_primary_inventory[player_id][i] == artifact_id) {
            return true;
        }
    }
    return false;
}

int apply_ultimate_locked(int player_id) {
    bool has_solar = player_has_artifact_locked(player_id, solar_core_inventory_id);
    bool has_lunar = player_has_artifact_locked(player_id, lunar_blade_inventory_id);
    if (!has_solar || !has_lunar) {
        snprintf(
            shared_state->action_log,
            sizeof(shared_state->action_log),
            "player %d ultimate failed: missing artifacts",
            player_id + 1
        );
        return action_none;
    }
    if (shared_state->asp_pid <= 0 || shared_state->arbiter_pid <= 0) {
        snprintf(
            shared_state->action_log,
            sizeof(shared_state->action_log),
            "player %d ultimate failed: process pids missing",
            player_id + 1
        );
        return action_none;
    }
    shared_state->player_stamina[player_id] = 0;
    return action_send_ultimate_signal;
}

int apply_stun_attack_locked(int player_id) {
    int enemy_indices[game_state::max_enemies];
    int enemy_count = collect_living_enemies_locked(enemy_indices, game_state::max_enemies);
    int target_enemy = pick_random_enemy_locked(enemy_indices, enemy_count);
    if (target_enemy < 0) {
        return action_none;
    }
    int next_hp = shared_state->enemy_hp[target_enemy] - player_stun_damage;
    shared_state->enemy_hp[target_enemy] = clamp_value(next_hp, 0, 99999);
    shared_state->player_stamina[player_id] = 0;
    snprintf(
        shared_state->action_log,
        sizeof(shared_state->action_log),
        "player %d used stun: all enemies frozen for 3s",
        player_id + 1
    );
    return action_send_stun_signal;
}

void handle_player_action(int key) {
    if (!lock_memory()) {
        return;
    }
    int action_code = action_none;
    pid_t asp_pid = 0;
    pid_t arbiter_pid = 0;
    int active_player = find_turn_player_locked();
    if (active_player < 0) {
        unlock_memory();
        return;
    }
    if (key == '1') {
        apply_weapon_attack_locked(active_player);
    } else if (key == '2') {
        apply_exhaust_locked(active_player);
    } else if (key == '3') {
        apply_heal_locked(active_player);
    } else if (key == '4') {
        apply_skip_locked(active_player);
    } else if (key == '5') {
        apply_pickup_drop_locked(active_player);
    } else if (key == '6') {
        action_code = apply_ultimate_locked(active_player);
    } else if (key == '7') {
        action_code = apply_stun_attack_locked(active_player);
    }
    if (action_code != action_none) {
        asp_pid = shared_state->asp_pid;
        arbiter_pid = shared_state->arbiter_pid;
    }
    bool unlocked = unlock_memory();
    if (!unlocked) {
        return;
    }
    if (action_code == action_send_ultimate_signal) {
        if (asp_pid > 0) {
            kill(asp_pid, SIGSTOP);
        }
        if (arbiter_pid > 0) {
            kill(arbiter_pid, SIGUSR1);
        }
    } else if (action_code == action_send_stun_signal) {
        if (asp_pid > 0) {
            kill(asp_pid, SIGSTOP);
        }
        if (arbiter_pid > 0) {
            kill(arbiter_pid, SIGUSR2);
        }
    }
}

int find_turn_player_from_snapshot(const hip_snapshot *snapshot) {
    for (int i = 0; i < game_state::max_players; ++i) {
        if (snapshot->player_hp[i] > 0 && snapshot->player_stamina[i] >= player_turn_stamina) {
            return i;
        }
    }
    return -1;
}

void destroy_window(WINDOW **target) {
    if (*target == nullptr) {
        return;
    }
    delwin(*target);
    *target = nullptr;
}

void destroy_windows() {
    destroy_window(&windows.left_panel);
    destroy_window(&windows.center_panel);
    destroy_window(&windows.right_panel);
    destroy_window(&windows.bottom_bar);
}

bool create_windows_for_size(int rows, int cols) {
    int bottom_h = 1;
    int panel_h = rows - bottom_h;
    int left_w = cols / 3;
    int center_w = cols / 3;
    int right_w = cols - left_w - center_w;
    if (left_w < 28) {
        left_w = 28;
    }
    if (center_w < 28) {
        center_w = 28;
    }
    right_w = cols - left_w - center_w;
    if (right_w < 28) {
        right_w = 28;
        center_w = cols - left_w - right_w;
    }
    if (center_w < 28) {
        center_w = 28;
        left_w = cols - center_w - right_w;
    }
    if (left_w < 28) {
        left_w = 28;
    }

    windows.left_panel = newwin(panel_h, left_w, 0, 0);
    windows.center_panel = newwin(panel_h, center_w, 0, left_w);
    windows.right_panel = newwin(panel_h, right_w, 0, left_w + center_w);
    windows.bottom_bar = newwin(bottom_h, cols, panel_h, 0);

    if (windows.left_panel == nullptr || windows.center_panel == nullptr || windows.right_panel == nullptr ||
        windows.bottom_bar == nullptr) {
        destroy_windows();
        return false;
    }

    windows.rows = rows;
    windows.cols = cols;
    return true;
}

bool ensure_windows() {
    int rows = 0;
    int cols = 0;
    getmaxyx(stdscr, rows, cols);
    if (rows < 20 || cols < 96) {
        erase();
        mvprintw(0, 0, "terminal too small: need at least 96x20");
        refresh();
        return false;
    }
    if (windows.left_panel != nullptr && windows.rows == rows && windows.cols == cols) {
        return true;
    }
    destroy_windows();
    return create_windows_for_size(rows, cols);
}

bool init_ncurses_ui() {
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    if (has_colors()) {
        start_color();
        init_pair(color_border, COLOR_CYAN, COLOR_BLACK);
        init_pair(color_player_hp, COLOR_GREEN, COLOR_BLACK);
        init_pair(color_player_stamina, COLOR_YELLOW, COLOR_BLACK);
        init_pair(color_enemy_hp, COLOR_RED, COLOR_BLACK);
        init_pair(color_enemy_stamina, COLOR_MAGENTA, COLOR_BLACK);
        init_pair(color_artifact, COLOR_WHITE, COLOR_BLACK);
        init_pair(color_empty_slot, COLOR_WHITE, COLOR_BLACK);
        init_pair(color_solar_slot, COLOR_YELLOW, COLOR_BLACK);
        init_pair(color_lunar_slot, COLOR_CYAN, COLOR_BLACK);
        init_pair(color_weapon_slot, COLOR_MAGENTA, COLOR_BLACK);
    }
    resize_requested = 0;
    full_redraw_requested = 1;
    if (!ensure_windows()) {
        return false;
    }
    pthread_mutex_lock(&ncurses_lock);
    ncurses_ready = true;
    pthread_mutex_unlock(&ncurses_lock);
    return true;
}

void shutdown_ncurses_ui() {
    pthread_mutex_lock(&ncurses_lock);
    if (endwin_called) {
        pthread_mutex_unlock(&ncurses_lock);
        return;
    }
    if (ncurses_ready) {
        destroy_windows();
        endwin();
    }
    endwin_called = true;
    ncurses_ready = false;
    pthread_mutex_unlock(&ncurses_lock);
}

bool ui_is_ready() {
    pthread_mutex_lock(&ncurses_lock);
    bool ready = ncurses_ready;
    pthread_mutex_unlock(&ncurses_lock);
    return ready;
}

void draw_bar(char *output, int output_size, int current_value, int max_value, int width) {
    if (output == nullptr || output_size < 4) {
        return;
    }
    int usable_width = width;
    if (usable_width < 1) {
        usable_width = 1;
    }
    int clean_max = max_value;
    if (clean_max < 1) {
        clean_max = 1;
    }
    int clean_value = clamp_value(current_value, 0, clean_max);
    int filled = (clean_value * usable_width) / clean_max;
    int total_size = usable_width + 3;
    if (total_size >= output_size) {
        usable_width = output_size - 4;
        if (usable_width < 1) {
            usable_width = 1;
        }
        filled = (clean_value * usable_width) / clean_max;
    }
    output[0] = '[';
    for (int i = 0; i < usable_width; ++i) {
        output[1 + i] = i < filled ? '#' : '.';
    }
    output[1 + usable_width] = ']';
    output[2 + usable_width] = '\0';
}

void draw_window_frame(WINDOW *target, const char *title) {
    int h = getmaxy(target);
    int w = getmaxx(target);
    if (h < 2 || w < 2) {
        return;
    }
    wattron(target, COLOR_PAIR(color_border) | A_BOLD);
    mvwaddch(target, 0, 0, ACS_ULCORNER);
    mvwaddch(target, 0, w - 1, ACS_URCORNER);
    mvwaddch(target, h - 1, 0, ACS_LLCORNER);
    mvwaddch(target, h - 1, w - 1, ACS_LRCORNER);
    for (int x = 1; x < w - 1; ++x) {
        mvwaddch(target, 0, x, ACS_HLINE);
        mvwaddch(target, h - 1, x, ACS_HLINE);
    }
    for (int y = 1; y < h - 1; ++y) {
        mvwaddch(target, y, 0, ACS_VLINE);
        mvwaddch(target, y, w - 1, ACS_VLINE);
    }
    int title_x = 2;
    if (title_x < w - 2) {
        mvwprintw(target, 0, title_x, " %s ", title);
    }
    wattroff(target, COLOR_PAIR(color_border) | A_BOLD);
}

void draw_stat_line(
    WINDOW *target, int row, const char *label, int value, int max_value, int bar_width, int color_pair_id
) {
    int w = getmaxx(target);
    int safe_width = bar_width;
    if (safe_width < 8) {
        safe_width = 8;
    }
    if (safe_width > w - 22) {
        safe_width = w - 22;
    }
    if (safe_width < 8) {
        safe_width = 8;
    }
    int clean = clamp_value(value, 0, max_value);
    char bar[128];
    draw_bar(bar, sizeof(bar), clean, max_value, safe_width);
    wattron(target, COLOR_PAIR(color_pair_id) | A_BOLD);
    mvwprintw(target, row, 2, "%s %3d/%3d %s", label, clean, max_value, bar);
    wattroff(target, COLOR_PAIR(color_pair_id) | A_BOLD);
}

void append_action_history_line(const char *line) {
    if (line == nullptr || line[0] == '\0') {
        return;
    }
    if (strncmp(last_action_seen, line, sizeof(last_action_seen)) == 0) {
        return;
    }
    snprintf(last_action_seen, sizeof(last_action_seen), "%s", line);
    int index = (action_history_start + action_history_count) % action_history_capacity;
    snprintf(action_history[index], sizeof(action_history[index]), "%s", line);
    if (action_history_count < action_history_capacity) {
        action_history_count++;
    } else {
        action_history_start = (action_history_start + 1) % action_history_capacity;
    }
}

int find_inventory_player(const hip_snapshot *snapshot) {
    for (int i = 0; i < game_state::max_players; ++i) {
        for (int j = 0; j < game_state::inventory_slots; ++j) {
            if (snapshot->player_primary_inventory[i][j] != 0) {
                return i;
            }
        }
    }
    int turn_player = find_turn_player_from_snapshot(snapshot);
    if (turn_player >= 0) {
        return turn_player;
    }
    for (int i = 0; i < game_state::max_players; ++i) {
        if (snapshot->player_hp[i] > 0) {
            return i;
        }
    }
    return 0;
}

void draw_inventory_grid(
    WINDOW *target, int start_row, int start_col, const int *slots, int row_count, int col_count, int col_width
) {
    int h = getmaxy(target);
    int w = getmaxx(target);
    for (int row = 0; row < row_count; ++row) {
        for (int col = 0; col < col_count; ++col) {
            int index = row * col_count + col;
            int weapon_id = slots[index];
            const char *label = weapon_label_from_id(weapon_id);
            int color_pair = weapon_color_from_id(weapon_id);
            int y = start_row + row;
            int x = start_col + col * col_width;
            if (y >= h - 1 || x >= w - 2) {
                continue;
            }
            wattron(target, COLOR_PAIR(color_pair) | A_REVERSE | A_BOLD);
            mvwprintw(target, y, x, "%-*.*s", col_width - 1, col_width - 1, label);
            wattroff(target, COLOR_PAIR(color_pair) | A_REVERSE | A_BOLD);
        }
    }
}

void draw_player_squad_panel(const hip_snapshot *snapshot) {
    werase(windows.left_panel);
    draw_window_frame(windows.left_panel, "player_squad");
    int h = getmaxy(windows.left_panel);
    int w = getmaxx(windows.left_panel);
    int row = 1;
    for (int i = 0; i < game_state::max_players; ++i) {
        if (row >= h - 6) {
            break;
        }
        int hp = clamp_value(snapshot->player_hp[i], 0, player_max_hp);
        int stamina = clamp_value(snapshot->player_stamina[i], 0, max_stat_value);
        int bar_width = w - 22;
        if (bar_width < 8) {
            bar_width = 8;
        }
        char hp_bar[96];
        char st_bar[96];
        draw_bar(hp_bar, sizeof(hp_bar), hp, player_max_hp, bar_width);
        draw_bar(st_bar, sizeof(st_bar), stamina, max_stat_value, bar_width);
        bool low_hp = hp > 0 && hp * 100 <= player_max_hp * 20;
        int hp_attr = COLOR_PAIR(color_player_hp) | A_BOLD;
        if (low_hp) {
            hp_attr = COLOR_PAIR(color_enemy_hp) | A_BOLD | A_BLINK;
        }
        wattron(windows.left_panel, A_BOLD);
        mvwprintw(windows.left_panel, row, 2, "p%d", i + 1);
        wattroff(windows.left_panel, A_BOLD);
        wattron(windows.left_panel, hp_attr);
        mvwprintw(windows.left_panel, row, 6, "hp %3d/%3d %s", hp, player_max_hp, hp_bar);
        wattroff(windows.left_panel, hp_attr);
        row++;
        wattron(windows.left_panel, COLOR_PAIR(color_player_stamina) | A_BOLD);
        mvwprintw(windows.left_panel, row, 6, "st %3d/%3d %s", stamina, max_stat_value, st_bar);
        wattroff(windows.left_panel, COLOR_PAIR(color_player_stamina) | A_BOLD);
        row++;
    }
    int active_player = find_inventory_player(snapshot);
    if (row < h - 6) {
        wattron(windows.left_panel, A_BOLD);
        mvwprintw(windows.left_panel, row++, 2, "inv p%d", active_player + 1);
        wattroff(windows.left_panel, A_BOLD);
        int col_width = (w - 4) / 5;
        if (col_width < 6) {
            col_width = 6;
        }
        draw_inventory_grid(
            windows.left_panel,
            row,
            2,
            snapshot->player_primary_inventory[active_player],
            4,
            5,
            col_width
        );
    }
}

int enemy_stamina_start_col(int width, int hp_bar_length, int st_bar_length) {
    int desired = 8 + hp_bar_length;
    int min_col = 6;
    int required = 1 + st_bar_length;
    int max_col = width - 2 - required;
    return clamp_value(desired, min_col, max_col);
}

void draw_enemy_forces_panel(const hip_snapshot *snapshot) {
    werase(windows.right_panel);
    draw_window_frame(windows.right_panel, "enemy_forces");
    const int enemy_max_hp = 230;
    int h = getmaxy(windows.right_panel);
    int w = getmaxx(windows.right_panel);
    int row = 1;
    for (int i = 0; i < game_state::max_enemies; ++i) {
        if (row >= h - 2) {
            break;
        }
        int hp = clamp_value(snapshot->enemy_hp[i], 0, enemy_max_hp);
        int st = clamp_value(snapshot->enemy_stamina[i], 0, enemy_stamina_cap);
        int bar_width = (w - 20) / 2;
        if (bar_width < 5) {
            bar_width = 5;
        }
        char hp_bar[64];
        char st_bar[64];
        draw_bar(hp_bar, sizeof(hp_bar), hp, enemy_max_hp, bar_width);
        draw_bar(st_bar, sizeof(st_bar), st, enemy_stamina_cap, bar_width);
        int hp_len = static_cast<int>(strlen(hp_bar));
        int st_len = static_cast<int>(strlen(st_bar));
        int st_col = enemy_stamina_start_col(w, hp_len, st_len);
        wattron(windows.right_panel, A_BOLD);
        mvwprintw(windows.right_panel, row, 2, "e%d", i + 1);
        wattroff(windows.right_panel, A_BOLD);
        wattron(windows.right_panel, COLOR_PAIR(color_enemy_hp) | A_BOLD);
        mvwprintw(windows.right_panel, row, 6, "h%s", hp_bar);
        wattroff(windows.right_panel, COLOR_PAIR(color_enemy_hp) | A_BOLD);
        wattron(windows.right_panel, COLOR_PAIR(color_enemy_stamina) | A_BOLD);
        mvwprintw(windows.right_panel, row, st_col, "s%s", st_bar);
        wattroff(windows.right_panel, COLOR_PAIR(color_enemy_stamina) | A_BOLD);
        row++;
    }
}

void draw_combat_arena_panel(const hip_snapshot *snapshot, unsigned long frame_id) {
    append_action_history_line(snapshot->action_log);
    werase(windows.center_panel);
    draw_window_frame(windows.center_panel, "combat_arena");
    int h = getmaxy(windows.center_panel);
    int w = getmaxx(windows.center_panel);
    time_t now = time(nullptr);
    wattron(windows.center_panel, A_BOLD);
    mvwprintw(windows.center_panel, 1, 2, "frame:%lu time:%ld", frame_id, static_cast<long>(now));
    wattroff(windows.center_panel, A_BOLD);
    int living_players = 0;
    int active_enemies = 0;
    for (int i = 0; i < game_state::max_players; ++i) {
        if (snapshot->player_hp[i] > 0) {
            living_players++;
        }
    }
    for (int i = 0; i < game_state::max_enemies; ++i) {
        if (snapshot->enemy_hp[i] > 0) {
            active_enemies++;
        }
    }
    mvwprintw(windows.center_panel, 2, 2, "alive %d/%d enemies %d/%d", living_players, game_state::max_players, active_enemies, game_state::max_enemies);
    wattron(windows.center_panel, COLOR_PAIR(color_artifact) | A_BOLD);
    mvwprintw(windows.center_panel, 4, 2, "global relics");
    mvwprintw(windows.center_panel, 5, 2, "solar_core holder: %d", snapshot->solar_core_holder);
    mvwprintw(windows.center_panel, 6, 2, "lunar_blade holder: %d", snapshot->lunar_blade_holder);
    wattroff(windows.center_panel, COLOR_PAIR(color_artifact) | A_BOLD);
    mvwprintw(windows.center_panel, 8, 2, "action log");
    int log_top = 9;
    int log_rows = h - log_top - 2;
    if (log_rows < 1) {
        log_rows = 1;
    }
    int max_scroll = action_history_count - 1;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    action_scroll_offset = clamp_value(action_scroll_offset, 0, max_scroll);
    int newest_index = action_history_count - 1 - action_scroll_offset;
    for (int row = 0; row < log_rows; ++row) {
        int history_index = newest_index - (log_rows - 1 - row);
        int y = log_top + row;
        if (y >= h - 1) {
            break;
        }
        if (history_index < 0 || history_index >= action_history_count) {
            continue;
        }
        int ring_index = (action_history_start + history_index) % action_history_capacity;
        mvwprintw(windows.center_panel, y, 2, "%-*.*s", w - 4, w - 4, action_history[ring_index]);
    }
    mvwprintw(windows.center_panel, h - 2, 2, "scroll up/down");
}

void draw_bottom_bar() {
    werase(windows.bottom_bar);
    wattron(windows.bottom_bar, A_REVERSE | A_BOLD);
    mvwprintw(
        windows.bottom_bar,
        0,
        0,
        " [1]strike [2]exhaust [3]heal [5]pickup [6]ultimate [7]stun [q]quit [up/down]logs "
    );
    wattroff(windows.bottom_bar, A_REVERSE | A_BOLD);
}

void handle_resize_locked() {
    resize_requested = 0;
    endwin();
    refresh();
    clear();
    destroy_windows();
    ensure_windows();
    clearok(stdscr, TRUE);
}

void render_all(const hip_snapshot *snapshot, unsigned long frame_id) {
    pthread_mutex_lock(&ncurses_lock);
    if (!ncurses_ready) {
        pthread_mutex_unlock(&ncurses_lock);
        return;
    }
    if (resize_requested) {
        handle_resize_locked();
    }
    if (!ensure_windows()) {
        pthread_mutex_unlock(&ncurses_lock);
        return;
    }
    if (full_redraw_requested) {
        full_redraw_requested = 0;
        clearok(stdscr, TRUE);
    }
    draw_player_squad_panel(snapshot);
    draw_combat_arena_panel(snapshot, frame_id);
    draw_enemy_forces_panel(snapshot);
    draw_bottom_bar();
    wnoutrefresh(windows.left_panel);
    wnoutrefresh(windows.center_panel);
    wnoutrefresh(windows.right_panel);
    wnoutrefresh(windows.bottom_bar);
    doupdate();
    pthread_mutex_unlock(&ncurses_lock);
}

void *render_loop(void *) {
    if (!init_ncurses_ui()) {
        running = 0;
        return nullptr;
    }
    unsigned long frame_id = 0;
    while (running) {
        hip_snapshot snapshot = {};
        if (!copy_snapshot(&snapshot)) {
            running = 0;
            break;
        }
        if (!running) {
            break;
        }
        render_all(&snapshot, frame_id++);
        while (usleep(frame_sleep_us) == -1 && errno == EINTR) {
            if (!running) {
                break;
            }
        }
    }
    return nullptr;
}

void *player_input_loop(void *) {
    while (running) {
        if (!ui_is_ready()) {
            usleep(10000);
            continue;
        }
        int key = ERR;
        pthread_mutex_lock(&ncurses_lock);
        if (ncurses_ready) {
            key = getch();
        }
        pthread_mutex_unlock(&ncurses_lock);
        if (key == 'q' || key == 'Q') {
            running = 0;
            break;
        }
        if (key == KEY_UP) {
            action_scroll_offset++;
        } else if (key == KEY_DOWN) {
            action_scroll_offset--;
            if (action_scroll_offset < 0) {
                action_scroll_offset = 0;
            }
        }
        if (key == '1' || key == '2' || key == '3' || key == '4' || key == '5' || key == '6' || key == '7') {
            handle_player_action(key);
        }
        usleep(10000);
    }
    return nullptr;
}

void handle_signal(int) {
    running = 0;
}

void handle_continue_signal(int) {
    full_redraw_requested = 1;
}

void handle_resize_signal(int) {
    resize_requested = 1;
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
    if (signal(SIGCONT, handle_continue_signal) == SIG_ERR) {
        fprintf(stderr, "failed to register sigcont handler\n");
        return false;
    }
    if (signal(SIGWINCH, handle_resize_signal) == SIG_ERR) {
        fprintf(stderr, "failed to register sigwinch handler\n");
        return false;
    }
    return true;
}

bool start_render_thread() {
    if (pthread_create(&render_thread, nullptr, render_loop, nullptr) != 0) {
        print_errno("pthread_create failed");
        return false;
    }
    printf("ui thread started\n");
    return true;
}

bool start_input_thread() {
    if (pthread_create(&input_thread, nullptr, player_input_loop, nullptr) != 0) {
        print_errno("pthread_create failed");
        return false;
    }
    printf("input thread started\n");
    return true;
}

void join_render_thread() {
    pthread_join(render_thread, nullptr);
}

void join_input_thread() {
    pthread_join(input_thread, nullptr);
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
    if (!register_hip_pid()) {
        cleanup();
        return 1;
    }
    if (!start_render_thread()) {
        cleanup();
        return 1;
    }
    if (!start_input_thread()) {
        running = 0;
        join_render_thread();
        shutdown_ncurses_ui();
        cleanup();
        return 1;
    }
    join_render_thread();
    join_input_thread();
    shutdown_ncurses_ui();
    cleanup();
    printf("hip exited cleanly\n");
    return 0;
}
