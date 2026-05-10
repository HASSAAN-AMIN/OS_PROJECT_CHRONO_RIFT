#ifndef HIP_SHARED_H
#define HIP_SHARED_H

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <queue>
#include <vector>

#include <fcntl.h>
#include <ncurses.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../gamestate.h"

using namespace std;

extern const char *shared_memory_name;

extern const useconds_t frame_sleep_us;
extern const useconds_t input_sleep_us;
extern const useconds_t player_idle_sleep_us;
extern const int pending_target_auto;
extern const long long hit_flash_duration;
extern const long long death_flash_duration;
extern const long long shake_duration;
extern const long long full_stamina_flash_duration;
extern const long long drop_flash_duration;

extern const int pair_default;
extern const int pair_border_normal;
extern const int pair_border_active;
extern const int pair_border_dead;
extern const int pair_hp_high;
extern const int pair_hp_med;
extern const int pair_hp_low;
extern const int pair_hp_critical;
extern const int pair_stamina;
extern const int pair_stamina_full;
extern const int pair_log;
extern const int pair_status_ok;
extern const int pair_status_warn;
extern const int pair_artifact_solar;
extern const int pair_artifact_lunar;
extern const int pair_artifact_eclipse;
extern const int pair_artifact_ground;
extern const int pair_command_bar;
extern const int pair_inv_empty;
extern const int pair_inv_splinter;
extern const int pair_inv_venom;
extern const int pair_inv_obsidian;
extern const int pair_inv_frost;
extern const int pair_inv_thunder;
extern const int pair_inv_iron;
extern const int pair_inv_solar;
extern const int pair_inv_lunar;
extern const int pair_inv_eclipse;
extern const int pair_panel_title;
extern const int pair_arena_title;
extern const int pair_banner;
extern const int pair_kill_counter;
extern const int pair_overlay_win;
extern const int pair_overlay_lose;
extern const int pair_overlay_quit;
extern const int pair_help;
extern const int pair_enemy_dead_purple;
extern const int pair_bg_canvas;
extern const int pair_bg_player;
extern const int pair_bg_arena;
extern const int pair_bg_enemy;
extern const int pair_bg_footer;
extern const int pair_title_player;
extern const int pair_title_enemy;
extern const int pair_title_arena;
extern const int pair_bg_player_box;
extern const int pair_bg_enemy_box;
extern const int pair_bg_timeline;
extern const int pair_bg_log;
extern const int pair_bg_status;
extern const int pair_bg_artifact;
extern const int pair_bg_banner_1;
extern const int pair_bg_banner_2;
extern const int pair_bg_banner_3;
extern const int pair_bg_banner_4;
extern const int pair_bg_banner_5;
extern const int pair_border_v1;
extern const int pair_border_v2;
extern const int pair_border_v3;
extern const int pair_border_v4;
extern const int pair_border_v5;
extern const int pair_border_v6;
extern const int pair_border_v7;

extern int shared_memory_fd;
extern game_state *shared_state;

extern volatile sig_atomic_t running;
extern volatile sig_atomic_t resize_pending;

extern pthread_mutex_t ncurses_lock;

struct player_input_buffer {
    int pending_key;
    pthread_mutex_t lock;
    pthread_cond_t cv;
};

extern player_input_buffer player_buffers[game_state::max_players];
extern pthread_t render_thread;
extern pthread_t input_dispatcher_thread;
extern pthread_t player_threads[game_state::max_players];
extern int player_thread_ids[game_state::max_players];
extern int chosen_party_size;
extern int show_help_overlay;
extern int show_swap_overlay;
extern int swap_overlay_player_id;
extern int swap_overlay_option_count;
extern int swap_overlay_weapon_ids[game_state::weapon_id_max];
extern char swap_overlay_option_keys[game_state::weapon_id_max];
extern int pending_swap_weapon_id[game_state::max_players];
extern unsigned long render_frame_counter;
extern long long current_frame;
extern long long hit_flash_frame[13];
extern long long death_flash_frame[13];
extern long long full_stamina_flash_frame[13];
extern long long stamina_ready_pulse_frame[game_state::max_players];
extern long long enemy_spawn_flash_frame[game_state::max_enemies];
extern long long kill_pop_frame;
extern long long deadlock_log_frame;
extern long long ultimate_shockwave_frame;
extern long long pickup_sweep_frame[game_state::max_players];
extern int prev_hp[13];
extern int prev_stamina[13];
extern int prev_enemy_dead_count[game_state::max_enemies];
extern int prev_enemy_display_id[game_state::max_enemies];
extern int prev_player_inventory_fill[game_state::max_players];
extern int prev_total_kills;
extern int prev_ultimate_active;
extern int prev_dropped_weapon;
extern long long weapon_drop_flash_frame;
extern time_t last_event_time;
extern char last_event_label[64];
extern queue<int> enemy_target_queue;
extern int cheat_drop_cursor;

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
    int total_kills;
    int enemy_display_id[game_state::max_enemies];
    int enemy_dead_count[game_state::max_enemies];
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

void initialize_animation_state();
void print_errno(const char *action);
void interruptible_usleep(useconds_t us);
bool open_shared_memory();
bool map_shared_memory();
void unmap_shared_memory();
void close_shared_fd();
bool lock_memory();
bool unlock_memory();
bool register_hip_pid_immediately();
int weapon_damage_value(int weapon_id);
int weapon_slot_size(int weapon_id);
const char *weapon_name(int weapon_id);
char weapon_letter(int weapon_id);
int weapon_color_pair_for(int weapon_id);
int find_first_living_enemy_locked();
int read_active_player_index();
int read_outcome();
void clear_swap_overlay_state();
void send_quit_signals();
void take_world_snapshot(world_snapshot &snap);

void *render_loop(void *);
void *input_dispatcher_loop(void *);
void *player_action_loop(void *);
void *outcome_watch_loop(void *);

bool init_tui();
void cleanup_tui_once();
int prompt_party_size(int argc, char **argv);

bool init_locks();
void destroy_locks();
bool apply_party_size_to_state(int party_size);
bool spawn_threads();
void join_threads();

#endif
