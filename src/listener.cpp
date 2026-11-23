/**
 * @file listener.cpp
 * @brief Thread that receives updates from our message queue.
 */
#include "shared.h"

namespace synctext {

// State used only by this file.
static std::atomic<uint64_t> g_rx_receive_errors{0}; // unexpected mq_receive errors

void listener_loop() {
    if (g_mq == (mqd_t)-1) return;
    // Determine actual queue message size; allocate receive buffer accordingly
    struct mq_attr cur{};
    if (mq_getattr(g_mq, &cur) != 0) {
        // Fallback to default if we can't query
        cur.mq_msgsize = MQ_MSGSIZE;
    }
    size_t rx_cap = (size_t)cur.mq_msgsize;
    if (rx_cap < 256) rx_cap = 256; // sanity floor
    std::vector<char> buf(rx_cap);
    // Wait in the kernel instead of polling. The queue was opened non-blocking; only
    // this thread reads it, so switch the descriptor to blocking and receive with a
    // short timeout: a message wakes us the moment it is queued, and the timeout is
    // only there so a shutdown request (g_stop) is noticed.
    struct mq_attr blocking{};
    blocking.mq_flags = 0;
    mq_setattr(g_mq, &blocking, nullptr);
    while (!g_stop) {
        struct timespec deadline{};
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 100 * 1000000L;              // 100 ms
        if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec += 1; deadline.tv_nsec -= 1000000000L; }
        ssize_t n = mq_timedreceive(g_mq, buf.data(), buf.size(), nullptr, &deadline);
        if (n >= 0) {
            Update u{};
            if (deserialize_update(buf.data(), (size_t)n, u)) {
                enqueue_received_update(Update(u));
                wake_main_loop();
                if (g_debug_level >= 2) {
                    std::cerr << "[debug] received msg from user=" << u.user_id << " line=" << u.line_no << " type=" << (int)u.type << " size=" << n << "\n";
                }
            } else {
                g_rx_dropped.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        if (errno == ETIMEDOUT || errno == EINTR || errno == EAGAIN) continue;
        // Unexpected receive error
        g_rx_receive_errors.fetch_add(1, std::memory_order_relaxed);
    if (g_debug_level >= 3) {
            std::cerr << "[debug] mq_receive error errno=" << errno << "\n";
        }
        usleep(50 * 1000);
    }
}

} // namespace synctext
