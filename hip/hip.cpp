#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <ncurses.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../gamestate.h"

static const char *shared_memory_name = "/chrono_rift_game_state";
static const int max_stat_value = 100;
static const useconds_t frame_sleep_us = 16666;

struct hip_snapshot {
    int player_hp[game_state::max_players];
    int player_stamina[game_state::max_players];
    int enemy_hp[game_state::max_enemies];
    int enemy_stamina[game_state::max_enemies];
    int player_primary_inventory[game_state::max_players][game_state::inventory_slots];
    int long_term_storage[game_state::max_players][game_state::inventory_slots];
    int solar_core_holder;
    int lunar_blade_holder;
    int eclipse_relic_holder;
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

static int shared_memory_fd = -1;
static game_state *shared_state = nullptr;
static volatile sig_atomic_t running = 1;
static pthread_t render_thread;
static bool ncurses_ready = false;
static window_set windows = {nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0};

static void print_errno(const char *action) {
    std::fprintf(stderr, "%s: %s\n", action, std::strerror(errno));
}

static int clamp_value(int value, int low, int high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static bool open_shared_memory() {
    shared_memory_fd = shm_open(shared_memory_name, O_RDWR, 0600);
    if (shared_memory_fd < 0) {
        print_errno("shm_open failed");
        return false;
    }
    std::printf("shared memory linked\n");
    return true;
}

static bool map_shared_memory() {
    void *mapped = mmap(nullptr, sizeof(game_state), PROT_READ | PROT_WRITE, MAP_SHARED, shared_memory_fd, 0);
    if (mapped == MAP_FAILED) {
        print_errno("mmap failed");
        return false;
    }
    shared_state = static_cast<game_state *>(mapped);
    std::printf("shared memory mapped\n");
    return true;
}

static void close_shared_memory_fd() {
    if (shared_memory_fd >= 0) {
        if (close(shared_memory_fd) != 0) {
            print_errno("close failed");
        }
        shared_memory_fd = -1;
    }
}

static void unmap_shared_memory() {
    if (shared_state == nullptr) {
        return;
    }
    if (munmap(shared_state, sizeof(game_state)) != 0) {
        print_errno("munmap failed");
    }
    shared_state = nullptr;
}

static bool lock_memory() {
    if (shared_state == nullptr) {
        return false;
    }
    while (sem_wait(&shared_state->memory_sem) != 0) {
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

static bool unlock_memory() {
    if (shared_state == nullptr) {
        return false;
    }
    if (sem_post(&shared_state->memory_sem) != 0) {
        print_errno("sem_post failed");
        return false;
    }
    return true;
}

static bool copy_snapshot(hip_snapshot *snapshot) {
    if (snapshot == nullptr) {
        return false;
    }
    if (!lock_memory()) {
        return false;
    }
    for (int i = 0; i < game_state::max_players; ++i) {
        snapshot->player_hp[i] = shared_state->player_hp[i];
        snapshot->player_stamina[i] = shared_state->player_stamina[i];
        for (int j = 0; j < game_state::inventory_slots; ++j) {
            snapshot->player_primary_inventory[i][j] = shared_state->player_primary_inventory[i][j];
            snapshot->long_term_storage[i][j] = shared_state->long_term_storage[i][j];
        }
    }
    for (int i = 0; i < game_state::max_enemies; ++i) {
        snapshot->enemy_hp[i] = shared_state->enemy_hp[i];
        snapshot->enemy_stamina[i] = shared_state->enemy_stamina[i];
    }
    snapshot->solar_core_holder = shared_state->solar_core_holder;
    snapshot->lunar_blade_holder = shared_state->lunar_blade_holder;
    snapshot->eclipse_relic_holder = shared_state->eclipse_relic_holder;
    if (!unlock_memory()) {
        return false;
    }
    return true;
}

static void destroy_window(WINDOW **target) {
    if (*target == nullptr) {
        return;
    }
    delwin(*target);
    *target = nullptr;
}

static void destroy_windows() {
    destroy_window(&windows.status);
    destroy_window(&windows.players);
    destroy_window(&windows.enemies);
    destroy_window(&windows.inventory);
    destroy_window(&windows.action_log);
}

static bool create_windows_for_size(int rows, int cols) {
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

static bool ensure_windows() {
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

static bool init_ncurses_ui() {
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_GREEN, -1);
        init_pair(2, COLOR_CYAN, -1);
        init_pair(3, COLOR_RED, -1);
        init_pair(4, COLOR_MAGENTA, -1);
        init_pair(5, COLOR_YELLOW, -1);
        init_pair(6, COLOR_WHITE, -1);
    }
    if (!ensure_windows()) {
        return false;
    }
    ncurses_ready = true;
    return true;
}

static void shutdown_ncurses_ui() {
    if (!ncurses_ready) {
        return;
    }
    destroy_windows();
    endwin();
    ncurses_ready = false;
}

static void draw_bar(WINDOW *target, int row, const char *label, int value, int color_pair_id) {
    int h = 0;
    int w = 0;
    getmaxyx(target, h, w);
    int bar_w = w - 22;
    if (bar_w < 10) {
        bar_w = 10;
    }
    int clean = clamp_value(value, 0, max_stat_value);
    int fill = (clean * bar_w) / max_stat_value;
    mvwprintw(target, row, 2, "%s %3d", label, clean);
    wattron(target, COLOR_PAIR(color_pair_id));
    for (int i = 0; i < bar_w; ++i) {
        mvwaddch(target, row, 12 + i, i < fill ? ' ' | A_REVERSE : ' ');
    }
    wattroff(target, COLOR_PAIR(color_pair_id));
}

static void draw_system_status(const hip_snapshot *snapshot, unsigned long frame_id) {
    werase(windows.status);
    box(windows.status, 0, 0);
    wattron(windows.status, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(windows.status, 0, 2, " system status ");
    wattroff(windows.status, COLOR_PAIR(5) | A_BOLD);
    std::time_t now = std::time(nullptr);
    mvwprintw(windows.status, 1, 2, "frame %lu", frame_id);
    mvwprintw(windows.status, 1, 20, "target fps 60");
    mvwprintw(windows.status, 1, 38, "time %ld", static_cast<long>(now));
    mvwprintw(
        windows.status, 2, 2, "solar %d  lunar %d  eclipse %d",
        snapshot->solar_core_holder, snapshot->lunar_blade_holder, snapshot->eclipse_relic_holder
    );
    mvwprintw(windows.status, 3, 2, "press q to exit hip");
}

static void draw_players(const hip_snapshot *snapshot) {
    werase(windows.players);
    box(windows.players, 0, 0);
    wattron(windows.players, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(windows.players, 0, 2, " player squad ");
    wattroff(windows.players, COLOR_PAIR(1) | A_BOLD);
    int row = 1;
    for (int i = 0; i < game_state::max_players; ++i) {
        char hp_label[32];
        char stamina_label[32];
        std::snprintf(hp_label, sizeof(hp_label), "p%d hp", i + 1);
        std::snprintf(stamina_label, sizeof(stamina_label), "p%d st", i + 1);
        draw_bar(windows.players, row++, hp_label, snapshot->player_hp[i], 1);
        draw_bar(windows.players, row++, stamina_label, snapshot->player_stamina[i], 2);
    }
}

static void draw_enemies(const hip_snapshot *snapshot) {
    werase(windows.enemies);
    box(windows.enemies, 0, 0);
    wattron(windows.enemies, COLOR_PAIR(3) | A_BOLD);
    mvwprintw(windows.enemies, 0, 2, " enemy bots ");
    wattroff(windows.enemies, COLOR_PAIR(3) | A_BOLD);
    int row = 1;
    int h = 0;
    int w = 0;
    getmaxyx(windows.enemies, h, w);
    int usable = h - 2;
    if (usable < 1) {
        return;
    }
    int col_split = w / 2;
    for (int i = 0; i < game_state::max_enemies; ++i) {
        int block = i % usable;
        int col = i / usable;
        int x = col == 0 ? 2 : col_split + 1;
        int hp = clamp_value(snapshot->enemy_hp[i], 0, max_stat_value);
        int st = clamp_value(snapshot->enemy_stamina[i], 0, max_stat_value);
        wattron(windows.enemies, COLOR_PAIR(3));
        mvwprintw(windows.enemies, 1 + block, x, "e%d hp %3d", i + 1, hp);
        wattroff(windows.enemies, COLOR_PAIR(3));
        wattron(windows.enemies, COLOR_PAIR(4));
        mvwprintw(windows.enemies, 1 + block, x + 10, "st %3d", st);
        wattroff(windows.enemies, COLOR_PAIR(4));
    }
    (void)row;
}

static void draw_inventory_row(
    WINDOW *target, int row, const int *items, int count, const char *label, int color_pair_id
) {
    mvwprintw(target, row, 2, "%s", label);
    wattron(target, COLOR_PAIR(color_pair_id));
    for (int i = 0; i < count; ++i) {
        char cell = items[i] == 0 ? '.' : '#';
        mvwaddch(target, row, 8 + i, cell);
    }
    wattroff(target, COLOR_PAIR(color_pair_id));
}

static void draw_inventory(const hip_snapshot *snapshot) {
    werase(windows.inventory);
    box(windows.inventory, 0, 0);
    wattron(windows.inventory, COLOR_PAIR(2) | A_BOLD);
    mvwprintw(windows.inventory, 0, 2, " inventory tetris ");
    wattroff(windows.inventory, COLOR_PAIR(2) | A_BOLD);
    for (int i = 0; i < game_state::max_players; ++i) {
        char label[16];
        std::snprintf(label, sizeof(label), "p%d inv", i + 1);
        draw_inventory_row(
            windows.inventory, 1 + i, snapshot->player_primary_inventory[i], game_state::inventory_slots, label, 2
        );
    }
    for (int i = 0; i < game_state::max_players; ++i) {
        char label[16];
        std::snprintf(label, sizeof(label), "p%d sto", i + 1);
        draw_inventory_row(
            windows.inventory, 7 + i, snapshot->long_term_storage[i], game_state::inventory_slots, label, 5
        );
    }
}

static void draw_action_log(const hip_snapshot *snapshot, unsigned long frame_id) {
    werase(windows.action_log);
    box(windows.action_log, 0, 0);
    wattron(windows.action_log, COLOR_PAIR(6) | A_BOLD);
    mvwprintw(windows.action_log, 0, 2, " combat action log ");
    wattroff(windows.action_log, COLOR_PAIR(6) | A_BOLD);
    mvwprintw(windows.action_log, 1, 2, "frame pulse %lu", frame_id);
    mvwprintw(windows.action_log, 2, 2, "solar core holder: %d", snapshot->solar_core_holder);
    mvwprintw(windows.action_log, 3, 2, "lunar blade holder: %d", snapshot->lunar_blade_holder);
    mvwprintw(windows.action_log, 4, 2, "eclipse relic holder: %d", snapshot->eclipse_relic_holder);
    mvwprintw(windows.action_log, 6, 2, "squad ready: %d/%d", game_state::max_players, game_state::max_players);
    mvwprintw(windows.action_log, 7, 2, "enemy active: %d", game_state::max_enemies);
    mvwprintw(windows.action_log, 9, 2, "input: q quits hip");
}

static void render_all(const hip_snapshot *snapshot, unsigned long frame_id) {
    if (!ensure_windows()) {
        return;
    }
    draw_system_status(snapshot, frame_id);
    draw_players(snapshot);
    draw_enemies(snapshot);
    draw_inventory(snapshot);
    draw_action_log(snapshot, frame_id);
    wrefresh(windows.status);
    wrefresh(windows.players);
    wrefresh(windows.enemies);
    wrefresh(windows.inventory);
    wrefresh(windows.action_log);
}

static void *render_loop(void *) {
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
        render_all(&snapshot, frame_id++);
        int key = getch();
        if (key == 'q' || key == 'Q') {
            running = 0;
            break;
        }
        usleep(frame_sleep_us);
    }
    shutdown_ncurses_ui();
    return nullptr;
}

static void handle_signal(int) {
    running = 0;
}

static bool register_signals() {
    if (std::signal(SIGINT, handle_signal) == SIG_ERR) {
        std::fprintf(stderr, "failed to register sigint handler\n");
        return false;
    }
    if (std::signal(SIGTERM, handle_signal) == SIG_ERR) {
        std::fprintf(stderr, "failed to register sigterm handler\n");
        return false;
    }
    return true;
}

static bool start_render_thread() {
    if (pthread_create(&render_thread, nullptr, render_loop, nullptr) != 0) {
        print_errno("pthread_create failed");
        return false;
    }
    std::printf("ui thread started\n");
    return true;
}

static void join_render_thread() {
    pthread_join(render_thread, nullptr);
}

static void cleanup() {
    shutdown_ncurses_ui();
    unmap_shared_memory();
    close_shared_memory_fd();
}

int main() {
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
    if (!start_render_thread()) {
        cleanup();
        return 1;
    }
    while (running) {
        usleep(50000);
    }
    join_render_thread();
    cleanup();
    std::printf("hip exited cleanly\n");
    return 0;
}
