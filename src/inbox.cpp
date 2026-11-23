/**
 * @file inbox.cpp
 * @brief Receive ring buffer and the main-loop wake-up fd.
 */
#include "shared.h"

namespace synctext {

// State used only by this file.
static std::mutex g_network_rx_mutex;

std::atomic<uint64_t> g_rx_total{0};
std::atomic<uint64_t> g_rx_dropped{0};
int  g_debug_level = 0;   // 0=off,1=basic,2=verbose,3=trace

// Lock-free SPSC ring for received updates (listener -> main thread)
std::vector<Update> g_rx_ring; // sized at startup (using RX_RING_CAP from header)
std::atomic<size_t> g_rx_head{0}; // producer index (listener)
std::atomic<size_t> g_rx_tail{0}; // consumer index (main thread)
static inline bool rx_ring_push(Update&& u) {
    size_t head = g_rx_head.load(std::memory_order_relaxed);
    size_t tail = g_rx_tail.load(std::memory_order_acquire);
    if (head - tail >= RX_RING_CAP) return false; // full
    g_rx_ring[head % RX_RING_CAP] = std::move(u);
    g_rx_head.store(head + 1, std::memory_order_release);
    return true;
}
void enqueue_received_update(Update&& u) {
    g_rx_total.fetch_add(1, std::memory_order_relaxed);
    if (g_network_mode) {
        std::lock_guard<std::mutex> lock(g_network_rx_mutex);
        if (!rx_ring_push(std::move(u))) g_rx_dropped.fetch_add(1, std::memory_order_relaxed);
    } else {
        if (!rx_ring_push(std::move(u))) g_rx_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

// Wake-up fd to nudge main loop immediately on inbound messages (no locks)
int g_wake_fd = -1;
void init_wake_fd() {
    if (g_wake_fd >= 0) return;
    g_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

} // namespace synctext
