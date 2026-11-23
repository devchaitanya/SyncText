/**
 * @file common.cpp
 * @brief Clock and signal handling.
 */
#include "shared.h"

namespace synctext {


/**
 * @brief Get current time in nanoseconds since Unix epoch
 * @return Nanosecond timestamp (uint64_t)
 * 
 * Uses CLOCK_REALTIME (wall clock). Used for heartbeats, timeouts and caret expiry.
 * Merge order does NOT depend on it: the CRDT orders edits by Lamport counters.
 */
uint64_t now_ns() {
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/**
 * @brief Signal handler for graceful shutdown
 * @param sig Signal number (unused)
 * 
 * Sets g_stop flag which triggers cleanup in main loop.
 * Handles SIGINT (Ctrl+C) and SIGTERM (kill command).
 */
static void handle_signal(int) {
    g_stop = 1;
}

/**
 * @brief Install signal handlers for clean exit
 * 
 * Registers handle_signal() for SIGINT and SIGTERM.
 * Allows deregistration from registry and queue cleanup before exit.
 */
void install_signal_handlers() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
}

} // namespace synctext
