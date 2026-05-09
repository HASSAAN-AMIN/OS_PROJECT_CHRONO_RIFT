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
    WINDOW *status;
    WINDOW *players;
    WINDOW *enemies;
    WINDOW *inventory;
    WINDOW *action_log;
    int rows;
    int cols;
};

int shared_memory_fd = -1;
game_state *shared_state = nullptr;
volatile sig_atomic_t running = 1;
volatile sig_atomic_t full_redraw_requested = 0;
pthread_t render_thread;
pthread_t input_thread;
bool ncurses_ready = false;
window_set windows = {nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0};
pthread_mutex_t ncurses_lock = PTHREAD_MUTEX_INITIALIZER;

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
    int next_hp = shared_state->enemy_hp[target_enemy] - player_base_damage;
    shared_state->enemy_hp[target_enemy] = clamp_value(next_hp, 0, 99999);
    assign_drop_on_enemy_death_locked(target_enemy);
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

bool apply_ultimate_locked(int player_id) {
    bool has_solar = player_has_artifact_locked(player_id, solar_core_inventory_id);
    bool has_lunar = player_has_artifact_locked(player_id, lunar_blade_inventory_id);
    if (!has_solar || !has_lunar) {
        snprintf(
            shared_state->action_log,
            sizeof(shared_state->action_log),
            "player %d ultimate failed: missing artifacts",
            player_id + 1
        );
        return true;
    }
    pid_t asp_pid = shared_state->asp_pid;
    pid_t arbiter_pid = shared_state->arbiter_pid;
    if (asp_pid <= 0 || arbiter_pid <= 0) {
        snprintf(
            shared_state->action_log,
            sizeof(shared_state->action_log),
            "player %d ultimate failed: process pids missing",
            player_id + 1
        );
        return true;
    }
    shared_state->player_stamina[player_id] = 0;
    if (!unlock_memory()) {
        return true;
    }
    kill(asp_pid, SIGSTOP);
    kill(arbiter_pid, SIGUSR1);
    if (!lock_memory()) {
        return false;
    }
    return true;
}

void apply_stun_attack_locked(int player_id) {
    int enemy_indices[game_state::max_enemies];
    int enemy_count = collect_living_enemies_locked(enemy_indices, game_state::max_enemies);
    int target_enemy = pick_random_enemy_locked(enemy_indices, enemy_count);
    if (target_enemy < 0) {
        return;
    }
    int next_hp = shared_state->enemy_hp[target_enemy] - player_stun_damage;
    shared_state->enemy_hp[target_enemy] = clamp_value(next_hp, 0, 99999);
    pid_t asp_pid = shared_state->asp_pid;
    pid_t arbiter_pid = shared_state->arbiter_pid;
    if (asp_pid > 0) {
        kill(asp_pid, SIGSTOP);
    }
    if (arbiter_pid > 0) {
        kill(arbiter_pid, SIGUSR2);
    }
    shared_state->player_stamina[player_id] = 0;
    snprintf(
        shared_state->action_log,
        sizeof(shared_state->action_log),
        "player %d used stun: all enemies frozen for 3s",
        player_id + 1
    );
}

void handle_player_action(int key) {
    if (!lock_memory()) {
        return;
    }
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
        if (!apply_ultimate_locked(active_player)) {
            return;
        }
    } else if (key == '7') {
        apply_stun_attack_locked(active_player);
    }
    unlock_memory();
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
    destroy_window(&windows.status);
    destroy_window(&windows.players);
    destroy_window(&windows.enemies);
    destroy_window(&windows.inventory);
    destroy_window(&windows.action_log);
}

bool create_windows_for_size(int rows, int cols) {
    int status_h = 5;
    int mid_h = (rows - status_h) / 2;
    if (mid_h < 8) {
        mid_h = 8;
    }
    int bottom_h = rows - status_h - mid_h;
    if (bottom_h < 8) {
        bottom_h = 8;
    }
    int used_h = status_h + mid_h + bottom_h;
    if (used_h > rows) {
        bottom_h -= (used_h - rows);
    }

    int left_w = cols / 2;
    int right_w = cols - left_w;

    windows.status = newwin(status_h, cols, 0, 0);
    windows.players = newwin(mid_h, left_w, status_h, 0);
    windows.enemies = newwin(mid_h, right_w, status_h, left_w);
    windows.inventory = newwin(bottom_h, left_w, status_h + mid_h, 0);
    windows.action_log = newwin(bottom_h, right_w, status_h + mid_h, left_w);

    if (windows.status == nullptr || windows.players == nullptr || windows.enemies == nullptr ||
        windows.inventory == nullptr || windows.action_log == nullptr) {
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
    if (rows < 24 || cols < 90) {
        erase();
        mvprintw(0, 0, "terminal too small: need at least 90x24");
        refresh();
        return false;
    }
    if (windows.status != nullptr && windows.rows == rows && windows.cols == cols) {
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
        init_pair(color_empty_slot, COLOR_BLACK, COLOR_BLACK);
        init_pair(color_solar_slot, COLOR_YELLOW, COLOR_BLACK);
        init_pair(color_lunar_slot, COLOR_CYAN, COLOR_BLACK);
        init_pair(color_weapon_slot, COLOR_MAGENTA, COLOR_BLACK);
    }
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
    if (!ncurses_ready) {
        pthread_mutex_unlock(&ncurses_lock);
        return;
    }
    destroy_windows();
    endwin();
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
    wattron(target, COLOR_PAIR(color_border));
    box(target, 0, 0);
    wattroff(target, COLOR_PAIR(color_border));
    wattron(target, COLOR_PAIR(color_border) | A_BOLD);
    mvwprintw(target, 0, 2, " %s ", title);
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
    wattron(target, COLOR_PAIR(color_pair_id));
    mvwprintw(target, row, 2, "%s %3d/%3d %s", label, clean, max_value, bar);
    wattroff(target, COLOR_PAIR(color_pair_id));
}

void draw_system_status(const hip_snapshot *snapshot, unsigned long frame_id) {
    werase(windows.status);
    draw_window_frame(windows.status, "system status");
    time_t now = time(nullptr);
    mvwprintw(windows.status, 1, 2, "frame %lu", frame_id);
    mvwprintw(windows.status, 1, 20, "target fps 60");
    mvwprintw(windows.status, 1, 38, "time %ld", static_cast<long>(now));
    wattron(windows.status, COLOR_PAIR(color_artifact) | A_BOLD);
    mvwprintw(
        windows.status, 2, 2, "solar %d  lunar %d  eclipse %d",
        snapshot->solar_core_holder, snapshot->lunar_blade_holder, snapshot->eclipse_relic_holder
    );
    wattroff(windows.status, COLOR_PAIR(color_artifact) | A_BOLD);
    mvwprintw(windows.status, 3, 2, "press q to exit hip");
}

void draw_players(const hip_snapshot *snapshot) {
    werase(windows.players);
    draw_window_frame(windows.players, "player squad");
    int row = 1;
    for (int i = 0; i < game_state::max_players; ++i) {
        char hp_label[32];
        char stamina_label[32];
        snprintf(hp_label, sizeof(hp_label), "p%d hp", i + 1);
        snprintf(stamina_label, sizeof(stamina_label), "p%d st", i + 1);
        draw_stat_line(windows.players, row++, hp_label, snapshot->player_hp[i], player_max_hp, 18, color_player_hp);
        draw_stat_line(
            windows.players, row++, stamina_label, snapshot->player_stamina[i], max_stat_value, 18, color_player_stamina
        );
    }
}

void draw_enemies(const hip_snapshot *snapshot) {
    werase(windows.enemies);
    draw_window_frame(windows.enemies, "enemy bots");
    const int enemy_max_hp = 230;
    int h = 0;
    int w = 0;
    getmaxyx(windows.enemies, h, w);
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
        mvwprintw(windows.enemies, row, 2, "e%d", i + 1);
        wattron(windows.enemies, COLOR_PAIR(color_enemy_hp));
        mvwprintw(windows.enemies, row, 6, "h%s", hp_bar);
        wattroff(windows.enemies, COLOR_PAIR(color_enemy_hp));
        wattron(windows.enemies, COLOR_PAIR(color_enemy_stamina));
        mvwprintw(windows.enemies, row, 8 + static_cast<int>(strlen(hp_bar)), "s%s", st_bar);
        wattroff(windows.enemies, COLOR_PAIR(color_enemy_stamina));
        row++;
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
    for (int row = 0; row < row_count; ++row) {
        for (int col = 0; col < col_count; ++col) {
            int index = row * col_count + col;
            int weapon_id = slots[index];
            const char *label = weapon_label_from_id(weapon_id);
            int color_pair = weapon_color_from_id(weapon_id);
            int y = start_row + row;
            int x = start_col + col * col_width;
            if (y >= h - 1) {
                continue;
            }
            if (target == windows.inventory) {
                wattron(windows.inventory, COLOR_PAIR(color_pair) | A_REVERSE | A_BOLD);
                mvwprintw(windows.inventory, y, x, "%-*.*s", col_width - 1, col_width - 1, label);
                wattroff(windows.inventory, COLOR_PAIR(color_pair) | A_REVERSE | A_BOLD);
            } else {
                int attr = weapon_attr_from_id(weapon_id);
                wattron(target, COLOR_PAIR(color_pair) | attr);
                mvwprintw(target, y, x, "%-*.*s", col_width - 1, col_width - 1, label);
                wattroff(target, COLOR_PAIR(color_pair) | attr);
            }
        }
    }
}

void draw_inventory(const hip_snapshot *snapshot) {
    werase(windows.inventory);
    draw_window_frame(windows.inventory, "inventory tetris");
    int active_player = find_inventory_player(snapshot);
    int h = getmaxy(windows.inventory);
    int w = getmaxx(windows.inventory);
    int row_count = 4;
    int col_count = 5;
    int col_width = (w - 4) / col_count;
    if (col_width < 6) {
        col_width = 6;
    }
    mvwprintw(windows.inventory, 1, 2, "gear bag p%d", active_player + 1);
    draw_inventory_grid(
        windows.inventory,
        3,
        2,
        snapshot->player_primary_inventory[active_player],
        row_count,
        col_count,
        col_width
    );
    if (h >= 14) {
        mvwprintw(windows.inventory, 8, 2, "storage");
        draw_inventory_grid(
            windows.inventory,
            9,
            2,
            snapshot->long_term_storage[active_player],
            row_count,
            col_count,
            col_width
        );
    } else {
        mvwprintw(windows.inventory, h - 2, 2, "storage hidden: enlarge terminal");
    }
}

void draw_action_log(const hip_snapshot *snapshot, unsigned long frame_id) {
    werase(windows.action_log);
    draw_window_frame(windows.action_log, "combat action log");
    mvwprintw(windows.action_log, 1, 2, "frame pulse %lu", frame_id);
    wattron(windows.action_log, COLOR_PAIR(color_artifact) | A_BOLD);
    mvwprintw(windows.action_log, 2, 2, "solar core holder: %d", snapshot->solar_core_holder);
    mvwprintw(windows.action_log, 3, 2, "lunar blade holder: %d", snapshot->lunar_blade_holder);
    mvwprintw(windows.action_log, 4, 2, "eclipse relic holder: %d", snapshot->eclipse_relic_holder);
    wattroff(windows.action_log, COLOR_PAIR(color_artifact) | A_BOLD);
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
    mvwprintw(windows.action_log, 6, 2, "squad alive: %d/%d", living_players, game_state::max_players);
    mvwprintw(windows.action_log, 7, 2, "enemy active: %d/%d", active_enemies, game_state::max_enemies);
    mvwprintw(windows.action_log, 8, 2, "last: %s", snapshot->action_log);
    int turn_player = find_turn_player_from_snapshot(snapshot);
    if (turn_player >= 0) {
        mvwprintw(
            windows.action_log,
            9,
            2,
            "player %d turn: 1)strike 2)exhaust 3)heal 4)skip",
            turn_player + 1
        );
        mvwprintw(windows.action_log, 10, 2, "pickup: 5)pick up drop");
        mvwprintw(windows.action_log, 11, 2, "ultimate: 6)freeze bots");
        mvwprintw(windows.action_log, 12, 2, "stun: 7)stun attack");
    } else {
        mvwprintw(windows.action_log, 9, 2, "waiting: no player has 100 stamina yet");
        mvwprintw(windows.action_log, 10, 2, "pickup: ready on turn only");
        mvwprintw(windows.action_log, 11, 2, "ultimate: ready on turn only");
        mvwprintw(windows.action_log, 12, 2, "stun: ready on turn only");
    }
    mvwprintw(windows.action_log, 13, 2, "drop id: %d", snapshot->current_dropped_weapon);
    mvwprintw(windows.action_log, 14, 2, "input: q quits hip");
}

void render_all(const hip_snapshot *snapshot, unsigned long frame_id) {
    pthread_mutex_lock(&ncurses_lock);
    if (!ncurses_ready) {
        pthread_mutex_unlock(&ncurses_lock);
        return;
    }
    if (!ensure_windows()) {
        pthread_mutex_unlock(&ncurses_lock);
        return;
    }
    if (full_redraw_requested) {
        full_redraw_requested = 0;
        clearok(stdscr, TRUE);
    }
    draw_system_status(snapshot, frame_id);
    draw_players(snapshot);
    draw_enemies(snapshot);
    draw_inventory(snapshot);
    draw_action_log(snapshot, frame_id);
    wnoutrefresh(windows.status);
    wnoutrefresh(windows.players);
    wnoutrefresh(windows.enemies);
    wnoutrefresh(windows.inventory);
    wnoutrefresh(windows.action_log);
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
        usleep(frame_sleep_us);
    }
    shutdown_ncurses_ui();
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
            shutdown_ncurses_ui();
            break;
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
    shutdown_ncurses_ui();
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
        cleanup();
        return 1;
    }
    join_render_thread();
    join_input_thread();
    cleanup();
    printf("hip exited cleanly\n");
    return 0;
}
