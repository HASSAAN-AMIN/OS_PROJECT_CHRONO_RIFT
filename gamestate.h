#ifndef gamestate_h
#define gamestate_h

#include <ctime>
#include <sys/types.h>
#include <pthread.h>
#include <semaphore.h>

struct game_state {
    static constexpr int max_players = 4;
    static constexpr int max_enemies = 9;
    static constexpr int inventory_slots = 20;
    static constexpr int storage_slots = 20;
    static constexpr int kills_required_to_win = 10;

    static constexpr int weapon_none = 0;
    static constexpr int splinter_stick_id = 1;
    static constexpr int venom_dagger_id = 2;
    static constexpr int obsidian_axe_id = 3;
    static constexpr int frostbow_id = 4;
    static constexpr int thunderstaff_id = 5;
    static constexpr int iron_halberd_id = 6;
    static constexpr int solar_core_id = 7;
    static constexpr int lunar_blade_id = 8;
    static constexpr int eclipse_relic_id = 9;
    static constexpr int weapon_id_max = 9;

    static constexpr int splinter_stick_slots = 2;
    static constexpr int venom_dagger_slots = 4;
    static constexpr int obsidian_axe_slots = 5;
    static constexpr int frostbow_slots = 6;
    static constexpr int thunderstaff_slots = 6;
    static constexpr int iron_halberd_slots = 7;
    static constexpr int solar_core_slots = 10;
    static constexpr int lunar_blade_slots = 10;
    static constexpr int eclipse_relic_slots = 8;

    static constexpr int splinter_stick_damage = 12;
    static constexpr int venom_dagger_damage = 30;
    static constexpr int obsidian_axe_damage = 45;
    static constexpr int frostbow_damage = 48;
    static constexpr int thunderstaff_damage = 50;
    static constexpr int iron_halberd_damage = 55;
    static constexpr int solar_core_damage = 95;
    static constexpr int lunar_blade_damage = 90;
    static constexpr int eclipse_relic_damage = 70;

    static constexpr int splinter_stick = 12;
    static constexpr int venom_dagger = 30;
    static constexpr int iron_halberd = 55;
    static constexpr int solar_core = 95;
    static constexpr int lunar_blade = 90;

    static constexpr int player_max_stamina = 100;
    static constexpr int enemy_max_stamina = 150;
    static constexpr int player_base_damage = 10;
    static constexpr int enemy_attack_damage = 18;

    static constexpr int outcome_ongoing = 0;
    static constexpr int outcome_win = 1;
    static constexpr int outcome_lose = 2;
    static constexpr int outcome_quit = 3;

    static constexpr int action_none = 0;
    static constexpr int action_strike = 1;
    static constexpr int action_exhaust = 2;
    static constexpr int action_heal = 3;
    static constexpr int action_skip = 4;
    static constexpr int action_pickup = 5;
    static constexpr int action_ultimate = 6;
    static constexpr int action_stun = 7;
    static constexpr int action_use_weapon = 8;
    static constexpr int action_swap_in = 9;

    int player_hp[max_players];
    int player_max_hp[max_players];
    int player_speed[max_players];
    int player_stamina[max_players];
    int player_damage[max_players];
    time_t player_stun_end_time[max_players];

    int enemy_hp[max_enemies];
    int enemy_max_hp[max_enemies];
    int enemy_speed[max_enemies];
    int enemy_stamina[max_enemies];
    int enemy_damage[max_enemies];
    time_t stun_end_time[max_enemies];

    int player_primary_inventory[max_players][inventory_slots];
    int long_term_storage[max_players][storage_slots];

    int solar_core_holder = -1;
    int lunar_blade_holder = -1;
    int eclipse_relic_holder;
    int eclipse_relic_present;
    int solar_core_waiter = -1;
    int lunar_blade_waiter = -1;
    int current_dropped_weapon = 0;
    int active_player_count;
    int active_enemy_count;
    int active_player_index;
    int enemy_kills;
    int outcome;
    int roll_number;
    pid_t arbiter_pid;
    pid_t asp_pid;
    pid_t hip_pid;
    char action_log[256];
    char last_event[128];

    pthread_mutex_t resource_lock;
    sem_t state_lock;
    sem_t memory_sem;
    sem_t player_sem;
    sem_t enemy_sem;
    sem_t inventory_sem;
    sem_t relic_sem;
};

#endif
