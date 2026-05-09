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
    static constexpr int weapon_none = 0;
    static constexpr int splinter_stick_id = 1;
    static constexpr int venom_dagger_id = 2;
    static constexpr int iron_halberd_id = 3;
    static constexpr int solar_core_id = 4;
    static constexpr int lunar_blade_id = 5;
    static constexpr int splinter_stick = 2;
    static constexpr int venom_dagger = 4;
    static constexpr int iron_halberd = 7;
    static constexpr int solar_core = 10;
    static constexpr int lunar_blade = 10;

    int player_hp[max_players];
    int player_speed[max_players];
    int player_stamina[max_players];
    time_t player_stun_end_time[max_players];

    int enemy_hp[max_enemies];
    int enemy_speed[max_enemies];
    int enemy_stamina[max_enemies];
    time_t stun_end_time[max_enemies];

    int player_primary_inventory[max_players][inventory_slots];
    int long_term_storage[max_players][inventory_slots];

    int solar_core_holder = -1;
    int lunar_blade_holder = -1;
    int eclipse_relic_holder;
    int solar_core_waiter = -1;
    int lunar_blade_waiter = -1;
    int current_dropped_weapon = 0;
    int active_player_count;
    int active_enemy_count;
    pid_t arbiter_pid;
    pid_t asp_pid;
    pid_t hip_pid;
    char action_log[256];

    pthread_mutex_t resource_lock;
    sem_t state_lock;
    sem_t memory_sem;
    sem_t player_sem;
    sem_t enemy_sem;
    sem_t inventory_sem;
    sem_t relic_sem;
};

#endif
