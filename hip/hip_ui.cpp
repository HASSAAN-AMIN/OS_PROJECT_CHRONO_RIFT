#include "hip_ui.h"

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

void paint_rect(int y, int x, int h, int w, int pair) {
    if (h <= 0 || w <= 0) {
        return;
    }
    attron(COLOR_PAIR(pair));
    for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) {
            mvaddch(yy, xx, ' ');
        }
    }
    attroff(COLOR_PAIR(pair));
}

int varied_border_pair(int requested_pair, int y, int x) {
    if (requested_pair == pair_border_dead || requested_pair == pair_border_active) {
        return requested_pair;
    }
    int options[7] = {
        pair_border_v1, pair_border_v2, pair_border_v3, pair_border_v4,
        pair_border_v5, pair_border_v6, pair_border_v7
    };
    int idx = ((y / 2) + (x / 3)) % 7;
    if (idx < 0) {
        idx = 0;
    }
    return options[idx];
}

void draw_titled_box(int y, int x, int h, int w, const char *title, int pair, int extra_attr) {
    int border_pair = varied_border_pair(pair, y, x);
    draw_box_at(y, x, h, w, border_pair, extra_attr);
    if (title == nullptr || w < 8) {
        return;
    }
    int title_len = (int)strlen(title);
    int max_title = w - 6;
    if (title_len > max_title) {
        title_len = max_title;
    }
    chtype attr = COLOR_PAIR(border_pair) | extra_attr | A_BOLD;
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

void draw_bar(
    int y,
    int x,
    int width,
    int value,
    int max_value,
    const char *label,
    int color_pair,
    bool blink_when_low,
    bool blink_when_full
) {
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
    if (blink_when_full && value >= max_value) {
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

void draw_inventory_strip(int y, int x, int width, const int *inventory, bool dead_overlay, int entity_index) {
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
        bool pulse = false;
        if (entity_index >= 0 && entity_index < 13) {
            for (int j = s; j < s + run && j < game_state::inventory_slots; ++j) {
                if (inventory_new_item_frames[entity_index][j] > 0 && weapon != 0) {
                    pulse = true;
                    break;
                }
            }
        }
        if (pulse) {
            attrs |= A_BLINK;
        }
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

void draw_inventory_sweep_overlay(int y, int x, int width, int slots_to_fill) {
    if (width < game_state::inventory_slots + 2) {
        return;
    }
    if (slots_to_fill <= 0) {
        return;
    }
    if (slots_to_fill > game_state::inventory_slots) {
        slots_to_fill = game_state::inventory_slots;
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
    attron(COLOR_PAIR(pair_default) | A_REVERSE | A_BOLD);
    for (int s = 0; s < slots_to_fill; ++s) {
        for (int k = 0; k < slot_w; ++k) {
            mvaddch(y, start_x + s * slot_w + k, ' ');
        }
    }
    attroff(COLOR_PAIR(pair_default) | A_REVERSE | A_BOLD);
}

void render_entity(int y, int x, int h, int w, const char *title, const entity_snapshot &es, bool is_player, int entity_index) {
    if (entity_index < 0 || entity_index >= 13) {
        entity_index = 0;
    }
    bool hit_flash_active = hit_flash_frame[entity_index] >= 0 &&
                            (current_frame - hit_flash_frame[entity_index]) < hit_flash_duration;
    if (entity_hit_frames[entity_index] > 0) {
        hit_flash_active = true;
    }
    bool death_flash_active = death_flash_frame[entity_index] >= 0 &&
                              (current_frame - death_flash_frame[entity_index]) < death_flash_duration;
    bool shake_active = (current_frame - hit_flash_frame[entity_index]) < shake_duration;
    if (shake_active) {
        int offset = (current_frame % 2 == 0) ? 1 : -1;
        int max_x = COLS - w - 1;
        if (max_x < 0) {
            max_x = 0;
        }
        x += offset;
        if (x < 0) {
            x = 0;
        }
        if (x > max_x) {
            x = max_x;
        }
    }
    int body_pair = is_player ? pair_bg_player_box : pair_bg_enemy_box;
    if (es.dead) {
        body_pair = pair_enemy_dead_purple;
    }
    if (death_flash_active) {
        body_pair = pair_hp_critical;
    }
    paint_rect(y + 1, x + 1, h - 2, w - 2, body_pair);
    if (hit_flash_active) {
        attron(COLOR_PAIR(pair_default) | A_REVERSE | A_BOLD);
        for (int yy = y + 1; yy < y + h - 1; ++yy) {
            for (int xx = x + 1; xx < x + w - 1; ++xx) {
                mvaddch(yy, xx, ' ');
            }
        }
        attroff(COLOR_PAIR(pair_default) | A_REVERSE | A_BOLD);
    }
    if (death_flash_active) {
        attron(COLOR_PAIR(pair_hp_critical) | A_REVERSE | A_BOLD);
        for (int yy = y + 1; yy < y + h - 1; ++yy) {
            for (int xx = x + 1; xx < x + w - 1; ++xx) {
                mvaddch(yy, xx, ' ');
            }
        }
        attroff(COLOR_PAIR(pair_hp_critical) | A_REVERSE | A_BOLD);
    }
    int border_pair = pair_border_normal;
    int border_extra = 0;
    if (hit_flash_active) {
        border_pair = pair_hp_low;
        border_extra = A_BOLD;
    }
    if (death_flash_active) {
        border_pair = pair_hp_critical;
        border_extra = A_BOLD | A_BLINK;
    }
    if (is_player && entity_index >= 0 && entity_index < game_state::max_players) {
        long long elapsed = current_frame - stamina_ready_pulse_frame[entity_index];
        if (elapsed >= 0 && elapsed < 18) {
            int phase = (int)((elapsed / 3) % 2);
            if (phase == 0) {
                border_pair = pair_default;
                border_extra |= A_BOLD | A_BLINK;
            }
        }
    }
    if (!is_player) {
        int enemy_slot = entity_index - game_state::max_players;
        if (enemy_slot >= 0 && enemy_slot < game_state::max_enemies) {
            long long elapsed = current_frame - enemy_spawn_flash_frame[enemy_slot];
            if (elapsed >= 0 && elapsed < 20) {
                int sequence[7] = {
                    pair_border_v1, pair_border_v2, pair_border_v3, pair_border_v4,
                    pair_border_v5, pair_border_v6, pair_border_v7
                };
                border_pair = sequence[elapsed % 7];
                border_extra |= A_BOLD | A_BLINK;
            }
        }
    }
    if (es.dead) {
        border_pair = pair_border_dead;
        border_extra = A_BOLD;
        if (death_flash_active) {
            border_pair = pair_hp_critical;
            border_extra = A_BOLD | A_BLINK;
        }
        draw_titled_box(y, x, h, w, title, border_pair, border_extra);
    } else if (is_player && es.turn) {
        border_pair = pair_border_active;
        border_extra = A_BOLD;
        if (hit_flash_active) {
            border_pair = pair_hp_low;
        }
        int active_pair = varied_border_pair(border_pair, y, x);
        draw_double_box_at(y, x, h, w, active_pair, border_extra);
        int title_len = (int)strlen(title);
        chtype attr = COLOR_PAIR(active_pair) | A_BOLD | A_REVERSE;
        attron(attr);
        mvaddch(y, x + 2, ' ');
        mvprintw(y, x + 3, " %.*s ", title_len, title);
        attroff(attr);
    } else if (!is_player && es.turn) {
        border_pair = pair_border_active;
        border_extra = A_BOLD;
        if (hit_flash_active) {
            border_pair = pair_hp_low;
        }
        draw_double_box_at(y, x, h, w, border_pair, border_extra);
        int title_len = (int)strlen(title);
        int max_t = w - 10;
        if (title_len > max_t) { title_len = max_t; }
        chtype attr = COLOR_PAIR(border_pair) | A_BOLD | A_REVERSE;
        attron(attr);
        mvprintw(y, x + 2, ">>%.*s<<", title_len, title);
        attroff(attr);
    } else {
        draw_titled_box(y, x, h, w, title, border_pair, border_extra);
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
        if (death_flash_active) {
            hp_pair = pair_hp_critical;
        }
        if (hit_flash_active && !es.dead) {
            hp_pair = pair_hp_critical;
        }
        draw_bar(inner_y + row, inner_x, inner_w, es.hp, es.max_hp, "hp", hp_pair, !es.dead && is_player, false);
        row++;
    }
    if (inner_h - row >= 1 && inner_w >= 12) {
        int sta_pair = es.dead ? pair_border_dead : pair_stamina;
        if (!es.dead && es.stamina >= es.max_stamina) {
            sta_pair = pair_stamina_full;
        }
        bool stamina_full_flash_active = full_stamina_flash_frame[entity_index] >= 0 &&
                                         (current_frame - full_stamina_flash_frame[entity_index]) < full_stamina_flash_duration;
        draw_bar(inner_y + row, inner_x, inner_w, es.stamina, es.max_stamina, "sta", sta_pair, false, stamina_full_flash_active);
        row++;
    }
    if (inner_h - row >= 1) {
        time_t now = time(nullptr);
        if (death_flash_active) {
            attron(COLOR_PAIR(pair_default) | A_BOLD | A_BLINK);
            mvprintw(inner_y + row, inner_x, "*** DEATH SHOCKWAVE ***");
            attroff(COLOR_PAIR(pair_default) | A_BOLD | A_BLINK);
            row++;
        }
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
            if (es.turn && is_player) {
                attron(COLOR_PAIR(pair_border_active) | A_BOLD | A_BLINK);
                mvprintw(inner_y + row, inner_x + 8, "<< YOUR TURN >>");
                attroff(COLOR_PAIR(pair_border_active) | A_BOLD | A_BLINK);
            } else if (es.turn && !is_player) {
                attron(COLOR_PAIR(pair_border_active) | A_BOLD | A_BLINK);
                mvprintw(inner_y + row, inner_x + 8, "** TARGET **");
                attroff(COLOR_PAIR(pair_border_active) | A_BOLD | A_BLINK);
            }
        }
        row++;
    }
    if (is_player && es.turn) {
        int bounce[4] = {1, 2, 1, 0};
        int shift = bounce[(current_frame / 3) % 4];
        int arrow_x = x + 2 + shift;
        if (arrow_x < x + w - 2) {
            attron(COLOR_PAIR(pair_border_active) | A_BOLD | A_BLINK);
            mvprintw(y, arrow_x, "▶");
            attroff(COLOR_PAIR(pair_border_active) | A_BOLD | A_BLINK);
        }
    }
    if (inner_h - row >= 2) {
        int inv_y = inner_y + inner_h - 2;
        int inv_h = 2;
        draw_box_at(inv_y, inner_x, inv_h, inner_w, border_pair, border_extra);
        draw_inventory_strip(inv_y + inv_h - 1, inner_x + 1, inner_w - 2, es.inventory, es.dead, entity_index);
        if (is_player && entity_index >= 0 && entity_index < game_state::max_players) {
            long long elapsed = current_frame - pickup_sweep_frame[entity_index];
            if (elapsed >= 0 && elapsed < 10) {
                int slots = (int)(((elapsed + 1) * game_state::inventory_slots) / 10);
                draw_inventory_sweep_overlay(inv_y + inv_h - 1, inner_x + 1, inner_w - 2, slots);
            }
        }
        attron(COLOR_PAIR(pair_panel_title) | A_BOLD);
        mvaddch(inv_y, inner_x + 2, ' ');
        mvprintw(inv_y, inner_x + 3, "inventory tetris");
        mvaddch(inv_y, inner_x + 19, ' ');
        attroff(COLOR_PAIR(pair_panel_title) | A_BOLD);
    } else if (inner_h - row >= 1) {
        draw_inventory_strip(inner_y + row, inner_x, inner_w, es.inventory, es.dead, entity_index);
    }
    if (entity_index >= 0 && entity_index < 13 && entity_hit_frames[entity_index] > 0) {
        entity_hit_frames[entity_index]--;
    }
}

void render_player_panel(int y, int x, int h, int w, const world_snapshot &snap) {
    paint_rect(y, x, h, w, pair_bg_player);
    draw_titled_box(y, x, h, w, "player squad", pair_title_player, A_BOLD);
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
        render_entity(drawn_y, inner_x, per_h, inner_w, title_buf, snap.players[i], true, i);
        drawn_y += per_h;
    }
}

void render_enemy_timeline_box(int y, int x, int h, int w, const world_snapshot &snap) {
    paint_rect(y, x, h, w, pair_bg_timeline);
    draw_titled_box(y, x, h, w, "enemy timeline", pair_title_enemy, A_BOLD);
    int inner_y = y + 1;
    int inner_x = x + 1;
    int inner_w = w - 2;
    int inner_h = h - 2;
    if (inner_h < 2 || inner_w < 20) {
        return;
    }
    int slots = game_state::kills_required_to_win;
    int cols = 5;
    int rows = (slots + cols - 1) / cols;
    int cell_h = inner_h / rows;
    if (cell_h < 2) {
        cell_h = 2;
    }
    int cell_w = inner_w / cols;
    if (cell_w < 6) {
        cell_w = 6;
    }
    for (int id = 1; id <= slots; ++id) {
        int idx = id - 1;
        int row = idx / cols;
        int col = idx % cols;
        int sy = inner_y + row * cell_h;
        int sx = inner_x + col * cell_w;
        int sh = cell_h;
        int sw = cell_w;
        if (sy + sh > inner_y + inner_h) {
            sh = inner_y + inner_h - sy;
        }
        if (sx + sw > inner_x + inner_w) {
            sw = inner_x + inner_w - sx;
        }
        if (sh < 2 || sw < 4) {
            continue;
        }
        bool alive = false;
        int alive_idx = -1;
        for (int i = 0; i < snap.active_enemy_count && i < game_state::max_enemies; ++i) {
            if (snap.enemy_display_id[i] == id && snap.enemies[i].hp > 0) {
                alive = true;
                alive_idx = i;
                break;
            }
        }
        bool dead = false;
        if (!alive) {
            if (id <= snap.total_kills) {
                dead = true;
            } else {
                for (int i = 0; i < snap.active_enemy_count && i < game_state::max_enemies; ++i) {
                    if (snap.enemy_display_id[i] > id) {
                        dead = true;
                        break;
                    }
                }
            }
        }
        int pair = pair_border_normal;
        int attr = A_BOLD;
        if (alive) {
            pair = pair_border_active;
        } else if (dead) {
            pair = pair_enemy_dead_purple;
        }
        draw_box_at(sy, sx, sh, sw, pair, attr);
        if (sw >= 6) {
            attron(COLOR_PAIR(pair) | A_BOLD);
            mvprintw(sy, sx + 1, "E%d", id);
            attroff(COLOR_PAIR(pair) | A_BOLD);
        }
        if (sh >= 3 && sw >= 8) {
            if (alive && alive_idx >= 0) {
                attron(COLOR_PAIR(pair_border_active) | A_BOLD);
                mvprintw(sy + 1, sx + 1, "hp %d", snap.enemies[alive_idx].hp);
                attroff(COLOR_PAIR(pair_border_active) | A_BOLD);
            } else if (dead) {
                attron(COLOR_PAIR(pair_enemy_dead_purple) | A_BOLD);
                mvprintw(sy + 1, sx + 1, "dead");
                attroff(COLOR_PAIR(pair_enemy_dead_purple) | A_BOLD);
            } else {
                attron(COLOR_PAIR(pair_default));
                mvprintw(sy + 1, sx + 1, "spawn");
                attroff(COLOR_PAIR(pair_default));
            }
        }
    }
}

vector<int> build_enemy_render_slots(const world_snapshot &snap) {
    vector<int> live_slots;
    vector<int> dead_slots;
    int active = snap.active_enemy_count;
    if (active < 0) {
        active = 0;
    }
    if (active > game_state::max_enemies) {
        active = game_state::max_enemies;
    }
    for (int i = 0; i < active; ++i) {
        if (snap.enemies[i].hp > 0) {
            live_slots.push_back(i);
        } else {
            dead_slots.push_back(i);
        }
    }
    vector<int> slots;
    for (int idx : live_slots) {
        slots.push_back(idx);
    }
    for (int idx : dead_slots) {
        slots.push_back(idx);
    }
    return slots;
}

void render_enemy_panel(int y, int x, int h, int w, const world_snapshot &snap) {
    paint_rect(y, x, h, w, pair_bg_enemy);
    draw_titled_box(y, x, h, w, "enemy forces", pair_title_enemy, A_BOLD);
    int inner_y = y + 1;
    int inner_x = x + 1;
    int inner_w = w - 2;
    int inner_h = h - 2;
    int timeline_h = 7;
    if (inner_h < 16) {
        timeline_h = 5;
    }
    if (timeline_h > inner_h - 4) {
        timeline_h = inner_h / 2;
    }
    if (timeline_h < 4) {
        timeline_h = 4;
    }
    render_enemy_timeline_box(inner_y, inner_x, timeline_h, inner_w, snap);
    int detail_y = inner_y + timeline_h;
    int detail_h = inner_h - timeline_h;
    if (detail_h < 4) {
        return;
    }
    vector<int> render_slots = build_enemy_render_slots(snap);
    int active = (int)render_slots.size();
    if (active <= 0) {
        active = 1;
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
    int per_h = detail_h / rows;
    if (per_h < 4) {
        per_h = 4;
    }
    int per_w = inner_w / columns;
    char title_buf[32];
    for (int i = 0; i < active; ++i) {
        if (i >= (int)render_slots.size()) {
            break;
        }
        int slot = render_slots[i];
        int col = i % columns;
        int row = i / columns;
        int box_y = detail_y + row * per_h;
        int box_x = inner_x + col * per_w;
        if (box_y + per_h > detail_y + detail_h) {
            break;
        }
        int eid = snap.enemy_display_id[slot];
        if (eid <= 0) {
            eid = slot + 1;
        }
        int fallen = snap.enemy_dead_count[slot];
        snprintf(title_buf, sizeof(title_buf), "E%d dmg%d d%d", eid, snap.enemies[slot].damage, fallen);
        render_entity(box_y, box_x, per_h, per_w, title_buf, snap.enemies[slot], false, game_state::max_players + slot);
    }
}

void render_banner(int y, int x, int w) {
    int rainbow_pairs[5] = {pair_bg_banner_1, pair_bg_banner_2, pair_bg_banner_3, pair_bg_banner_4, pair_bg_banner_5};
    if (w < 60) {
        attron(COLOR_PAIR(pair_bg_banner_3) | A_BOLD);
        const char *short_title = "  CHRONO RIFT  ";
        int len = (int)strlen(short_title);
        int sx = x + (w - len) / 2;
        mvprintw(y, sx, "%s", short_title);
        attroff(COLOR_PAIR(pair_bg_banner_3) | A_BOLD);
        return;
    }
    const char *line1 = "  ____ _   _ ____   ___  _   _  ___    ____  ___ _____ _____ ";
    const char *line2 = " / ___| | | |  _ \\ / _ \\| \\ | |/ _ \\  |  _ \\|_ _|  ___|_   _|";
    const char *line3 = "| |   | |_| | |_) | | | |  \\| | | | | | |_) || || |_    | |  ";
    const char *line4 = "| |___|  _  |  _ <| |_| | |\\  | |_| | |  _ < | ||  _|   | |  ";
    const char *line5 = " \\____|_| |_|_| \\_\\\\___/|_| \\_|\\___/  |_| \\_\\___|_|     |_|  ";
    const char *lines[] = {line1, line2, line3, line4, line5};
    int line_count = 5;
    int line_w = (int)strlen(line1);
    int sx = x + (w - line_w) / 2;
    if (sx < x + 1) {
        sx = x + 1;
    }
    for (int i = 0; i < line_count; ++i) {
        attron(COLOR_PAIR(rainbow_pairs[i]) | A_BOLD);
        mvprintw(y + i, sx, "%.*s", w - 2, lines[i]);
        attroff(COLOR_PAIR(rainbow_pairs[i]) | A_BOLD);
    }
}

void render_action_log_box(int y, int x, int h, int w, const world_snapshot &snap) {
    paint_rect(y, x, h, w, pair_bg_log);
    bool drop_flash_active = (current_frame - weapon_drop_flash_frame) < drop_flash_duration;
    if (drop_flash_active) {
        draw_titled_box(y, x, h, w, "action log", pair_artifact_solar, A_BOLD | A_BLINK);
    } else {
        draw_titled_box(y, x, h, w, "action log", pair_title_arena, A_BOLD);
    }
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
    bool deadlock_log_active = (current_frame - deadlock_log_frame) >= 0 && (current_frame - deadlock_log_frame) < 90;
    if (deadlock_log_active && inner_h > 2) {
        int msg_y = inner_y + inner_h - 2;
        attron(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
        mvprintw(msg_y, inner_x, "DEADLOCK FIXED: arbiter forced artifact drop");
        attroff(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
    }
    if (drop_flash_active && inner_h > 1) {
        int msg_y = inner_y + inner_h - 1;
        attron(COLOR_PAIR(pair_artifact_solar) | A_BOLD | A_BLINK);
        mvprintw(msg_y, inner_x, ">> WEAPON DROPPED <<");
        attroff(COLOR_PAIR(pair_artifact_solar) | A_BOLD | A_BLINK);
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
    bool kill_pop_active = (current_frame - kill_pop_frame) >= 0 && (current_frame - kill_pop_frame) < 6;
    if (kill_pop_active) {
        attron(COLOR_PAIR(pair_kill_counter) | A_BOLD | A_BLINK | A_REVERSE);
        mvprintw(y, x, "  KILLS %2d / %2d  ★ KILL ★  ", filled, total);
        attroff(COLOR_PAIR(pair_kill_counter) | A_BOLD | A_BLINK | A_REVERSE);
    } else {
        attron(COLOR_PAIR(pair_kill_counter) | A_BOLD);
        mvprintw(y, x, "KILLS %2d/%2d", filled, total);
        attroff(COLOR_PAIR(pair_kill_counter) | A_BOLD);
    }
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
    paint_rect(y, x, h, w, pair_bg_status);
    draw_titled_box(y, x, h, w, "system status", pair_title_arena, A_BOLD);
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
    paint_rect(y, x, h, w, pair_bg_artifact);
    draw_titled_box(y, x, h, w, "artifact tracker", pair_title_arena, A_BOLD);
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
    paint_rect(y, x, h, w, pair_bg_arena);
    bool drop_flash_active = (current_frame - weapon_drop_flash_frame) < drop_flash_duration;
    if (drop_flash_active) {
        draw_titled_box(y, x, h, w, "combat arena", pair_artifact_solar, A_BOLD | A_BLINK);
    } else {
        draw_titled_box(y, x, h, w, "combat arena", pair_title_arena, A_BOLD);
    }
    int inner_y = y + 1;
    int inner_x = x + 1;
    int inner_w = w - 2;
    int inner_h = h - 2;
    if (inner_h < 8 || inner_w < 10) {
        return;
    }
    int banner_h = (inner_w >= 60 && inner_h >= 18) ? 6 : 2;
    if (banner_h > inner_h - 8) {
        banner_h = 2;
    }
    render_banner(inner_y, inner_x, inner_w);
    int below_banner_y = inner_y + banner_h;
    int remaining_h = inner_h - banner_h;
    int log_h = remaining_h / 2;
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

void render_action_banner_overlay(int y, int x, int h, int w) {
    if (action_banner_frames <= 0) {
        return;
    }
    int text_len = (int)strlen(action_banner_text);
    if (text_len <= 0) {
        action_banner_frames = 0;
        return;
    }
    int box_w = text_len + 8;
    if (box_w < 24) {
        box_w = 24;
    }
    if (box_w > w - 4) {
        box_w = w - 4;
    }
    int box_h = 5;
    if (box_h > h - 2) {
        box_h = h - 2;
    }
    int box_y = y + (h - box_h) / 2;
    int box_x = x + (w - box_w) / 2;
    for (int yy = box_y; yy < box_y + box_h; ++yy) {
        attron(COLOR_PAIR(action_banner_pair) | A_REVERSE | A_BOLD);
        for (int xx = box_x; xx < box_x + box_w; ++xx) {
            mvaddch(yy, xx, ' ');
        }
        attroff(COLOR_PAIR(action_banner_pair) | A_REVERSE | A_BOLD);
    }
    int border_attr = A_BOLD;
    if ((action_banner_frames / 5) % 2 == 0) {
        border_attr |= A_BLINK;
    }
    draw_double_box_at(box_y, box_x, box_h, box_w, action_banner_pair, border_attr);
    attron(COLOR_PAIR(action_banner_pair) | A_REVERSE | A_BOLD);
    mvprintw(box_y + 2, box_x + (box_w - text_len) / 2, "%s", action_banner_text);
    attroff(COLOR_PAIR(action_banner_pair) | A_REVERSE | A_BOLD);
    action_banner_frames--;
}

void render_command_bar(int y, int term_w) {
    attron(COLOR_PAIR(pair_command_bar) | A_REVERSE | A_BOLD);
    for (int i = 0; i < term_w; ++i) {
        mvaddch(y, i, ' ');
    }
    const char *legend = " [1]strike [2]exhaust [3]heal [4]skip [5]pickup [6]ult [8]use [9]swap-select [T/TAB]target [H/J/K]cheat [?]help [q]quit ";
    int legend_len = (int)strlen(legend);
    int start = (term_w - legend_len) / 2;
    if (start < 0) {
        start = 0;
        legend_len = term_w;
    }
    mvprintw(y, start, "%.*s", legend_len, legend);
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
        "7  reserved    - no player action",
        "8  use weapon  - hit with best weapon in inventory",
        "9  swap in     - open storage selection overlay",
        "t/tab target   - cycle focused enemy target",
        "h  cheat ult   - force solar+lunar into active inventory",
        "j  cheat drop  - drop next collectible weapon on ground",
        "k  cheat dmg   - direct -50 hp on target enemy",
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

void render_swap_overlay(int term_h, int term_w, const world_snapshot &snap) {
    if (!show_swap_overlay || swap_overlay_player_id < 0 || swap_overlay_player_id >= game_state::max_players) {
        return;
    }
    int box_w = 74;
    int box_h = 12 + swap_overlay_option_count;
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
    mvprintw(box_y + 1, box_x + 2, "  swap in selection - player %d  ", swap_overlay_player_id + 1);
    attroff(COLOR_PAIR(pair_help) | A_BOLD | A_REVERSE);
    attron(COLOR_PAIR(pair_help) | A_BOLD);
    mvprintw(box_y + 3, box_x + 3, "press listed letter to select a stored weapon");
    mvprintw(box_y + 4, box_x + 3, "press x / 0 / esc to cancel");
    attroff(COLOR_PAIR(pair_help) | A_BOLD);

    int row = box_y + 6;
    if (swap_overlay_option_count <= 0) {
        attron(COLOR_PAIR(pair_status_warn) | A_BOLD);
        mvprintw(row, box_x + 3, "no weapons found in long-term storage");
        attroff(COLOR_PAIR(pair_status_warn) | A_BOLD);
        return;
    }

    int storage_count = 0;
    for (int s = 0; s < game_state::inventory_slots; ++s) {
        if (snap.players[swap_overlay_player_id].storage[s] > 0) {
            storage_count++;
        }
    }
    attron(COLOR_PAIR(pair_help));
    mvprintw(row, box_x + 3, "storage occupied slots: %d / %d", storage_count, game_state::storage_slots);
    attroff(COLOR_PAIR(pair_help));
    row += 2;

    for (int i = 0; i < swap_overlay_option_count && row < box_y + box_h - 1; ++i) {
        int weapon_id = swap_overlay_weapon_ids[i];
        char option_key = swap_overlay_option_keys[i];
        int dmg = weapon_damage_value(weapon_id);
        int slots = weapon_slot_size(weapon_id);
        int pair = weapon_color_pair_for(weapon_id);
        attron(COLOR_PAIR(pair) | A_BOLD);
        mvprintw(
            row, box_x + 3,
            "[%c] %-18s  dmg:%-3d  slots:%-2d",
            option_key,
            weapon_name(weapon_id),
            dmg,
            slots
        );
        attroff(COLOR_PAIR(pair) | A_BOLD);
        row++;
    }
}

void render_frame(const world_snapshot &snap) {
    pthread_mutex_lock(&ncurses_lock);
    if (resize_pending) {
        resize_pending = 0;
        refresh();
        clear();
    }
    erase();
    int term_h, term_w;
    getmaxyx(stdscr, term_h, term_w);
    for (int i = 0; i < 13; ++i) {
        for (int s = 0; s < game_state::inventory_slots; ++s) {
            if (inventory_new_item_frames[i][s] > 0) {
                inventory_new_item_frames[i][s]--;
            }
        }
    }
    for (int i = 0; i < game_state::max_players; ++i) {
        if (!snap.players[i].active) {
            continue;
        }
        int idx = i;
        int hp = snap.players[i].hp;
        if (prev_hp[idx] > 0 && hp < prev_hp[idx]) {
            hit_flash_frame[idx] = current_frame;
            entity_hit_frames[idx] = 30;
        }
        if (prev_hp[idx] > 0 && hp <= 0) {
            death_flash_frame[idx] = current_frame;
        }
        if (prev_stamina[idx] >= 0 && prev_stamina[idx] < snap.players[i].max_stamina &&
            snap.players[i].stamina >= snap.players[i].max_stamina) {
            full_stamina_flash_frame[idx] = current_frame;
        }
        prev_hp[idx] = hp;
        prev_stamina[idx] = snap.players[i].stamina;
        for (int s = 0; s < game_state::inventory_slots; ++s) {
            int item = snap.players[i].inventory[s];
            if (previous_inventory_items[idx][s] == 0 && item != 0) {
                inventory_new_item_frames[idx][s] = 60;
            }
            previous_inventory_items[idx][s] = item;
        }
    }
    for (int i = 0; i < game_state::max_enemies; ++i) {
        if (!snap.enemies[i].active) {
            continue;
        }
        int idx = game_state::max_players + i;
        int hp = snap.enemies[i].hp;
        int dead_count = snap.enemy_dead_count[i];
        if (prev_hp[idx] > 0 && hp < prev_hp[idx]) {
            hit_flash_frame[idx] = current_frame;
            entity_hit_frames[idx] = 30;
        }
        if (prev_hp[idx] > 0 && hp <= 0) {
            death_flash_frame[idx] = current_frame;
        }
        if (prev_enemy_dead_count[i] >= 0 && dead_count > prev_enemy_dead_count[i]) {
            death_flash_frame[idx] = current_frame;
            hit_flash_frame[idx] = current_frame;
        }
        prev_hp[idx] = hp;
        prev_stamina[idx] = snap.enemies[i].stamina;
        prev_enemy_dead_count[i] = dead_count;
        for (int s = 0; s < game_state::inventory_slots; ++s) {
            int item = snap.enemies[i].inventory[s];
            if (previous_inventory_items[idx][s] == 0 && item != 0) {
                inventory_new_item_frames[idx][s] = 60;
            }
            previous_inventory_items[idx][s] = item;
        }
    }
    if (!prev_ultimate_active && snap.ultimate_active) {
        snprintf(action_banner_text, sizeof(action_banner_text), "ultimate triggered");
        action_banner_pair = pair_artifact_solar;
        action_banner_frames = 60;
    }
    if (!prev_stun_active && snap.stun_active) {
        snprintf(action_banner_text, sizeof(action_banner_text), "stun pulse");
        action_banner_pair = pair_status_warn;
        action_banner_frames = 60;
    }
    if (prev_total_kills >= 0 && snap.total_kills > prev_total_kills) {
        snprintf(action_banner_text, sizeof(action_banner_text), "enemy %d eliminated", snap.total_kills);
        action_banner_pair = pair_overlay_lose;
        action_banner_frames = 60;
    }
    prev_ultimate_active = snap.ultimate_active ? 1 : 0;
    prev_stun_active = snap.stun_active ? 1 : 0;
    prev_total_kills = snap.total_kills;
    if (prev_dropped_weapon == 0 && snap.current_dropped_weapon != 0) {
        weapon_drop_flash_frame = current_frame;
    }
    prev_dropped_weapon = snap.current_dropped_weapon;
    if (term_h < 14 || term_w < 70) {
        attron(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
        mvprintw(0, 0, "terminal too small (need at least 70x14)");
        attroff(COLOR_PAIR(pair_status_warn) | A_BOLD | A_BLINK);
        refresh();
        pthread_mutex_unlock(&ncurses_lock);
        current_frame++;
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
    paint_rect(0, 0, main_h, left_w, pair_bg_player);
    paint_rect(0, left_w, main_h, center_w, pair_bg_arena);
    paint_rect(0, left_w + center_w, main_h, right_w, pair_bg_enemy);
    paint_rect(term_h - 1, 0, 1, term_w, pair_bg_footer);
    render_player_panel(0, 0, main_h, left_w, snap);
    render_arena_panel(0, left_w, main_h, center_w, snap);
    render_enemy_panel(0, left_w + center_w, main_h, right_w, snap);
    render_action_banner_overlay(0, left_w, main_h, center_w);
    render_command_bar(term_h - 1, term_w);
    if (snap.ultimate_active) {
        int pulse_pair = ((current_frame / 8) % 2 == 0) ? pair_artifact_solar : pair_artifact_lunar;
        draw_double_box_at(0, 0, term_h, term_w, pulse_pair, A_BOLD);
    }

    if (snap.outcome == game_state::outcome_win) {
        char line1[64];
        char line2[64];
        snprintf(line1, sizeof(line1), "all 10 enemies killed");
        snprintf(line2, sizeof(line2), "total kills %d  roll seed %d", snap.total_kills, snap.roll_number);
        render_overlay(term_h, term_w, "  V I C T O R Y  ", line1, line2, pair_overlay_win);
    } else if (snap.outcome == game_state::outcome_lose) {
        char line1[64];
        char line2[64];
        snprintf(line1, sizeof(line1), "%d enemies eliminated before defeat", snap.enemy_kills);
        snprintf(line2, sizeof(line2), "the rift consumed your party");
        render_overlay(term_h, term_w, "  D E F E A T  ", line1, line2, pair_overlay_lose);
    } else if (snap.outcome == game_state::outcome_quit) {
        render_overlay(term_h, term_w, "  S H U T D O W N  ", "session terminated", "thanks for playing chrono rift", pair_overlay_quit);
    } else if (show_swap_overlay) {
        render_swap_overlay(term_h, term_w, snap);
    } else if (show_help_overlay) {
        render_help_overlay(term_h, term_w);
    }

    refresh();
    pthread_mutex_unlock(&ncurses_lock);
    render_frame_counter++;
    current_frame++;
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

void init_color_pairs() {
    int c_canvas        = COLOR_BLACK;
    int c_player_bg     = COLOR_BLUE;
    int c_arena_bg      = COLOR_BLACK;
    int c_enemy_bg      = COLOR_BLACK;
    int c_footer_bg     = COLOR_BLACK;
    int c_player_box_bg = COLOR_BLACK;
    int c_enemy_box_bg  = COLOR_BLACK;
    int c_timeline_bg   = COLOR_BLACK;
    int c_log_bg        = COLOR_BLACK;
    int c_status_bg     = COLOR_BLACK;
    int c_artifact_bg   = COLOR_BLACK;

    int c_sky    = COLOR_CYAN;
    int c_green  = COLOR_GREEN;
    int c_amber  = COLOR_YELLOW;
    int c_orange = COLOR_YELLOW;
    int c_red    = COLOR_RED;
    int c_teal   = COLOR_CYAN;
    int c_indigo = COLOR_BLUE;
    int c_gold   = COLOR_YELLOW;
    int c_slate  = COLOR_WHITE;

    if (can_change_color()) {
        c_canvas        = 16;
        c_player_bg     = 17;
        c_arena_bg      = 18;
        c_enemy_bg      = 19;
        c_footer_bg     = 20;
        c_player_box_bg = 21;
        c_enemy_box_bg  = 22;
        c_timeline_bg   = 23;
        c_log_bg        = 24;
        c_status_bg     = 25;
        c_artifact_bg   = 26;
        c_sky    = 32;
        c_green  = 33;
        c_amber  = 34;
        c_orange = 35;
        c_red    = 36;
        c_teal   = 37;
        c_indigo = 38;
        c_gold   = 39;
        c_slate  = 40;

        init_color(c_canvas,        18,  20,  32);
        init_color(c_player_bg,      8,  14,  52);
        init_color(c_arena_bg,       6,  28,  32);
        init_color(c_enemy_bg,      22,   8,  42);
        init_color(c_footer_bg,      4,   4,  12);
        init_color(c_player_box_bg,  4,  10,  40);
        init_color(c_enemy_box_bg,  16,   5,  32);
        init_color(c_timeline_bg,    4,   4,  26);
        init_color(c_log_bg,         4,  18,  22);
        init_color(c_status_bg,      4,   8,  24);
        init_color(c_artifact_bg,    2,   5,  16);

        init_color(c_sky,    310, 760, 980);
        init_color(c_green,  490, 800, 510);
        init_color(c_amber, 1000, 720, 180);
        init_color(c_orange,1000, 560,  60);
        init_color(c_red,    900, 270, 250);
        init_color(c_teal,   200, 714, 694);
        init_color(c_indigo, 580, 490, 870);
        init_color(c_gold,  1000, 840, 100);
        init_color(c_slate,  480, 580, 660);
    }


    init_pair(pair_default,           COLOR_WHITE,    -1);

    init_pair(pair_border_normal,     c_slate,        -1);
    init_pair(pair_border_active,     c_sky,          -1);
    init_pair(pair_border_dead,       c_red,          -1);

    init_pair(pair_hp_high,           c_green,        -1);
    init_pair(pair_hp_med,            c_amber,        -1);
    init_pair(pair_hp_low,            c_orange,       -1);
    init_pair(pair_hp_critical,       c_red,          -1);

    init_pair(pair_stamina,           c_teal,         -1);
    init_pair(pair_stamina_full,      c_sky,          -1);

    init_pair(pair_log,               COLOR_WHITE,    -1);
    init_pair(pair_status_ok,         c_green,        -1);
    init_pair(pair_status_warn,       c_orange,       -1);

    init_pair(pair_artifact_solar,    c_gold,         -1);
    init_pair(pair_artifact_lunar,    c_sky,          -1);
    init_pair(pair_artifact_eclipse,  c_indigo,       -1);
    init_pair(pair_artifact_ground,   COLOR_WHITE,    -1);

    init_pair(pair_command_bar,       COLOR_WHITE,    c_footer_bg);

    init_pair(pair_inv_empty,         COLOR_BLACK,    COLOR_BLACK);
    init_pair(pair_inv_splinter,      c_green,        COLOR_BLACK);
    init_pair(pair_inv_venom,         c_teal,         COLOR_BLACK);
    init_pair(pair_inv_obsidian,      c_slate,        COLOR_BLACK);
    init_pair(pair_inv_frost,         c_sky,          COLOR_BLACK);
    init_pair(pair_inv_thunder,       c_amber,        COLOR_BLACK);
    init_pair(pair_inv_iron,          COLOR_WHITE,    COLOR_BLACK);
    init_pair(pair_inv_solar,         c_gold,         COLOR_BLACK);
    init_pair(pair_inv_lunar,         c_indigo,       COLOR_BLACK);
    init_pair(pair_inv_eclipse,       c_orange,       COLOR_BLACK);

    init_pair(pair_panel_title,       c_amber,        -1);
    init_pair(pair_arena_title,       c_teal,         -1);
    init_pair(pair_banner,            c_gold,         -1);

    init_pair(pair_kill_counter,      c_green,        -1);

    init_pair(pair_overlay_win,       c_green,        COLOR_BLACK);
    init_pair(pair_overlay_lose,      c_red,          COLOR_BLACK);
    init_pair(pair_overlay_quit,      c_sky,          COLOR_BLACK);

    init_pair(pair_help,              COLOR_WHITE,    c_canvas);

    init_pair(pair_enemy_dead_purple, c_red,          -1);

    init_pair(pair_bg_canvas,         COLOR_WHITE,    c_canvas);
    init_pair(pair_bg_player,         c_sky,          c_player_bg);
    init_pair(pair_bg_arena,          c_teal,         c_arena_bg);
    init_pair(pair_bg_enemy,          c_indigo,       c_enemy_bg);
    init_pair(pair_bg_footer,         COLOR_WHITE,    c_footer_bg);

    init_pair(pair_title_player,      c_sky,          c_player_bg);
    init_pair(pair_title_enemy,       c_indigo,       c_enemy_bg);
    init_pair(pair_title_arena,       c_teal,         c_arena_bg);

    init_pair(pair_bg_player_box,     COLOR_WHITE,    c_player_box_bg);
    init_pair(pair_bg_enemy_box,      COLOR_WHITE,    c_enemy_box_bg);
    init_pair(pair_bg_timeline,       COLOR_WHITE,    c_timeline_bg);
    init_pair(pair_bg_log,            COLOR_WHITE,    c_log_bg);
    init_pair(pair_bg_status,         COLOR_WHITE,    c_status_bg);
    init_pair(pair_bg_artifact,       COLOR_WHITE,    c_artifact_bg);

    init_pair(pair_bg_banner_1,       c_sky,          c_arena_bg);
    init_pair(pair_bg_banner_2,       c_sky,          c_arena_bg);
    init_pair(pair_bg_banner_3,       c_sky,          c_arena_bg);
    init_pair(pair_bg_banner_4,       c_sky,          c_arena_bg);
    init_pair(pair_bg_banner_5,       c_sky,          c_arena_bg);

    init_pair(pair_border_v1,         c_sky,          -1);
    init_pair(pair_border_v2,         c_teal,         -1);
    init_pair(pair_border_v3,         c_green,        -1);
    init_pair(pair_border_v4,         c_amber,        -1);
    init_pair(pair_border_v5,         c_indigo,       -1);
    init_pair(pair_border_v6,         c_orange,       -1);
    init_pair(pair_border_v7,         c_gold,         -1);
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
    bkgd(COLOR_PAIR(pair_bg_canvas));
    return true;
}

int clamp_party_size(int value) {
    if (value < 1) {
        return 1;
    }
    if (value > game_state::max_players) {
        return game_state::max_players;
    }
    return value;
}

void init_intro_colors() {
    if (!has_colors()) {
        return;
    }
    start_color();
    use_default_colors();
    init_pair(1, COLOR_BLUE, -1);
    init_pair(2, COLOR_CYAN, -1);
    init_pair(3, COLOR_GREEN, -1);
    init_pair(4, COLOR_MAGENTA, -1);
    init_pair(5, COLOR_YELLOW, -1);
    init_pair(6, COLOR_WHITE, -1);
    init_pair(7, COLOR_BLACK, COLOR_CYAN);
    init_pair(8, COLOR_BLACK, COLOR_GREEN);
}

void draw_intro_particles(int frame, int h, int w) {
    int count = 42;
    for (int i = 0; i < count; ++i) {
        int x = (frame * 3 + i * 17) % w;
        int y = (frame + i * 7) % h;
        int pair = 1 + (i % 5);
        char ch = ((i + frame) % 4 == 0) ? '*' : '.';
        attron(COLOR_PAIR(pair) | A_DIM);
        mvaddch(y, x, ch);
        attroff(COLOR_PAIR(pair) | A_DIM);
    }
}

void draw_intro_banner(int frame, int h, int w) {
    const char *line1 = "  ____ _   _ ____   ___  _   _  ___    ____  ___ _____ _____ ";
    const char *line2 = " / ___| | | |  _ \\ / _ \\| \\ | |/ _ \\  |  _ \\|_ _|  ___|_   _|";
    const char *line3 = "| |   | |_| | |_) | | | |  \\| | | | | | |_) || || |_    | |  ";
    const char *line4 = "| |___|  _  |  _ <| |_| | |\\  | |_| | |  _ < | ||  _|   | |  ";
    const char *line5 = " \\____|_| |_|_| \\_\\\\___/|_| \\_|\\___/  |_| \\_\\___|_|     |_|  ";
    const char *lines[] = {line1, line2, line3, line4, line5};
    int line_count = 5;
    int line_w = (int)strlen(line1);
    int sx = (w - line_w) / 2;
    if (sx < 1) {
        sx = 1;
    }
    int sy = (h / 2) - 8;
    if (sy < 1) {
        sy = 1;
    }
    for (int i = 0; i < line_count; ++i) {
        int pair = 2 + ((frame / 3 + i) % 4);
        attron(COLOR_PAIR(pair) | A_BOLD);
        mvprintw(sy + i, sx, "%.*s", w - 2, lines[i]);
        attroff(COLOR_PAIR(pair) | A_BOLD);
    }
}

void draw_intro_party_box(int frame, int h, int w, int selected) {
    int box_w = 52;
    int box_h = 9;
    int y = (h / 2) + 1;
    int x = (w - box_w) / 2;
    if (x < 2) {
        x = 2;
    }
    if (y + box_h >= h) {
        y = h - box_h - 1;
    }
    if (y < 1) {
        y = 1;
    }
    int border_pair = 2 + ((frame / 4) % 4);
    draw_double_box_at(y, x, box_h, box_w, border_pair, A_BOLD);
    attron(COLOR_PAIR(6) | A_BOLD);
    mvprintw(y + 1, x + 2, "select party size");
    attroff(COLOR_PAIR(6) | A_BOLD);
    int ox = x + 6;
    int oy = y + 3;
    for (int i = 1; i <= game_state::max_players; ++i) {
        int px = ox + (i - 1) * 10;
        if (i == selected) {
            int pair = 7 + ((frame / 5) % 2);
            attron(COLOR_PAIR(pair) | A_BOLD | A_REVERSE);
            mvprintw(oy, px, "  [%d]  ", i);
            attroff(COLOR_PAIR(pair) | A_BOLD | A_REVERSE);
        } else {
            attron(COLOR_PAIR(6) | A_BOLD);
            mvprintw(oy, px, "   %d   ", i);
            attroff(COLOR_PAIR(6) | A_BOLD);
        }
    }
    attron(COLOR_PAIR(6));
    mvprintw(y + 6, x + 2, "keys: 1-4 choose   enter start   arrows cycle");
    attroff(COLOR_PAIR(6));
}

int run_intro_party_prompt(int initial) {
    int selected = clamp_party_size(initial);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    noecho();
    cbreak();
    curs_set(0);
    int frame = 0;
    while (true) {
        int h, w;
        getmaxyx(stdscr, h, w);
        erase();
        draw_intro_particles(frame, h, w);
        draw_intro_banner(frame, h, w);
        draw_intro_party_box(frame, h, w, selected);
        refresh();
        int ch = getch();
        if (ch >= '1' && ch <= '4') {
            selected = clamp_party_size(ch - '0');
        } else if (ch == KEY_LEFT) {
            selected--;
            if (selected < 1) {
                selected = game_state::max_players;
            }
        } else if (ch == KEY_RIGHT) {
            selected++;
            if (selected > game_state::max_players) {
                selected = 1;
            }
        } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER || ch == ' ') {
            return selected;
        }
        frame++;
        interruptible_usleep(50000);
    }
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
    if (chosen >= 1 && chosen <= game_state::max_players) {
        return clamp_party_size(chosen);
    }
    if (initscr() != nullptr) {
        init_intro_colors();
        chosen = run_intro_party_prompt(2);
        endwin();
        return clamp_party_size(chosen);
    }
    chosen = 2;
    if (chosen < 1) {
        chosen = 1;
    }
    return chosen;
}

void cleanup_tui_once() {
    endwin();
}
