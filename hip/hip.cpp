#include <cerrno>
#include <csignal>
#include <cstdio>
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

const int strike_cost = 30;
const int exhaust_cost = 40;
const int heal_cost = 50;
const int ultimate_cost = 100;
const int stun_cost = 60;
const int heal_amount = 100;
const int player_stamina_cap_local = 100;
const int enemy_stamina_cap_local = 150;

const int pair_default = 1;
const int pair_border_normal = 2;
const int pair_border_active = 3;
const int pair_border_dead = 4;
const int pair_hp_high = 5;
const int pair_hp_med = 6;
const int pair_hp_low = 7;
const int pair_hp_critical = 8;
const int pair_stamina = 9;
const int pair_log = 10;
const int pair_status_ok = 11;
const int pair_status_warn = 12;
const int pair_artifact_solar = 13;
const int pair_artifact_lunar = 14;
const int pair_artifact_ground = 15;
const int pair_command_bar = 16;
const int pair_inv_empty = 17;
const int pair_inv_splinter = 18;
const int pair_inv_venom = 19;
const int pair_inv_iron = 20;
const int pair_inv_solar = 21;
const int pair_inv_lunar = 22;
const int pair_panel_title = 23;
const int pair_arena_title = 24;

int shared_memory_fd = -1;
game_state *shared_state = nullptr;

volatile sig_atomic_t running = 1;
volatile sig_atomic_t resize_pending = 0;

pthread_mutex_t ncurses_lock;

int observed_player_max_hp[game_state::max_players] = {0};
int observed_enemy_max_hp[game_state::max_enemies] = {0};

struct entity_snapshot {
    int hp;
    int stamina;
    int max_hp;
    int max_stamina;
    time_t stun_end;
    int inventory[game_state::inventory_slots];
    bool active;
    bool dead;
    bool turn;
};

struct world_snapshot {
    entity_snapshot players[game_state::max_players];
    entity_snapshot enemies[game_state::max_enemies];
    int active_player_count;
    int active_enemy_count;
    int solar_core_holder;
    int lunar_blade_holder;
    int current_dropped_weapon;
    int control_player;
    int target_enemy;
    char action_log[256];
    pid_t arbiter_pid;
    pid_t asp_pid;
    pid_t hip_pid;
    bool ultimate_active;
    bool stun_active;
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
        case game_state::splinter_stick_id: return game_state::splinter_stick;
        case game_state::venom_dagger_id: return game_state::venom_dagger;
        case game_state::iron_halberd_id: return game_state::iron_halberd;
        case game_state::solar_core_id: return game_state::solar_core;
        case game_state::lunar_blade_id: return game_state::lunar_blade;
        default: return 1;
    }
}

const char *weapon_short_name(int weapon_id) {
    switch (weapon_id) {
        case game_state::splinter_stick_id: return "splinter stick";
        case game_state::venom_dagger_id: return "venom dagger";
        case game_state::iron_halberd_id: return "iron halberd";
        case game_state::solar_core_id: return "solar core";
        case game_state::lunar_blade_id: return "lunar blade";
        default: return "fists";
    }
}

char weapon_letter(int weapon_id) {
    switch (weapon_id) {
        case game_state::splinter_stick_id: return 'S';
        case game_state::venom_dagger_id: return 'V';
        case game_state::iron_halberd_id: return 'I';
        case game_state::solar_core_id: return 'O';
        case game_state::lunar_blade_id: return 'L';
        default: return ' ';
    }
}

int weapon_color_pair_for(int weapon_id) {
    switch (weapon_id) {
        case game_state::splinter_stick_id: return pair_inv_splinter;
        case game_state::venom_dagger_id: return pair_inv_venom;
        case game_state::iron_halberd_id: return pair_inv_iron;
        case game_state::solar_core_id: return pair_inv_solar;
        case game_state::lunar_blade_id: return pair_inv_lunar;
        default: return pair_inv_empty;
    }
}

int find_best_weapon_locked(int player_index) {
    int best_id = 0;
    int best_dmg = 0;
    for (int s = 0; s < game_state::inventory_slots; ++s) {
        int w = shared_state->player_primary_inventory[player_index][s];
        if (w == 0) {
            continue;
        }
        int dmg = weapon_damage_value(w);
        if (dmg > best_dmg) {
            best_dmg = dmg;
            best_id = w;
        }
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

int allocate_weapon_iterative_locked(int player_index, int weapon_id) {
    for (int s = 0; s < game_state::inventory_slots; ++s) {
        if (shared_state->player_primary_inventory[player_index][s] == 0) {
            shared_state->player_primary_inventory[player_index][s] = weapon_id;
            return s;
        }
    }
    int storage_slot = -1;
    for (int s = 0; s < game_state::inventory_slots; ++s) {
        if (shared_state->long_term_storage[player_index][s] == 0) {
            storage_slot = s;
            break;
        }
    }
    if (storage_slot < 0) {
        return -1;
    }
    int displaced = shared_state->player_primary_inventory[player_index][0];
    shared_state->long_term_storage[player_index][storage_slot] = displaced;
    int last_slot = game_state::inventory_slots - 1;
    for (int s = 0; s < last_slot; ++s) {
        shared_state->player_primary_inventory[player_index][s] = shared_state->player_primary_inventory[player_index][s + 1];
    }
    shared_state->player_primary_inventory[player_index][last_slot] = 0;
    for (int s = 0; s < game_state::inventory_slots; ++s) {
        if (shared_state->player_primary_inventory[player_index][s] == 0) {
            shared_state->player_primary_inventory[player_index][s] = weapon_id;
            return s;
        }
    }
    return -1;
}

int select_active_player_locked() {
    int best = -1;
    int best_stam = -1;
    for (int i = 0; i < game_state::max_players; ++i) {
        if (shared_state->player_hp[i] <= 0) {
            continue;
        }
        int s = shared_state->player_stamina[i];
        if (s > best_stam) {
            best_stam = s;
            best = i;
        }
    }
    if (best < 0) {
        for (int i = 0; i < game_state::max_players; ++i) {
            if (shared_state->player_hp[i] > 0) {
                return i;
            }
        }
    }
    return best;
}

int select_active_enemy_locked() {
    int best = -1;
    int best_stam = -1;
    for (int i = 0; i < game_state::max_enemies; ++i) {
        if (shared_state->enemy_hp[i] <= 0) {
            continue;
        }
        int s = shared_state->enemy_stamina[i];
        if (s > best_stam) {
            best_stam = s;
            best = i;
        }
    }
    if (best < 0) {
        for (int i = 0; i < game_state::max_enemies; ++i) {
            if (shared_state->enemy_hp[i] > 0) {
                return i;
            }
        }
    }
    return best;
}

int find_first_living_enemy_locked() {
    for (int i = 0; i < game_state::max_enemies; ++i) {
        if (shared_state->enemy_hp[i] > 0) {
            return i;
        }
    }
    return -1;
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
        int pattern = enemy_index % 3;
        if (pattern == 0) {
            drop = game_state::iron_halberd_id;
        } else if (pattern == 1) {
            drop = game_state::venom_dagger_id;
        } else {
            drop = game_state::splinter_stick_id;
        }
    }
    shared_state->current_dropped_weapon = drop;
}

void perform_strike() {
    if (!lock_memory()) {
        return;
    }
    int p = select_active_player_locked();
    if (p < 0) {
        unlock_memory();
        return;
    }
    if (shared_state->player_stamina[p] < strike_cost) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d strike: insufficient stamina (%d/%d)",
                 p + 1, shared_state->player_stamina[p], strike_cost);
        unlock_memory();
        return;
    }
    int target = find_first_living_enemy_locked();
    if (target < 0) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d strike: no living enemies", p + 1);
        unlock_memory();
        return;
    }
    int weapon_id = find_best_weapon_locked(p);
    int dmg = weapon_damage_value(weapon_id);
    shared_state->player_stamina[p] -= strike_cost;
    int new_hp = shared_state->enemy_hp[target] - dmg;
    if (new_hp < 0) {
        new_hp = 0;
    }
    shared_state->enemy_hp[target] = new_hp;
    if (new_hp == 0) {
        distribute_drop_on_enemy_death_locked(target);
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d strike: hit enemy %d for %d (%s) - enemy died, dropped %s",
                 p + 1, target + 1, dmg, weapon_short_name(weapon_id),
                 weapon_short_name(shared_state->current_dropped_weapon));
    } else {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d strike: hit enemy %d for %d damage (%s)",
                 p + 1, target + 1, dmg, weapon_short_name(weapon_id));
    }
    unlock_memory();
}

void perform_exhaust() {
    if (!lock_memory()) {
        return;
    }
    int p = select_active_player_locked();
    if (p < 0) {
        unlock_memory();
        return;
    }
    if (shared_state->player_stamina[p] < exhaust_cost) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d exhaust: insufficient stamina (%d/%d)",
                 p + 1, shared_state->player_stamina[p], exhaust_cost);
        unlock_memory();
        return;
    }
    int target = find_first_living_enemy_locked();
    if (target < 0) {
        unlock_memory();
        return;
    }
    shared_state->player_stamina[p] -= exhaust_cost;
    shared_state->enemy_stamina[target] = 0;
    snprintf(shared_state->action_log, sizeof(shared_state->action_log),
             "player %d exhaust: drained enemy %d stamina to 0",
             p + 1, target + 1);
    unlock_memory();
}

void perform_heal() {
    if (!lock_memory()) {
        return;
    }
    int p = select_active_player_locked();
    if (p < 0) {
        unlock_memory();
        return;
    }
    if (shared_state->player_stamina[p] < heal_cost) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d heal: insufficient stamina (%d/%d)",
                 p + 1, shared_state->player_stamina[p], heal_cost);
        unlock_memory();
        return;
    }
    shared_state->player_stamina[p] -= heal_cost;
    shared_state->player_hp[p] += heal_amount;
    snprintf(shared_state->action_log, sizeof(shared_state->action_log),
             "player %d heal: restored %d hp (now %d)",
             p + 1, heal_amount, shared_state->player_hp[p]);
    unlock_memory();
}

void perform_skip() {
    if (!lock_memory()) {
        return;
    }
    int p = select_active_player_locked();
    if (p < 0) {
        unlock_memory();
        return;
    }
    shared_state->player_stamina[p] = 0;
    snprintf(shared_state->action_log, sizeof(shared_state->action_log),
             "player %d skip: stamina reset to 0", p + 1);
    unlock_memory();
}

void perform_pickup() {
    if (!lock_memory()) {
        return;
    }
    int p = select_active_player_locked();
    if (p < 0) {
        unlock_memory();
        return;
    }
    int dropped = shared_state->current_dropped_weapon;
    if (dropped == 0) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d pickup: nothing on the ground", p + 1);
        unlock_memory();
        return;
    }
    int slot = allocate_weapon_iterative_locked(p, dropped);
    if (slot < 0) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d pickup: inventory and storage full", p + 1);
        unlock_memory();
        return;
    }
    shared_state->current_dropped_weapon = 0;
    if (dropped == game_state::solar_core_id) {
        shared_state->solar_core_holder = p;
    }
    if (dropped == game_state::lunar_blade_id) {
        shared_state->lunar_blade_holder = p;
    }
    snprintf(shared_state->action_log, sizeof(shared_state->action_log),
             "player %d pickup: acquired %s in slot %d",
             p + 1, weapon_short_name(dropped), slot + 1);
    unlock_memory();
}

void perform_ultimate() {
    if (!lock_memory()) {
        return;
    }
    int p = select_active_player_locked();
    if (p < 0) {
        unlock_memory();
        return;
    }
    if (shared_state->player_stamina[p] < ultimate_cost) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d ultimate: insufficient stamina (%d/%d)",
                 p + 1, shared_state->player_stamina[p], ultimate_cost);
        unlock_memory();
        return;
    }
    bool has_solar = inventory_contains_locked(p, game_state::solar_core_id);
    bool has_lunar = inventory_contains_locked(p, game_state::lunar_blade_id);
    if (!has_solar || !has_lunar) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d ultimate: requires solar core and lunar blade", p + 1);
        unlock_memory();
        return;
    }
    shared_state->player_stamina[p] = 0;
    pid_t asp_pid_local = shared_state->asp_pid;
    pid_t arbiter_pid_local = shared_state->arbiter_pid;
    snprintf(shared_state->action_log, sizeof(shared_state->action_log),
             "player %d ultimate: arena freezing for 10s", p + 1);
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

void perform_stun() {
    if (!lock_memory()) {
        return;
    }
    int p = select_active_player_locked();
    if (p < 0) {
        unlock_memory();
        return;
    }
    if (shared_state->player_stamina[p] < stun_cost) {
        snprintf(shared_state->action_log, sizeof(shared_state->action_log),
                 "player %d stun: insufficient stamina (%d/%d)",
                 p + 1, shared_state->player_stamina[p], stun_cost);
        unlock_memory();
        return;
    }
    int target = find_first_living_enemy_locked();
    if (target < 0) {
        unlock_memory();
        return;
    }
    shared_state->player_stamina[p] = 0;
    shared_state->stun_end_time[target] = time(nullptr) + 3;
    pid_t asp_pid_local = shared_state->asp_pid;
    pid_t arbiter_pid_local = shared_state->arbiter_pid;
    snprintf(shared_state->action_log, sizeof(shared_state->action_log),
             "player %d stun: enemy %d stunned for 3s", p + 1, target + 1);
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

void handle_key(int ch) {
    switch (ch) {
        case '1':
            perform_strike();
            break;
        case '2':
            perform_exhaust();
            break;
        case '3':
            perform_heal();
            break;
        case '4':
            perform_skip();
            break;
        case '5':
            perform_pickup();
            break;
        case '6':
            perform_ultimate();
            break;
        case '7':
            perform_stun();
            break;
        case 'q':
        case 'Q':
            running = 0;
            break;
        default:
            break;
    }
}

void synth_enemy_inventory(int enemy_index, int *out, int solar_core_holder, int lunar_blade_holder) {
    for (int s = 0; s < game_state::inventory_slots; ++s) {
        out[s] = 0;
    }
    int slot = 0;
    int eid = game_state::max_players + enemy_index;
    if (solar_core_holder == eid && slot < game_state::inventory_slots) {
        out[slot++] = game_state::solar_core_id;
    }
    if (lunar_blade_holder == eid && slot < game_state::inventory_slots) {
        out[slot++] = game_state::lunar_blade_id;
    }
    int pattern = enemy_index % 3;
    int filler = game_state::splinter_stick_id;
    if (pattern == 1) {
        filler = game_state::venom_dagger_id;
    } else if (pattern == 2) {
        filler = game_state::iron_halberd_id;
    }
    if (slot < game_state::inventory_slots) {
        out[slot++] = filler;
    }
}

void take_world_snapshot(world_snapshot &snap) {
    if (!lock_memory()) {
        memset(&snap, 0, sizeof(snap));
        return;
    }
    snap.active_player_count = shared_state->active_player_count;
    snap.active_enemy_count = shared_state->active_enemy_count;
    snap.solar_core_holder = shared_state->solar_core_holder;
    snap.lunar_blade_holder = shared_state->lunar_blade_holder;
    snap.current_dropped_weapon = shared_state->current_dropped_weapon;
    snap.arbiter_pid = shared_state->arbiter_pid;
    snap.asp_pid = shared_state->asp_pid;
    snap.hip_pid = shared_state->hip_pid;
    memcpy(snap.action_log, shared_state->action_log, sizeof(snap.action_log));
    snap.action_log[sizeof(snap.action_log) - 1] = '\0';

    snap.control_player = select_active_player_locked();
    snap.target_enemy = find_first_living_enemy_locked();

    time_t now = time(nullptr);
    bool any_player_stunned = false;
    bool any_enemy_stunned = false;

    for (int i = 0; i < game_state::max_players; ++i) {
        snap.players[i].hp = shared_state->player_hp[i];
        snap.players[i].stamina = shared_state->player_stamina[i];
        snap.players[i].stun_end = shared_state->player_stun_end_time[i];
        if (snap.players[i].hp > observed_player_max_hp[i]) {
            observed_player_max_hp[i] = snap.players[i].hp;
        }
        snap.players[i].max_hp = observed_player_max_hp[i] > 0 ? observed_player_max_hp[i] : 1;
        snap.players[i].max_stamina = player_stamina_cap_local;
        snap.players[i].active = (i < snap.active_player_count);
        snap.players[i].dead = snap.players[i].active && snap.players[i].hp <= 0;
        snap.players[i].turn = (i == snap.control_player);
        for (int s = 0; s < game_state::inventory_slots; ++s) {
            snap.players[i].inventory[s] = shared_state->player_primary_inventory[i][s];
        }
        if (snap.players[i].active && snap.players[i].stun_end > now) {
            any_player_stunned = true;
        }
    }

    for (int i = 0; i < game_state::max_enemies; ++i) {
        snap.enemies[i].hp = shared_state->enemy_hp[i];
        snap.enemies[i].stamina = shared_state->enemy_stamina[i];
        snap.enemies[i].stun_end = shared_state->stun_end_time[i];
        if (snap.enemies[i].hp > observed_enemy_max_hp[i]) {
            observed_enemy_max_hp[i] = snap.enemies[i].hp;
        }
        snap.enemies[i].max_hp = observed_enemy_max_hp[i] > 0 ? observed_enemy_max_hp[i] : 1;
        snap.enemies[i].max_stamina = enemy_stamina_cap_local;
        snap.enemies[i].active = (i < snap.active_enemy_count);
        snap.enemies[i].dead = snap.enemies[i].active && snap.enemies[i].hp <= 0;
        snap.enemies[i].turn = (i == snap.target_enemy);
        synth_enemy_inventory(i, snap.enemies[i].inventory, snap.solar_core_holder, snap.lunar_blade_holder);
        if (snap.enemies[i].active && snap.enemies[i].stun_end > now) {
            any_enemy_stunned = true;
        }
    }

    snap.ultimate_active = false;
    snap.stun_active = any_player_stunned || any_enemy_stunned;
    const char *log = snap.action_log;
    if (strstr(log, "ultimate triggered") != nullptr) {
        snap.ultimate_active = true;
    }

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

void draw_titled_box(int y, int x, int h, int w, const char *title, int pair, int extra_attr) {
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
    int counts_w = 12;
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
    if (width < 22) {
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
    for (int s = 0; s < game_state::inventory_slots; ++s) {
        int weapon = inventory[s];
        int pair = weapon == 0 ? pair_inv_empty : weapon_color_pair_for(weapon);
        int attrs = COLOR_PAIR(pair) | A_REVERSE | A_BOLD;
        if (dead_overlay) {
            pair = pair_border_dead;
            attrs = COLOR_PAIR(pair) | A_REVERSE | A_BOLD;
        }
        char letter = weapon == 0 ? ' ' : weapon_letter(weapon);
        attron(attrs);
        for (int k = 0; k < slot_w; ++k) {
            char ch = (slot_w >= 2 && k == slot_w / 2) ? letter : ' ';
            mvaddch(y, start_x + s * slot_w + k, ch);
        }
        attroff(attrs);
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
    }
    draw_titled_box(y, x, h, w, title, border_pair, border_extra);
    int inner_y = y + 1;
    int inner_x = x + 2;
    int inner_w = w - 4;
    int inner_h = h - 2;
    if (inner_h <= 0 || inner_w <= 0) {
        return;
    }
    int row = 0;
    if (inner_h - row >= 1 && inner_w >= 10) {
        int hp_pair = es.dead ? pair_border_dead : 0;
        draw_bar(inner_y + row, inner_x, inner_w, es.hp, es.max_hp, "hp", hp_pair, !es.dead && is_player);
        row++;
    }
    if (inner_h - row >= 1 && inner_w >= 10) {
        int sta_pair = es.dead ? pair_border_dead : pair_stamina;
        draw_bar(inner_y + row, inner_x, inner_w, es.stamina, es.max_stamina, "sta", sta_pair, false);
        row++;
    }
    if (inner_h - row >= 1) {
        time_t now = time(nullptr);
        if (es.stun_end > now) {
            int seconds_left = (int)(es.stun_end - now);
            attron(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
            mvprintw(inner_y + row, inner_x, "stunned %ds", seconds_left);
            attroff(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
        } else if (es.dead) {
            attron(COLOR_PAIR(pair_border_dead) | A_BOLD | A_BLINK);
            mvprintw(inner_y + row, inner_x, "  ELIMINATED  ");
            attroff(COLOR_PAIR(pair_border_dead) | A_BOLD | A_BLINK);
        }
    }
    if (inner_h - row >= 2) {
        int inv_y = inner_y + inner_h - 2;
        int inv_h = 2;
        int inv_pair = border_pair;
        draw_box_at(inv_y, inner_x, inv_h, inner_w, inv_pair, border_extra);
        draw_inventory_strip(inv_y + inv_h - 1 - 0, inner_x + 1, inner_w - 2, es.inventory, es.dead);
        if (inv_h >= 3) {
            attron(COLOR_PAIR(pair_panel_title) | A_BOLD);
            mvaddch(inv_y, inner_x + 2, ' ');
            mvprintw(inv_y, inner_x + 3, "inventroy tetris");
            mvaddch(inv_y, inner_x + 3 + 16, ' ');
            attroff(COLOR_PAIR(pair_panel_title) | A_BOLD);
        }
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
    if (per_h < 4) {
        per_h = 4;
    }
    int min_h = 5;
    if (per_h < min_h) {
        per_h = min_h;
    }
    char title_buf[32];
    int drawn_y = inner_y;
    for (int i = 0; i < active; ++i) {
        if (drawn_y + per_h > inner_y + inner_h) {
            break;
        }
        snprintf(title_buf, sizeof(title_buf), "player %d", i + 1);
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
        snprintf(title_buf, sizeof(title_buf), "enemy %d", i + 1);
        render_entity(box_y, box_x, per_h, per_w, title_buf, snap.enemies[i], false);
    }
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
        mvprintw(inner_y + row, inner_x, "arbiter pid: %d", (int)snap.arbiter_pid);
        attroff(COLOR_PAIR(pair_default));
        row++;
    }
    if (row < max_rows) {
        attron(COLOR_PAIR(pair_default));
        mvprintw(inner_y + row, inner_x, "asp pid: %d  hip pid: %d", (int)snap.asp_pid, (int)snap.hip_pid);
        attroff(COLOR_PAIR(pair_default));
        row++;
    }
    if (row < max_rows) {
        if (snap.ultimate_active) {
            attron(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
            mvprintw(inner_y + row, inner_x, "ULTIMATE FROZEN");
            attroff(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
        } else if (snap.stun_active) {
            attron(COLOR_PAIR(pair_status_warn) | A_BOLD);
            mvprintw(inner_y + row, inner_x, "stun active");
            attroff(COLOR_PAIR(pair_status_warn) | A_BOLD);
        } else {
            attron(COLOR_PAIR(pair_status_ok) | A_BOLD);
            mvprintw(inner_y + row, inner_x, "arena: live");
            attroff(COLOR_PAIR(pair_status_ok) | A_BOLD);
        }
        row++;
    }
    if (row < max_rows) {
        if (strstr(snap.action_log, "deadlock") != nullptr) {
            attron(COLOR_PAIR(pair_status_warn) | A_BOLD);
            mvprintw(inner_y + row, inner_x, "DEADLOCK BROKEN");
            attroff(COLOR_PAIR(pair_status_warn) | A_BOLD);
        }
        row++;
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
    int holder;
    int row = 0;

    if (row < max_rows) {
        holder = snap.solar_core_holder;
        const char *label;
        int color = pair_artifact_solar;
        if (snap.current_dropped_weapon == game_state::solar_core_id) {
            label = "on ground";
            color = pair_artifact_ground;
        } else if (holder < 0) {
            label = "missing";
            color = pair_artifact_ground;
        } else if (holder < game_state::max_players) {
            snprintf(buffer, sizeof(buffer), "player %d", holder + 1);
            label = buffer;
        } else {
            snprintf(buffer, sizeof(buffer), "enemy %d", holder - game_state::max_players + 1);
            label = buffer;
            color = pair_border_dead;
        }
        attron(COLOR_PAIR(pair_inv_solar) | A_REVERSE | A_BOLD);
        mvaddch(inner_y + row, inner_x, ' ');
        mvaddch(inner_y + row, inner_x + 1, 'O');
        mvaddch(inner_y + row, inner_x + 2, ' ');
        attroff(COLOR_PAIR(pair_inv_solar) | A_REVERSE | A_BOLD);
        attron(COLOR_PAIR(color) | A_BOLD);
        mvprintw(inner_y + row, inner_x + 4, "solar core: %s", label);
        attroff(COLOR_PAIR(color) | A_BOLD);
        row++;
    }

    if (row < max_rows) {
        holder = snap.lunar_blade_holder;
        const char *label;
        int color = pair_artifact_lunar;
        if (snap.current_dropped_weapon == game_state::lunar_blade_id) {
            label = "on ground";
            color = pair_artifact_ground;
        } else if (holder < 0) {
            label = "missing";
            color = pair_artifact_ground;
        } else if (holder < game_state::max_players) {
            snprintf(buffer, sizeof(buffer), "player %d", holder + 1);
            label = buffer;
        } else {
            snprintf(buffer, sizeof(buffer), "enemy %d", holder - game_state::max_players + 1);
            label = buffer;
            color = pair_border_dead;
        }
        attron(COLOR_PAIR(pair_inv_lunar) | A_REVERSE | A_BOLD);
        mvaddch(inner_y + row, inner_x, ' ');
        mvaddch(inner_y + row, inner_x + 1, 'L');
        mvaddch(inner_y + row, inner_x + 2, ' ');
        attroff(COLOR_PAIR(pair_inv_lunar) | A_REVERSE | A_BOLD);
        attron(COLOR_PAIR(color) | A_BOLD);
        mvprintw(inner_y + row, inner_x + 4, "lunar blade: %s", label);
        attroff(COLOR_PAIR(color) | A_BOLD);
        row++;
    }

    if (row < max_rows && snap.current_dropped_weapon != 0) {
        const char *name = weapon_short_name(snap.current_dropped_weapon);
        attron(COLOR_PAIR(pair_artifact_ground) | A_BOLD);
        mvprintw(inner_y + row, inner_x, "ground drop: %s [press 5]", name);
        attroff(COLOR_PAIR(pair_artifact_ground) | A_BOLD);
    }
}

void render_arena_panel(int y, int x, int h, int w, const world_snapshot &snap) {
    draw_titled_box(y, x, h, w, "combat arena", pair_arena_title, A_BOLD);
    int inner_y = y + 1;
    int inner_x = x + 1;
    int inner_w = w - 2;
    int inner_h = h - 2;
    if (inner_h < 6 || inner_w < 10) {
        return;
    }
    int log_h = inner_h / 2;
    if (log_h < 5) {
        log_h = 5;
    }
    int remaining = inner_h - log_h;
    int status_h = remaining / 2;
    if (status_h < 4) {
        status_h = 4;
    }
    int artifact_h = remaining - status_h;
    if (artifact_h < 4) {
        artifact_h = 4;
        status_h = remaining - artifact_h;
    }
    if (status_h < 4) {
        status_h = remaining;
        artifact_h = 0;
    }
    render_action_log_box(inner_y, inner_x, log_h, inner_w, snap);
    if (status_h > 0) {
        render_system_status_box(inner_y + log_h, inner_x, status_h, inner_w, snap);
    }
    if (artifact_h > 0) {
        render_artifact_tracker_box(inner_y + log_h + status_h, inner_x, artifact_h, inner_w, snap);
    }
}

void render_command_bar(int y, int term_w) {
    attron(COLOR_PAIR(pair_command_bar) | A_REVERSE | A_BOLD);
    for (int i = 0; i < term_w; ++i) {
        mvaddch(y, i, ' ');
    }
    const char *legend = " [1]strike  [2]exhaust  [3]heal  [4]skip  [5]pickup  [6]ultimate  [7]stun  [q]quit ";
    int legend_len = (int)strlen(legend);
    int start = (term_w - legend_len) / 2;
    if (start < 0) {
        start = 0;
        legend_len = term_w;
    }
    mvprintw(y, start, "%.*s", legend_len, legend);
    attroff(COLOR_PAIR(pair_command_bar) | A_REVERSE | A_BOLD);
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
    if (term_h < 12 || term_w < 60) {
        attron(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
        mvprintw(0, 0, "terminal too small (need at least 60x12)");
        attroff(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
        refresh();
        pthread_mutex_unlock(&ncurses_lock);
        return;
    }
    int command_h = 1;
    int main_h = term_h - command_h;
    int left_w = term_w * 28 / 100;
    int right_w = term_w * 38 / 100;
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
    render_player_panel(0, 0, main_h, left_w, snap);
    render_arena_panel(0, left_w, main_h, center_w, snap);
    render_enemy_panel(0, left_w + center_w, main_h, right_w, snap);
    render_command_bar(term_h - 1, term_w);
    refresh();
    pthread_mutex_unlock(&ncurses_lock);
}

void *render_loop(void *) {
    while (running) {
        world_snapshot snap;
        take_world_snapshot(snap);
        render_frame(snap);
        interruptible_usleep(frame_sleep_us);
    }
    return nullptr;
}

void *player_input_loop(void *) {
    while (running) {
        int ch = ERR;
        pthread_mutex_lock(&ncurses_lock);
        ch = getch();
        pthread_mutex_unlock(&ncurses_lock);
        if (ch != ERR) {
            handle_key(ch);
        }
        interruptible_usleep(input_sleep_us);
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
    init_pair(pair_default, COLOR_WHITE, COLOR_BLACK);
    init_pair(pair_border_normal, COLOR_CYAN, COLOR_BLACK);
    init_pair(pair_border_active, COLOR_YELLOW, COLOR_BLACK);
    init_pair(pair_border_dead, COLOR_RED, COLOR_BLACK);
    init_pair(pair_hp_high, COLOR_GREEN, COLOR_BLACK);
    init_pair(pair_hp_med, COLOR_YELLOW, COLOR_BLACK);
    init_pair(pair_hp_low, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(pair_hp_critical, COLOR_RED, COLOR_BLACK);
    init_pair(pair_stamina, COLOR_CYAN, COLOR_BLACK);
    init_pair(pair_log, COLOR_WHITE, COLOR_BLACK);
    init_pair(pair_status_ok, COLOR_GREEN, COLOR_BLACK);
    init_pair(pair_status_warn, COLOR_RED, COLOR_BLACK);
    init_pair(pair_artifact_solar, COLOR_YELLOW, COLOR_BLACK);
    init_pair(pair_artifact_lunar, COLOR_CYAN, COLOR_BLACK);
    init_pair(pair_artifact_ground, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(pair_command_bar, COLOR_BLACK, COLOR_WHITE);
    init_pair(pair_inv_empty, COLOR_BLACK, COLOR_BLACK);
    init_pair(pair_inv_splinter, COLOR_GREEN, COLOR_BLACK);
    init_pair(pair_inv_venom, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(pair_inv_iron, COLOR_WHITE, COLOR_BLACK);
    init_pair(pair_inv_solar, COLOR_YELLOW, COLOR_BLACK);
    init_pair(pair_inv_lunar, COLOR_CYAN, COLOR_BLACK);
    init_pair(pair_panel_title, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(pair_arena_title, COLOR_CYAN, COLOR_BLACK);
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
    bkgd(COLOR_PAIR(pair_default));
    return true;
}

bool spawn_threads(pthread_t *render_thread, pthread_t *input_thread) {
    if (pthread_create(render_thread, nullptr, render_loop, nullptr) != 0) {
        print_errno("pthread_create render failed");
        return false;
    }
    if (pthread_create(input_thread, nullptr, player_input_loop, nullptr) != 0) {
        print_errno("pthread_create input failed");
        running = 0;
        pthread_join(*render_thread, nullptr);
        return false;
    }
    return true;
}

void join_threads(pthread_t render_thread, pthread_t input_thread) {
    pthread_join(render_thread, nullptr);
    pthread_join(input_thread, nullptr);
}

void cleanup_tui_once() {
    endwin();
}

void cleanup_resources() {
    pthread_mutex_destroy(&ncurses_lock);
    unmap_shared_memory();
    close_shared_fd();
}

bool init_locks() {
    if (pthread_mutex_init(&ncurses_lock, nullptr) != 0) {
        print_errno("pthread_mutex_init ncurses_lock failed");
        return false;
    }
    return true;
}

int main() {
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
    if (!init_tui()) {
        cleanup_resources();
        return 1;
    }
    pthread_t render_thread;
    pthread_t input_thread;
    if (!spawn_threads(&render_thread, &input_thread)) {
        cleanup_tui_once();
        cleanup_resources();
        return 1;
    }
    join_threads(render_thread, input_thread);
    cleanup_tui_once();
    cleanup_resources();
    return 0;
}
