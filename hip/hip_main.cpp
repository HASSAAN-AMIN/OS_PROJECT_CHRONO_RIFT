#include "hip_logic.h"
#include "hip_ui.h"

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

void cleanup_resources() {
    destroy_locks();
    unmap_shared_memory();
    close_shared_fd();
}

int main(int argc, char **argv) {
    chosen_party_size = prompt_party_size(argc, argv);
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
    if (!apply_party_size_to_state(chosen_party_size)) {
        cleanup_resources();
        return 1;
    }
    if (!init_tui()) {
        cleanup_resources();
        return 1;
    }
    initialize_animation_state();
    if (!spawn_threads()) {
        cleanup_tui_once();
        cleanup_resources();
        return 1;
    }
    pthread_t outcome_thread;
    bool outcome_thread_started = false;
    if (pthread_create(&outcome_thread, nullptr, outcome_watch_loop, nullptr) != 0) {
        print_errno("pthread_create outcome watch failed");
    } else {
        outcome_thread_started = true;
    }
    join_threads();
    if (outcome_thread_started) {
        pthread_join(outcome_thread, nullptr);
    }
    cleanup_tui_once();
    cleanup_resources();
    return 0;
}
