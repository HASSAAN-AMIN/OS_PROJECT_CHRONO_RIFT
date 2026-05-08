#ifndef gamestate_h
#define gamestate_h

#include <semaphore.h>

struct game_state {
    static constexpr int max_players = 4;
    static constexpr int max_enemies = 9;
    static constexpr int inventory_slots = 20;
    static constexpr int storage_rows = 4;
    static constexpr int storage_slots = 40;

    int player_hp[max_players];
    int player_stamina[max_players];

    int enemy_hp[max_enemies];
    int enemy_stamina[max_enemies];

    int player_primary_inventory[max_players][inventory_slots];
    int long_term_storage[storage_rows][storage_slots];

    int solar_core_holder;
    int lunar_blade_holder;
    int eclipse_relic_holder;

    sem_t memory_sem;
    sem_t player_sem;
    sem_t enemy_sem;
    sem_t inventory_sem;
    sem_t relic_sem;
};

#endif
