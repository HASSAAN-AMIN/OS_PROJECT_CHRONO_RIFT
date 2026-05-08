#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../gamestate.h"

static const char *shared_memory_name = "/chrono_rift_game_state";

static int shared_memory_fd = -1;
static game_state *shared_state = nullptr;

static void print_errno(const char *action) {
    std::fprintf(stderr, "%s: %s\n", action, std::strerror(errno));
}

static bool open_shared_memory() {
    shared_memory_fd = shm_open(shared_memory_name, O_CREAT | O_RDWR, 0600);
    if (shared_memory_fd < 0) {
        print_errno("shm_open failed");
        return false;
    }
    std::printf("shared memory opened\n");
    return true;
}

static bool size_shared_memory() {
    if (ftruncate(shared_memory_fd, sizeof(game_state)) != 0) {
        print_errno("ftruncate failed");
        return false;
    }
    std::printf("shared memory sized\n");
    return true;
}

static bool map_shared_memory() {
    void *mapped = mmap(nullptr, sizeof(game_state), PROT_READ | PROT_WRITE, MAP_SHARED, shared_memory_fd, 0);
    if (mapped == MAP_FAILED) {
        print_errno("mmap failed");
        return false;
    }
    shared_state = static_cast<game_state *>(mapped);
    std::printf("shared memory initialized\n");
    return true;
}

static bool initialize_semaphore(sem_t *target, unsigned int value, const char *name) {
    if (sem_init(target, 1, value) != 0) {
        std::fprintf(stderr, "sem_init failed for %s: %s\n", name, std::strerror(errno));
        return false;
    }
    return true;
}

static bool initialize_semaphores() {
    if (!initialize_semaphore(&shared_state->memory_sem, 1, "memory_sem")) {
        return false;
    }
    if (!initialize_semaphore(&shared_state->player_sem, 1, "player_sem")) {
        return false;
    }
    if (!initialize_semaphore(&shared_state->enemy_sem, 1, "enemy_sem")) {
        return false;
    }
    if (!initialize_semaphore(&shared_state->inventory_sem, 1, "inventory_sem")) {
        return false;
    }
    if (!initialize_semaphore(&shared_state->relic_sem, 1, "relic_sem")) {
        return false;
    }
    std::printf("semaphores linked\n");
    return true;
}

static void clear_state() {
    std::memset(shared_state, 0, sizeof(game_state));
}

static void close_descriptor() {
    if (shared_memory_fd >= 0) {
        close(shared_memory_fd);
        shared_memory_fd = -1;
    }
}

static void cleanup() {
    if (shared_state != nullptr) {
        munmap(shared_state, sizeof(game_state));
        shared_state = nullptr;
    }
    close_descriptor();
    if (shm_unlink(shared_memory_name) != 0 && errno != ENOENT) {
        print_errno("shm_unlink failed");
    } else {
        std::printf("shared memory unlinked\n");
    }
}

static void handle_signal(int) {
    std::exit(0);
}

static bool register_exit_handlers() {
    if (std::atexit(cleanup) != 0) {
        std::fprintf(stderr, "failed to register cleanup\n");
        return false;
    }
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

static bool setup_shared_state() {
    if (!open_shared_memory()) {
        return false;
    }
    if (!size_shared_memory()) {
        return false;
    }
    if (!map_shared_memory()) {
        return false;
    }
    clear_state();
    if (!initialize_semaphores()) {
        return false;
    }
    return true;
}

int main() {
    if (!register_exit_handlers()) {
        return 1;
    }
    if (!setup_shared_state()) {
        return 1;
    }
    std::printf("arbiter ready\n");
    pause();
    return 0;
}
