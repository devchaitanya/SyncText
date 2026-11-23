/**
 * @file broadcast.cpp
 * @brief Sending local updates to every peer.
 */
#include "shared.h"

namespace synctext {

// State used only by this file.
static std::atomic<uint64_t> g_tx_sent_total{0};
static std::atomic<uint64_t> g_tx_send_errors{0};
static std::atomic<uint64_t> g_tx_open_errors{0}; // failed mq_open for recipient


// Outgoing accumulation buffer
std::vector<Update> g_tx_pending;

/// Another participant's slot that is registered, alive and heartbeating.
static bool slot_is_live_peer(const UserEntry& slot) {
    if (slot.active.load(std::memory_order_relaxed) != 2) return false;
    if (std::strncmp(slot.user_id, g_user_id.c_str(), USER_ID_MAX) == 0) return false;
    bool alive = (kill(slot.pid, 0) == 0) || (errno == EPERM);
    uint64_t hb = slot.last_heartbeat_ns.load(std::memory_order_relaxed);
    uint64_t now = now_ns();
    bool fresh = now >= hb && (now - hb) <= g_stale_ns;
    return alive && fresh;
}

bool local_peer_alive() {
    if (g_network_mode || !g_registry) return false;
    for (size_t i = 0; i < MAX_USERS; ++i)
        if (slot_is_live_peer(g_registry->users[i])) return true;
    return false;
}

/// Deliver ops[0..count) to one message queue, retrying while it is full.
static void send_to_queue(const char* queue_name, const std::vector<Update>& ops, size_t count) {
    mqd_t mq = mq_open(queue_name, O_WRONLY | O_NONBLOCK);
    if (mq == (mqd_t)-1) {
        g_tx_open_errors.fetch_add(1, std::memory_order_relaxed);
        if (g_debug_level >= 3) {
            std::cerr << "[debug] mq_open failed for recipient queue='" << queue_name << "' errno=" << errno << "\n";
        }
        return;
    }
    char msg[MQ_MSGSIZE]; size_t mlen=0;
    for (size_t k = 0; k < count; ++k) {
        const auto& u = ops[k];
        if (!serialize_update(u, msg, sizeof(msg), mlen)) {
            if (g_debug_level >= 3) {
                std::cerr << "[debug] serialize FAILED type=" << (int)u.type << " content_len=" << u.new_content.size() << "\n";
            }
            continue;
        }
        int attempt = 0;
        while (attempt <= g_send_retry_count) {
            if (mq_send(mq, msg, mlen, 0) == 0) {
                g_tx_sent_total.fetch_add(1, std::memory_order_relaxed);
                if (g_debug_level >= 2) {
                    std::cerr << "[debug] sent msg to queue='" << queue_name << "' type=" << (int)u.type << " size=" << mlen << " attempt=" << attempt << "\n";
                }
                break;
            } else if (errno == EAGAIN) {
                if (attempt == g_send_retry_count) {
                    g_tx_send_errors.fetch_add(1, std::memory_order_relaxed);
                    if (g_debug_level >= 3) {
                        std::cerr << "[debug] mq_send failed after retries queue='" << queue_name << "' errno=" << errno << " len=" << mlen << "\n";
                    }
                    break; // give up this message; anti-entropy will resend what it carried
                }
                usleep(g_send_retry_delay_ms * 1000);
                ++attempt;
                continue;
            } else {
                g_tx_send_errors.fetch_add(1, std::memory_order_relaxed);
                if (g_debug_level >= 2) {
                    std::cerr << "[debug] mq_send immediate failure queue='" << queue_name << "' errno=" << errno << " len=" << mlen << "\n";
                }
                break;
            }
        }
    }
    mq_close(mq);
}

void send_updates_to_user(const std::string& user_id, std::vector<Update>& ops) {
    if (ops.empty()) return;
    if (g_network_mode) {
        // Over the link to that peer; with no such link (or a --serve relay in between),
        // over every link: the message names its addressee, so the others ignore it.
        std::lock_guard<std::mutex> lock(g_network_clients_mutex);
        std::shared_ptr<NetworkClient> target;
        for (const auto& c : g_network_clients) if (c->user_id == user_id) target = c;
        for (const auto& u : ops) {
            if (target) network_send_update(target->fd, u);
            else for (const auto& c : g_network_clients) network_send_update(c->fd, u);
        }
        return;
    }
    for (size_t i = 0; i < MAX_USERS; ++i) {
        auto &slot = g_registry->users[i];
        if (!slot_is_live_peer(slot)) continue;
        if (std::strncmp(slot.user_id, user_id.c_str(), USER_ID_MAX) != 0) continue;
        send_to_queue(slot.queue_name, ops, ops.size());
        return;
    }
    while (!ops.empty()) {                 // unknown recipient: tell everyone
        size_t sent = broadcast_updates_to_all(ops);
        if (sent == 0) break;
        ops.erase(ops.begin(), ops.begin() + (long)sent);
    }
}

// Send up to 'g_max_broadcast_per_cycle' operations (prefix of ops) to ALL recipients.
// Returns the number of ops sent (0..min(ops.size(), g_max_broadcast_per_cycle)).
size_t broadcast_updates_to_all(std::vector<Update>& ops) {
    if (ops.empty()) return 0;
    size_t to_send = ops.size();
    if (g_max_broadcast_per_cycle > 0 && to_send > (size_t)g_max_broadcast_per_cycle) {
        to_send = (size_t)g_max_broadcast_per_cycle;
    }
    if (g_network_mode) {
        // Straight to every peer over its own link (full mesh: nobody relays).
        std::lock_guard<std::mutex> lock(g_network_clients_mutex);
        for (size_t k = 0; k < to_send; ++k) {
            for (const auto& client : g_network_clients) network_send_update(client->fd, ops[k]);
            g_tx_sent_total.fetch_add(1, std::memory_order_relaxed);
        }
        return to_send;
    }
    // For fairness, send the same prefix [0..to_send) to every recipient in this cycle
    for (size_t i = 0; i < MAX_USERS; ++i) {
        auto &slot = g_registry->users[i];
        if (!slot_is_live_peer(slot)) continue;
        send_to_queue(slot.queue_name, ops, to_send);
    }
    return to_send;
}

} // namespace synctext
