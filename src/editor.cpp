/**
 * @file editor.cpp
 * @brief Core implementation of the SyncText Collaborative Editor.
 * @date November 11, 2025
 *
 * @details
 * This file contains the complete implementation for the SyncText application.
 * It manages peer discovery, real-time communication, conflict-free data
 * replication (CRDT), and file monitoring. The design prioritizes lock-free
 * mechanisms for high performance and responsiveness.
 *
 * ### Key Architectural Concepts:
 *
 * 1.  **Peer Discovery via Shared Memory:**
 *     A POSIX shared memory segment (`/synctext_registry`) acts as a discovery
 *     mechanism. Each editor instance registers its PID and a unique POSIX
 *     message queue name in a slot within this shared `Registry` struct.
 *     Registration and heartbeat updates are performed atomically using
 *     `std::atomic` and Compare-And-Swap (CAS) operations, avoiding the need
 *     for mutexes.
 *
 * 2.  **Asynchronous Communication with Message Queues:**
 *     Each peer creates a dedicated POSIX message queue to receive updates.
 *     When a local change is detected, the editor broadcasts it to all other
 *     active peers by sending a message to their respective queues. This is
 *     done non-blockingly, with a retry mechanism to handle full queues (`EAGAIN`).
 *
 * 3.  **CRDT for Conflict-Free Merging:**
 *     SyncText uses a Last-Writer-Wins (LWW) CRDT approach. Every document
 *     update is tagged with a high-precision nanosecond timestamp. When updates
 *     conflict, the one with the later timestamp is chosen. A user ID tie-breaker
 *     ensures deterministic resolution if timestamps are identical.
 *
 * 4.  **Structural-First Merge Strategy:**
 *     To maintain document integrity and prevent index corruption, updates are
 *     applied in a specific order:
 *     - Deletes are applied first, in descending line order.
 *     - Inserts are applied next, in ascending line order.
 *     - Inline (character-level) changes are applied last, rebased on the
 *       structurally modified document.
 *
 * 5.  **Efficient Change Detection:**
 *     File changes are monitored using `inotify` for near-instantaneous
 *     detection. A "settling" period is used to debounce events from editors
 *     that perform atomic saves (e.g., `rename`), which can otherwise appear
 *     as a mass-delete followed by a mass-insert. A fallback to periodic
 *     modification time polling is available if `inotify` is not supported.
 *
 * @note This implementation is self-contained within the `synctext` namespace
 *       and uses `static` for all internal functions and global variables to
 *       restrict their scope to this translation unit.
 */

#include "synctext.h"

// System includes needed for implementation
#include <iostream>
#include <fstream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cctype>
#include <vector>
#include <string>
#include <atomic>
#include <csignal>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <termios.h>
#include <mqueue.h>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <errno.h>
#include <spawn.h>
#include <signal.h>
#include <sys/eventfd.h>

using namespace synctext;

// ============================================================================
// SECTION 1: SHARED MEMORY REGISTRY - USER DISCOVERY & COORDINATION
// ============================================================================
/**
 * The registry provides lock-free user discovery via POSIX shared memory.
 * Each user atomically reserves a slot (CAS), publishes their message queue
 * name, and sends periodic heartbeats. This enables dynamic peer discovery
 * without centralized coordination.
 */

namespace synctext {

// ============================================================================
// :: Internal Globals & State
// ============================================================================

// --- Core State ---
static volatile std::sig_atomic_t g_stop = 0; ///< Global flag to signal graceful shutdown. Set by `handle_signal`.
static Registry* g_registry = nullptr;       ///< Pointer to the mmapped shared memory registry.
static int g_shm_fd = -1;                    ///< File descriptor for the shared memory object.
static std::string g_user_id;                ///< Identifier for the current user.
static std::string g_doc_path;               ///< Absolute or relative path to the document being edited.
static std::string g_queue_name;             ///< Name of this instance's POSIX message queue.
static mqd_t g_mq = (mqd_t)-1;               ///< Descriptor for our own message queue.
static int g_slot_index = -1;                ///< The index of our slot in the shared `g_registry`.

// --- UI & Interaction ---
static bool g_spinner = false;               ///< If true, an animated spinner is shown in the UI.
static bool g_hotkeys = false;               ///< If true, hotkey support for interaction is enabled.
static bool g_force_refresh = false;         ///< If true, forces a UI re-render in the current loop iteration.
static struct termios g_term_orig{};         ///< Stores original terminal settings to restore on exit.
static bool g_tty_ok = false;                ///< True if the terminal was successfully put into raw mode.
static bool g_interactive_login = false;     ///< If true, prompts user on how to handle an existing session for the same user ID.

// --- Timing & Tuning Configuration ---
static int g_debounce_ms = 0;                ///< Milliseconds to wait after a file change before processing, for basic debouncing.
static int g_settle_ms = 150;                ///< Milliseconds of quiet time required after a burst of `inotify` events before processing.
static int g_settle_overall_ms = 1500;       ///< Maximum time to wait for settling before proceeding regardless.
static int g_merge_batch_n = 1;              ///< Number of updates to accumulate before triggering a CRDT merge.
static int g_send_retry_count = 200;         ///< Number of retries for a blocked `mq_send` call.
static int g_send_retry_delay_ms = 5;        ///< Delay in milliseconds between `mq_send` retries.
static uint64_t g_stale_ns = 10ull * 1000000000ull; ///< Nanoseconds after which an inactive peer is considered stale. Default: 10s.
static uint64_t g_idle_refresh_sec = 5;      ///< Seconds between UI refreshes when idle and `g_spinner` is off.
static int g_merge_write_grace_ms = 300;     ///< Milliseconds to wait after a merge before writing to disk, to avoid fighting the user's editor.
static int g_mass_diff_recheck_ms = 150;     ///< Milliseconds to wait before re-reading a file if a suspicious mass change is detected.

// --- Heuristics & Advanced Tuning ---
static bool g_periodic_mtime = false;        ///< If true, periodically check file modification time even if `inotify` is active.
static int g_suspicious_min_lines = 10;      ///< Minimum number of lines that must change to trigger a "suspicious change" check.
static int g_suspicious_ratio = 90;          ///< Percentage of lines that must change to trigger a "suspicious change" check.
static int g_broadcast_batch_n = 1;          ///< Number of local updates to batch before broadcasting.
static int g_max_broadcast_per_cycle = 128;  ///< Maximum number of updates to broadcast in a single main loop iteration.
static int g_block_ops_mode = 0;             ///< Mode for sending multi-line changes as single "block" operations (0=off, 1=auto, 2=always).
static int g_block_ops_min_lines = 6;        ///< Minimum number of lines for a change to be considered for a block operation.

// --- Internal State Management ---
static size_t g_registry_size = sizeof(Registry); ///< The expected size of the shared memory registry.
static int g_is_creator = 0;                 ///< Flag indicating if this instance created the shared memory segment.
static std::atomic<uint64_t> g_last_local_change_ns{0}; ///< Timestamp of the last detected local file modification.
static std::atomic<uint64_t> g_last_merge_write_ns{0};  ///< Timestamp of the last time we wrote merged changes to disk.
static std::atomic<uint64_t> g_last_local_delete_ns{0}; ///< Timestamp of the last local structural delete operation.
static std::vector<uint64_t> g_local_line_ts; ///< Per-line LWW timestamps for the local document version.
static std::vector<std::string> g_pending_merge_lines; ///< Holds the merged document content before it's written to disk.
static uint64_t g_pending_merge_ns = 0;      ///< Timestamp when the pending merge was generated.

// Forward decl for env parsing used early
static bool parse_env_bool(const char* name, bool defVal);
static uint64_t parse_env_seconds(const char* name, uint64_t def_sec);
static int parse_env_millis(const char* name, int def_ms);

/**
 * @brief Get current time in nanoseconds since Unix epoch
 * @return Nanosecond timestamp (uint64_t)
 * 
 * Uses CLOCK_REALTIME for wall-clock time. Critical for LWW conflict resolution
 * - nanosecond precision prevents same-second ambiguity that caused merge bugs.
 */
static uint64_t now_ns() {
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
static void install_signal_handlers() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
}

/**
 * @brief Initialize or attach to shared memory registry
 * 
 * Creates /synctext_registry if it doesn't exist, or attaches to existing.
 * Uses shm_open() + mmap() for POSIX shared memory.
 * 
 * Flow:
 * 1. shm_open() with O_CREAT - get or create segment
 * 2. Check size; if too small, ftruncate() and mark as creator
 * 3. mmap() into process address space (MAP_SHARED)
 * 4. Initialize version=1 and max_users if we're creator
 * 
 * @note Exits on failure (critical subsystem)
 */
static void init_registry() {
    g_shm_fd = shm_open(REGISTRY_SHM_NAME, O_RDWR | O_CREAT, 0666);
    if (g_shm_fd < 0) {
        perror("shm_open");
        std::exit(1);
    }

    struct stat st{};
    if (fstat(g_shm_fd, &st) < 0) {
        perror("fstat");
        std::exit(1);
    }

    if ((size_t)st.st_size < g_registry_size) {
        if (ftruncate(g_shm_fd, g_registry_size) < 0) {
            perror("ftruncate");
            std::exit(1);
        }
        g_is_creator = 1;
    }

    void* mem = mmap(nullptr, g_registry_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (mem == MAP_FAILED) {
        perror("mmap");
        std::exit(1);
    }
    g_registry = reinterpret_cast<Registry*>(mem);

    // Lazy init if needed
    uint32_t expected = 1;
    if (!g_registry->version.compare_exchange_strong(expected, 1)) {
        // If not equal to 1, attempt to initialize from zero state
        if (g_registry->version.load() != 1) {
            // Zero out safely
            std::memset(mem, 0, g_registry_size);
            g_registry->max_users = MAX_USERS;
            g_registry->version.store(1);
        }
    } else {
        // already initialized
        g_registry->max_users = MAX_USERS;
    }
}

/**
 * @brief Update heartbeat timestamp for current user
 * 
 * Writes now_ns() to registry slot's last_heartbeat_ns field.
 * Called periodically (every ~1 second) to prove liveness.
 * Stale users (no heartbeat for >10 seconds) may be reclaimed.
 * 
 * @note No-op if not registered (g_slot_index < 0)
 */
static void heartbeat() {
    if (g_registry && g_slot_index >= 0) {
        g_registry->users[g_slot_index].last_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
    }
}

/**
 * @brief Deregister current user from registry
 * 
 * Atomically sets slot state to 0 (empty), releasing for other users.
 * Should be called during graceful shutdown.
 * 
 * @note No-op if not registered (g_slot_index < 0)
 */
static void deregister_user() {
    if (g_registry && g_slot_index >= 0) {
        g_registry->users[g_slot_index].active.store(0);
    }
}

/**
 * @brief Generate POSIX message queue name from user ID
 * @param user_id User identifier string
 * @return Queue name like "/synctext_user_alice"
 * 
 * Sanitizes user_id by replacing '/' with '_' (queue names must start with /).
 * Truncates to QUEUE_NAME_MAX if necessary.
 */
static std::string make_queue_name(const std::string& user_id) {
    std::string name = "/synctext_user_";
    for (char c : user_id) {
        if (c == '/') name.push_back('_');
        else name.push_back(c);
    }
    if (name.size() >= QUEUE_NAME_MAX) name.resize(QUEUE_NAME_MAX - 1);
    return name;
}

// Message queue attributes
static int g_mq_maxmsg = 256; // default max outstanding messages per queue (larger to reduce EAGAIN)

static void open_message_queue(const std::string& qname) {
    // Best-effort: remove any stale queue so attributes take effect on recreation
    mq_unlink(qname.c_str());
    struct mq_attr attr{};
    attr.mq_flags = 0;
    attr.mq_maxmsg = g_mq_maxmsg;
    attr.mq_msgsize = MQ_MSGSIZE;
    attr.mq_curmsgs = 0;
    mqd_t mq = mq_open(qname.c_str(), O_CREAT | O_RDONLY | O_NONBLOCK, 0666, &attr);
    if (mq == (mqd_t)-1) {
        mq = mq_open(qname.c_str(), O_CREAT | O_RDONLY | O_NONBLOCK, 0666, nullptr);
    }
    if (mq == (mqd_t)-1) {
        perror("mq_open recv");
        return;
    }
    g_mq = mq;
}

static bool wait_pid_gone(pid_t pid, int timeout_ms) {
    const int step = 100; // ms
    int waited = 0;
    while (waited < timeout_ms) {
        // kill(pid, 0) returns 0 if a process with pid exists and we have permission to send signals to it
        // or -1 with errno==EPERM if it exists but no permission; ESRCH if it doesn't exist
        if (kill(pid, 0) != 0 && errno == ESRCH) return true; // gone
        usleep(step * 1000);
        waited += step;
    }
    return false;
}

static void attempt_force_reclaim_user(const std::string& user_id) {
    int grace_ms = parse_env_millis("SYNCTEXT_RECLAIM_GRACE_MS", 1500);
    bool force_kill = parse_env_bool("SYNCTEXT_FORCE_KILL", false);
    for (size_t i = 0; i < MAX_USERS; ++i) {
        auto &slot = g_registry->users[i];
        if (slot.active.load(std::memory_order_relaxed) != 2) continue;
        if (std::strncmp(slot.user_id, user_id.c_str(), USER_ID_MAX) != 0) continue;
        pid_t pid = slot.pid;
        bool alive = (kill(pid, 0) == 0) || (errno == EPERM);
        if (!alive) {
            uint32_t expected = 2; slot.active.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
            continue;
        }
        // Politely ask old process to exit
        kill(pid, SIGTERM);
        if (!wait_pid_gone(pid, grace_ms) && force_kill) {
            kill(pid, SIGKILL);
            // small wait to let it be reaped
            wait_pid_gone(pid, 300);
        }
        // Clear registry slot best-effort
        uint32_t expected = 2;
        slot.active.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
    }
}

// ============================================================================
// SECTION 3: MESSAGE SERIALIZATION - BINARY PROTOCOL FOR UPDATES
// ============================================================================

static bool serialize_update(const Update& u, char* out, size_t cap, size_t& out_len) {
    const std::string& uid = u.user_id;
    const std::string& oldc = u.old_content;
    const std::string& newc = u.new_content;
    size_t need = sizeof(WireHdr) + uid.size() + oldc.size() + newc.size();
    if (need > cap) return false;
    // Use version 2 to allow extended operation types
    WireHdr h{2,(uint8_t)u.type,(uint32_t)u.line_no,(uint32_t)u.col_start,(uint32_t)u.col_end,u.timestamp_ns,
              (uint16_t)uid.size(),(uint32_t)oldc.size(),(uint32_t)newc.size()};
    std::memcpy(out,&h,sizeof(h));
    size_t off = sizeof(h);
    std::memcpy(out+off, uid.data(), uid.size()); off += uid.size();
    std::memcpy(out+off, oldc.data(), oldc.size()); off += oldc.size();
    std::memcpy(out+off, newc.data(), newc.size()); off += newc.size();
    out_len = off;
    return true;
}

static bool deserialize_update(const char* in, size_t len, Update& u) {
    if (len < sizeof(WireHdr)) return false;
    WireHdr h{}; std::memcpy(&h, in, sizeof(h));
    if (h.ver != 1 && h.ver != 2) return false;
    size_t off = sizeof(h);
    if (off + h.user_len + h.old_len + h.new_len > len) return false;
    std::string uid(in+off, h.user_len); off += h.user_len;
    std::string oldc(in+off, h.old_len); off += h.old_len;
    std::string newc(in+off, h.new_len); off += h.new_len;
    Update out{};
    out.type = (Update::Type)h.type;
    out.line_no = h.line_no;
    out.col_start = h.col_start;
    out.col_end = h.col_end;
    out.timestamp_ns = h.ts_ns;
    out.user_id = std::move(uid);
    out.old_content = std::move(oldc);
    out.new_content = std::move(newc);
    u = std::move(out);
    return true;
}

// Listener thread counters
static std::thread g_listener_thread;
static std::atomic<uint64_t> g_rx_total{0};
static std::atomic<uint64_t> g_rx_dropped{0};
static std::atomic<uint64_t> g_tx_sent_total{0};
static std::atomic<uint64_t> g_tx_send_errors{0};
static std::atomic<uint64_t> g_tx_open_errors{0}; // failed mq_open for recipient
static std::atomic<uint64_t> g_rx_receive_errors{0}; // unexpected mq_receive errors
static bool g_debug_msg = false; // legacy flag (SYNCTEXT_DEBUG_MSG=1) mapped to level>=2
static int  g_debug_level = 0;   // 0=off,1=basic,2=verbose,3=trace

// Lock-free SPSC ring for received updates (listener -> main thread)
static std::vector<Update> g_rx_ring; // sized at startup (using RX_RING_CAP from header)
static std::atomic<size_t> g_rx_head{0}; // producer index (listener)
static std::atomic<size_t> g_rx_tail{0}; // consumer index (main thread)
static std::vector<Update> g_rx_local;   // accumulated received updates for later processing (Part 3)

static inline bool rx_ring_push(Update&& u) {
    size_t head = g_rx_head.load(std::memory_order_relaxed);
    size_t tail = g_rx_tail.load(std::memory_order_acquire);
    if (head - tail >= RX_RING_CAP) return false; // full
    g_rx_ring[head % RX_RING_CAP] = std::move(u);
    g_rx_head.store(head + 1, std::memory_order_release);
    return true;
}

static inline bool rx_ring_pop(Update& out) {
    size_t tail = g_rx_tail.load(std::memory_order_relaxed);
    size_t head = g_rx_head.load(std::memory_order_acquire);
    if (tail == head) return false; // empty
    out = std::move(g_rx_ring[tail % RX_RING_CAP]);
    g_rx_tail.store(tail + 1, std::memory_order_release);
    return true;
}

// Wake-up fd to nudge main loop immediately on inbound messages (no locks)
static int g_wake_fd = -1;
static void init_wake_fd() {
    if (g_wake_fd >= 0) return;
    g_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}
static inline void wake_main_loop() {
    if (g_wake_fd < 0) return;
    uint64_t one = 1;
    ssize_t r = write(g_wake_fd, &one, sizeof(one));
    (void)r;
}
static inline void drain_wake_fd() {
    if (g_wake_fd < 0) return;
    uint64_t v;
    for (;;) {
        ssize_t n = read(g_wake_fd, &v, sizeof(v));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }
        if (n == 0) break;
    }
}
static void listener_loop() {
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
        std::vector<std::string> prev_lines; // Store previous lines for comparison
        while (!g_stop) {
            // Receive messages from the message queue
        ssize_t n = mq_receive(g_mq, buf.data(), buf.size(), nullptr);
        if (n >= 0) {
            Update u{};
            if (deserialize_update(buf.data(), (size_t)n, u)) {
                g_rx_total.fetch_add(1, std::memory_order_relaxed);
                if (!rx_ring_push(Update(u))) {
                    g_rx_dropped.fetch_add(1, std::memory_order_relaxed);
                }
                wake_main_loop();
                if (g_debug_level >= 2) {
                    std::cerr << "[debug] received msg from user=" << u.user_id << " line=" << u.line_no << " type=" << (int)u.type << " size=" << n << "\n";
                }
            } else {
                g_rx_dropped.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }
        if (errno == EAGAIN) {
            usleep(50 * 1000);
            continue;
        }
        if (errno == EINTR) continue;
        // Unexpected receive error
        g_rx_receive_errors.fetch_add(1, std::memory_order_relaxed);
    if (g_debug_level >= 3) {
            std::cerr << "[debug] mq_receive error errno=" << errno << "\n";
        }
        usleep(50 * 1000);
    }
}

// Outgoing accumulation buffer
static std::vector<Update> g_tx_pending;
static std::vector<Update> g_local_since_merge; // local ops since last merge

// Send up to 'g_max_broadcast_per_cycle' operations (prefix of ops) to ALL recipients.
// Returns the number of ops sent (0..min(ops.size(), g_max_broadcast_per_cycle)).
static size_t broadcast_updates_to_all(std::vector<Update>& ops) {
    if (ops.empty()) return 0;
    size_t to_send = ops.size();
    if (g_max_broadcast_per_cycle > 0 && to_send > (size_t)g_max_broadcast_per_cycle) {
        to_send = (size_t)g_max_broadcast_per_cycle;
    }
    // For fairness, send the same prefix [0..to_send) to every recipient in this cycle
    for (size_t i = 0; i < MAX_USERS; ++i) {
        auto &slot = g_registry->users[i];
        if (slot.active.load(std::memory_order_relaxed) != 2) continue;
        if (std::strncmp(slot.user_id, g_user_id.c_str(), USER_ID_MAX) == 0) continue;
        bool alive = (kill(slot.pid, 0) == 0) || (errno == EPERM);
        uint64_t hb = slot.last_heartbeat_ns.load(std::memory_order_relaxed);
        bool fresh = (now_ns() - hb) <= g_stale_ns;
        if (!(alive && fresh)) continue;
        mqd_t mq = mq_open(slot.queue_name, O_WRONLY | O_NONBLOCK);
        if (mq == (mqd_t)-1) {
            g_tx_open_errors.fetch_add(1, std::memory_order_relaxed);
            if (g_debug_level >= 3) {
                std::cerr << "[debug] mq_open failed for recipient queue='" << slot.queue_name << "' errno=" << errno << "\n";
            }
            continue;
        }
        char msg[MQ_MSGSIZE]; size_t mlen=0;
        for (size_t k = 0; k < to_send; ++k) {
            const auto& u = ops[k];
            if (!serialize_update(u, msg, sizeof(msg), mlen)) {
                if (g_debug_level >= 3) {
                    std::cerr << "[debug] serialize FAILED for line=" << u.line_no << " type=" << (int)u.type << " content_len=" << u.new_content.size() << "\n";
                }
                continue;
            }
            int attempt = 0;
            while (attempt <= g_send_retry_count) {
                if (mq_send(mq, msg, mlen, 0) == 0) {
                    g_tx_sent_total.fetch_add(1, std::memory_order_relaxed);
                    if (g_debug_level >= 2) {
                        std::cerr << "[debug] sent msg to queue='" << slot.queue_name << "' line=" << u.line_no << " type=" << (int)u.type << " size=" << mlen << " attempt=" << attempt << "\n";
                    }
                    break;
                } else if (errno == EAGAIN) {
                    if (attempt == g_send_retry_count) {
                        g_tx_send_errors.fetch_add(1, std::memory_order_relaxed);
                        if (g_debug_level >= 3) {
                            std::cerr << "[debug] mq_send failed after retries queue='" << slot.queue_name << "' errno=" << errno << " len=" << mlen << "\n";
                        }
                        break; // give up this message
                    }
                    usleep(g_send_retry_delay_ms * 1000);
                    ++attempt;
                    continue;
                } else {
                    g_tx_send_errors.fetch_add(1, std::memory_order_relaxed);
                    if (g_debug_level >= 2) {
                        std::cerr << "[debug] mq_send immediate failure queue='" << slot.queue_name << "' errno=" << errno << " len=" << mlen << "\n";
                    }
                    break;
                }
            }
        }
        mq_close(mq);
    }
    return to_send;
}

// ============================================================================
// SECTION 4: USER REGISTRATION - SLOT RESERVATION & DISCOVERY
// ============================================================================

static RegisterResult register_user_try(const std::string& user_id, bool force_reclaim) {
    // Enforce unique user_id if an active live process already registered
    for (size_t i = 0; i < MAX_USERS; ++i) {
        auto &slot = g_registry->users[i];
        if (slot.active.load(std::memory_order_relaxed) == 2 && std::strncmp(slot.user_id, user_id.c_str(), USER_ID_MAX) == 0) {
            bool alive = (kill(slot.pid, 0) == 0) || (errno == EPERM);
            uint64_t hb = slot.last_heartbeat_ns.load(std::memory_order_relaxed);
            bool fresh = (now_ns() - hb) <= g_stale_ns;
            if (alive && fresh) {
                if (!force_reclaim) {
                    return RegisterResult::DuplicateActive;
                }
                // Force reclaim requested: try to terminate old process then clear slot
                attempt_force_reclaim_user(user_id);
            } else {
                // Clear stale if possible
                uint32_t expected = 2;
                slot.active.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
            }
        }
    }

    // Try to find a free slot via CAS on active flag
    for (size_t i = 0; i < MAX_USERS; ++i) {
        uint32_t expected = 0;
        if (g_registry->users[i].active.compare_exchange_strong(expected, 1)) {
            // Reserve; fill fields
            g_slot_index = (int)i;
            g_registry->users[i].pid = getpid();
            std::snprintf(g_registry->users[i].user_id, USER_ID_MAX, "%s", user_id.c_str());
            std::snprintf(g_registry->users[i].queue_name, QUEUE_NAME_MAX, "%s", g_queue_name.c_str());
            g_registry->users[i].last_heartbeat_ns.store(now_ns());
            // Publish as active
            g_registry->users[i].active.store(2);
            return RegisterResult::Ok;
        }
    }
    return RegisterResult::NoSlots;
}

static pid_t find_active_pid_for_user(const std::string& user_id, bool& outFresh) {
    outFresh = false;
    for (size_t i = 0; i < MAX_USERS; ++i) {
        auto &slot = g_registry->users[i];
        if (slot.active.load(std::memory_order_relaxed) == 2 && std::strncmp(slot.user_id, user_id.c_str(), USER_ID_MAX) == 0) {
            bool alive = (kill(slot.pid, 0) == 0) || (errno == EPERM);
            uint64_t hb = slot.last_heartbeat_ns.load(std::memory_order_relaxed);
            bool fresh = (now_ns() - hb) <= g_stale_ns;
            if (alive && fresh) {
                outFresh = true;
                return slot.pid;
            }
        }
    }
    return -1;
}

static std::vector<std::string> read_file_lines(const std::string& path) {
    std::vector<std::string> lines;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return lines;
    char* buf = nullptr;
    size_t n = 0;
    ssize_t len = 0;
#if defined(_GNU_SOURCE) || defined(__USE_GNU)
    while ((len = getline(&buf, &n, f)) != -1) {
        if (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
            // strip trailing newline or CR
            while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) { --len; }
        }
        lines.emplace_back(std::string(buf, (size_t)len));
    }
#else
    {
        char local[8192];
        std::string accum;
        while (std::fgets(local, sizeof(local), f)) {
            std::string s(local);
            if (!s.empty() && (s.back()=='\n' || s.back()=='\r')) {
                while (!s.empty() && (s.back()=='\n' || s.back()=='\r')) s.pop_back();
                lines.push_back(s);
            } else {
                // handle extremely long lines without newline
                lines.push_back(s);
            }
        }
    }
#endif
    if (buf) free(buf);
    std::fclose(f);
    return lines;
}

// Helper to compare timespec with nanosecond precision
static inline bool timespec_equal(const struct timespec& a, const struct timespec& b) {
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

// Read lines with basic stabilization: ensure file's mtime and size are
// unchanged before and after the read, or give up after max_wait_ms.
static std::vector<std::string> read_file_lines_stable(const std::string& path, int max_wait_ms) {
    struct stat st1{}, st2{};
    uint64_t start = now_ns();
    const int step_ms = 50;
    while (((now_ns() - start) / 1000000ull) < (uint64_t)max_wait_ms) {
        if (stat(path.c_str(), &st1) != 0) {
            // File temporarily missing (rename-in-progress) — wait
            usleep(step_ms * 1000);
            continue;
        }
        auto lines = read_file_lines(path);
        if (stat(path.c_str(), &st2) == 0 && st1.st_mtime == st2.st_mtime && st1.st_size == st2.st_size) {
            // If size reports >0 but we got zero lines, try again once more (suspicious mid-write)
            if (st2.st_size > 0 && lines.empty()) {
                usleep(step_ms * 1000);
                continue;
            }
            return lines;
        }
        usleep(step_ms * 1000);
    }
    // Final best-effort
    return read_file_lines(path);
}

static void write_initial_doc(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        perror("fopen initial doc");
        std::exit(3);
    }
    const char* content =
        "Hello World\n"
        "This is a collaborative editor\n"
        "Welcome to SyncText\n"
        "Edit this document and see real-time updates\n";
    std::fwrite(content, 1, std::strlen(content), f);
    std::fclose(f);
}

static std::string type_to_string(Update::Type t) {
    switch (t) {
        case Update::Type::Insert: return "insert";
        case Update::Type::Delete: return "delete";
        case Update::Type::Replace: return "replace";
        case Update::Type::LineInsert: return "line-insert";
        case Update::Type::LineDelete: return "line-delete";
        case Update::Type::BlockInsert: return "block-insert";
        case Update::Type::BlockDelete: return "block-delete";
    }
    return "unknown";
}

// Decide if a block op is allowed based on mode, size, and MQ payload capacity
static bool allow_block_op(size_t num_lines, size_t payload_len) {
    if (g_block_ops_mode == 0) return false; // never
    if (g_block_ops_mode == 1 && num_lines < (size_t)g_block_ops_min_lines) return false; // below threshold
    // Bound by MQ message capacity: sizeof(WireHdr) + user_id + payload must fit
    size_t header_overhead = sizeof(WireHdr) + g_user_id.size();
    size_t total = header_overhead + payload_len;
    if (total > (size_t)MQ_MSGSIZE) return false;
    return true;
}

static void detect_line_change(size_t i, const std::string& old_line, const std::string& new_line, std::vector<Update>& out) {
    if (old_line == new_line) return;
    size_t lcp = 0;
    while (lcp < old_line.size() && lcp < new_line.size() && old_line[lcp] == new_line[lcp]) ++lcp;
    size_t lcs = 0;
    while (lcs < old_line.size()-lcp && lcs < new_line.size()-lcp &&
           old_line[old_line.size()-1 - lcs] == new_line[new_line.size()-1 - lcs]) ++lcs;

    std::string old_mid = old_line.substr(lcp, old_line.size() - lcp - lcs);
    std::string new_mid = new_line.substr(lcp, new_line.size() - lcp - lcs);

    Update u{};
    u.line_no = i;
    u.col_start = lcp;
    if (!old_mid.empty()) u.col_end = lcp + old_mid.size();
    else u.col_end = lcp; // insert
    u.old_content = old_mid;
    u.new_content = new_mid;
    u.timestamp_ns = now_ns();
    u.user_id = g_user_id;
    if (old_mid.empty() && !new_mid.empty()) u.type = Update::Type::Insert;
    else if (!old_mid.empty() && new_mid.empty()) u.type = Update::Type::Delete;
    else u.type = Update::Type::Replace;
    out.push_back(std::move(u));
}

// ============================================================================
// SECTION 3: DIFF ALGORITHM - LINE & CHARACTER LEVEL CHANGE DETECTION
// ============================================================================
/**
 * The diff engine computes fine-grained differences between document snapshots.
 * It generates structured Update objects representing:
 * - Structural changes (line insert/delete)
 * - Inline edits (character-level insert/delete/replace)
 * - Block operations (multi-line batches for performance)
 * 
 * Strategy:
 * 1. Detect simple patterns (single line add/remove, common prefix/suffix)
 * 2. For complex changes, fall back to full line-by-line comparison
 * 3. Apply block operation heuristics when enabled (SYNCTEXT_BLOCK_OPS_MODE)
 */

/**
 * @brief Compute diff between two document snapshots
 * @param old_lines Previous document state (vector of line strings)
 * @param new_lines Current document state (vector of line strings)
 * @return Vector of Update objects representing all changes
 * 
 * Algorithm optimizations:
 * - Fast path for single-line insert/delete (common case)
 * - Common prefix/suffix trimming to avoid redundant comparisons
 * - Block operation batching when multiple consecutive lines change
 * - Character-level diff for modified lines (not just line replacement)
 * 
 * Example:
 *   Old: ["hello", "world"]
 *   New: ["hello", "beautiful", "world"]
 *   Result: [LineInsert(line=1, payload="beautiful")]
 */
static std::vector<Update> diff_documents(const std::vector<std::string>& old_lines,
                                          const std::vector<std::string>& new_lines) {
    std::vector<Update> updates;
    // Detect single line insertion anywhere (generalized), including split-line + typed text
    if (new_lines.size() == old_lines.size() + 1) {
        size_t prefix = 0; while (prefix < old_lines.size() && old_lines[prefix] == new_lines[prefix]) ++prefix;
        size_t suffix = 0; while (suffix < old_lines.size()-prefix && old_lines[old_lines.size()-1 - suffix] == new_lines[new_lines.size()-1 - suffix]) ++suffix;
        if (prefix + suffix == old_lines.size()) {
            size_t i = prefix; // insertion occurs around this index
            // Insertion at end
            if (i == old_lines.size()) {
                for (size_t k = 0; k < old_lines.size(); ++k) detect_line_change(k, old_lines[k], new_lines[k], updates);
                Update ins{}; ins.type = Update::Type::LineInsert; ins.line_no = i; ins.col_start=0; ins.col_end=0; ins.old_content=""; ins.new_content=new_lines[i]; ins.timestamp_ns=now_ns(); ins.user_id=g_user_id; updates.push_back(std::move(ins));
                return updates;
            }
            bool pure_between = (old_lines[i] == new_lines[i+1]);
            if (pure_between) {
                // Pure insertion between unchanged lines
                Update ins{}; ins.type = Update::Type::LineInsert; ins.line_no = i; ins.col_start=0; ins.col_end=0; ins.old_content=""; ins.new_content=new_lines[i]; ins.timestamp_ns=now_ns(); ins.user_id=g_user_id; updates.push_back(std::move(ins));
                for (size_t k = 0; k < i; ++k) detect_line_change(k, old_lines[k], new_lines[k], updates);
                for (size_t k = i; k < old_lines.size(); ++k) detect_line_change(k+1, old_lines[k], new_lines[k+1], updates);
                return updates;
            } else {
                // Likely a split of old_lines[i] -> new_lines[i] and inserted new_lines[i+1]
                detect_line_change(i, old_lines[i], new_lines[i], updates);
                Update ins{}; ins.type = Update::Type::LineInsert; ins.line_no = i+1; ins.col_start=0; ins.col_end=0; ins.old_content=""; ins.new_content=new_lines[i+1]; ins.timestamp_ns=now_ns(); ins.user_id=g_user_id; updates.push_back(std::move(ins));
                for (size_t k = 0; k < i; ++k) detect_line_change(k, old_lines[k], new_lines[k], updates);
                for (size_t k = i+1; k < old_lines.size(); ++k) detect_line_change(k+1, old_lines[k], new_lines[k+1], updates);
                return updates;
            }
        }
    }
    // Detect single line deletion
    if (old_lines.size() == new_lines.size() + 1) {
        size_t prefix = 0; while (prefix < new_lines.size() && old_lines[prefix] == new_lines[prefix]) ++prefix;
        size_t suffix = 0; while (suffix < new_lines.size()-prefix && old_lines[old_lines.size()-1 - suffix] == new_lines[new_lines.size()-1 - suffix]) ++suffix;
        if (prefix + suffix == new_lines.size()) {
            Update u{};
            u.type = Update::Type::LineDelete;
            u.line_no = prefix;
            u.col_start = 0; u.col_end = 0;
            u.old_content = old_lines[prefix];
            u.new_content = "";
            u.timestamp_ns = now_ns();
            u.user_id = g_user_id;
            updates.push_back(std::move(u));
            return updates;
        }
    }
    // Generalized block diff for multi-line insert/delete/replace
    if (old_lines.size() == new_lines.size()) {
        // No structural change in line count; compute per-line inline diffs
        for (size_t i = 0; i < old_lines.size(); ++i) {
            detect_line_change(i, old_lines[i], new_lines[i], updates);
        }
        return updates;
    } else {
        // Structural change in line count: compute common prefix/suffix and emit block ops
        const size_t oldN = old_lines.size();
        const size_t newN = new_lines.size();
        size_t p = 0;
        while (p < oldN && p < newN && old_lines[p] == new_lines[p]) ++p;
        size_t s = 0;
        while (s < (oldN - p) && s < (newN - p) && old_lines[oldN - 1 - s] == new_lines[newN - 1 - s]) ++s;

        size_t old_mid = oldN - p - s;
        size_t new_mid = newN - p - s;

        // Emit inline diffs for the preserved prefix
        for (size_t i = 0; i < p; ++i) detect_line_change(i, old_lines[i], new_lines[i], updates);

        if (old_mid == 0 && new_mid > 0) {
            // Pure insertion block at position p
            if (new_mid == 1) {
                Update u{}; u.type = Update::Type::LineInsert; u.line_no = p; u.col_start=0; u.col_end=0; u.old_content=""; u.new_content=new_lines[p]; u.timestamp_ns=now_ns(); u.user_id=g_user_id; updates.push_back(std::move(u));
            } else {
                std::string payload;
                for (size_t j = 0; j < new_mid; ++j) {
                    if (j) payload.push_back('\n');
                    payload += new_lines[p + j];
                }
                if (allow_block_op(new_mid, payload.size())) {
                    Update u{}; u.type = Update::Type::BlockInsert; u.line_no = p; u.col_start=0; u.col_end=0; u.old_content=""; u.new_content=std::move(payload); u.timestamp_ns=now_ns(); u.user_id=g_user_id; updates.push_back(std::move(u));
                } else {
                    // fallback to per-line inserts
                    for (size_t j = 0; j < new_mid; ++j) {
                        Update u{}; u.type = Update::Type::LineInsert; u.line_no = p + j; u.col_start=0; u.col_end=0; u.old_content=""; u.new_content=new_lines[p + j]; u.timestamp_ns=now_ns(); u.user_id=g_user_id; updates.push_back(std::move(u));
                    }
                }
            }
        } else if (old_mid > 0 && new_mid == 0) {
            // Pure deletion block starting at position p
            if (old_mid == 1) {
                Update u{}; u.type = Update::Type::LineDelete; u.line_no = p; u.col_start=0; u.col_end=0; u.old_content=old_lines[p]; u.new_content=""; u.timestamp_ns=now_ns(); u.user_id=g_user_id; updates.push_back(std::move(u));
            } else {
                std::string payload;
                for (size_t j = 0; j < old_mid; ++j) {
                    if (j) payload.push_back('\n');
                    payload += old_lines[p + j];
                }
                if (allow_block_op(old_mid, payload.size())) {
                    Update u{}; u.type = Update::Type::BlockDelete; u.line_no = p; u.col_start=0; u.col_end=0; u.old_content=std::move(payload); u.new_content=""; u.timestamp_ns=now_ns(); u.user_id=g_user_id; updates.push_back(std::move(u));
                } else {
                    for (size_t j = 0; j < old_mid; ++j) {
                        Update u{}; u.type = Update::Type::LineDelete; u.line_no = p + j; u.col_start=0; u.col_end=0; u.old_content=old_lines[p + j]; u.new_content=""; u.timestamp_ns=now_ns(); u.user_id=g_user_id; updates.push_back(std::move(u));
                    }
                }
            }
        } else {
            // Replacement block: delete old block, insert new block
            if (old_mid == 1) {
                Update u{}; u.type = Update::Type::LineDelete; u.line_no = p; u.col_start=0; u.col_end=0; u.old_content=old_lines[p]; u.new_content=""; u.timestamp_ns=now_ns(); u.user_id=g_user_id; updates.push_back(std::move(u));
            } else if (old_mid > 1) {
                std::string payload;
                for (size_t j = 0; j < old_mid; ++j) {
                    if (j) payload.push_back('\n');
                    payload += old_lines[p + j];
                }
                if (allow_block_op(old_mid, payload.size())) {
                    Update u{}; u.type = Update::Type::BlockDelete; u.line_no = p; u.col_start=0; u.col_end=0; u.old_content=std::move(payload); u.new_content=""; u.timestamp_ns=now_ns(); u.user_id=g_user_id; updates.push_back(std::move(u));
                } else {
                    for (size_t j = 0; j < old_mid; ++j) {
                        Update u{}; u.type = Update::Type::LineDelete; u.line_no = p + j; u.col_start=0; u.col_end=0; u.old_content=old_lines[p + j]; u.new_content=""; u.timestamp_ns=now_ns(); u.user_id=g_user_id; updates.push_back(std::move(u));
                    }
                }
            }

            if (new_mid == 1) {
                Update u{}; u.type = Update::Type::LineInsert; u.line_no = p; u.col_start=0; u.col_end=0; u.old_content=""; u.new_content=new_lines[p]; u.timestamp_ns=now_ns(); u.user_id=g_user_id; updates.push_back(std::move(u));
            } else if (new_mid > 1) {
                std::string payload2;
                for (size_t j = 0; j < new_mid; ++j) {
                    if (j) payload2.push_back('\n');
                    payload2 += new_lines[p + j];
                }
                if (allow_block_op(new_mid, payload2.size())) {
                    Update v{}; v.type = Update::Type::BlockInsert; v.line_no = p; v.col_start=0; v.col_end=0; v.old_content=""; v.new_content=std::move(payload2); v.timestamp_ns=now_ns(); v.user_id=g_user_id; updates.push_back(std::move(v));
                } else {
                    // fallback to per-line inserts for replacement
                    for (size_t j = 0; j < new_mid; ++j) {
                        Update v{}; v.type = Update::Type::LineInsert; v.line_no = p + j; v.col_start=0; v.col_end=0; v.old_content=""; v.new_content=new_lines[p + j]; v.timestamp_ns=now_ns(); v.user_id=g_user_id; updates.push_back(std::move(v));
                    }
                }
            }
        }

        // Emit inline diffs for the preserved suffix
        for (size_t i = 0; i < s; ++i) {
            size_t old_idx = oldN - s + i;
            size_t new_idx = newN - s + i;
            detect_line_change(new_idx, old_lines[old_idx], new_lines[new_idx], updates);
        }
        // Clean-up pass: suppress spurious empty line inserts immediately following deletes when total line count decreased
        if (newN < oldN) {
            std::vector<Update> cleaned; cleaned.reserve(updates.size());
            for (size_t ui = 0; ui < updates.size(); ++ui) {
                const Update &u = updates[ui];
                bool drop = false;
                if (u.type == Update::Type::LineInsert && u.new_content.empty()) {
                    // If previous update was a delete at the same line (after shifting) from same user, treat as artifact
                    if (ui > 0) {
                        const Update &prev = updates[ui-1];
                        if ((prev.type == Update::Type::LineDelete || prev.type == Update::Type::BlockDelete) && prev.line_no == u.line_no) {
                            drop = true;
                        }
                    }
                }
                if (!drop) cleaned.push_back(u);
            }
            updates.swap(cleaned);
        }
        return updates;
    }
}

// Count lines represented by a block op (BlockInsert uses new_content, BlockDelete uses old_content)
static size_t block_line_count(const Update& u) {
    const std::string* src = nullptr;
    if (u.type == Update::Type::BlockInsert) src = &u.new_content;
    else if (u.type == Update::Type::BlockDelete) src = &u.old_content;
    if (!src || src->empty()) return 0;
    size_t c = 1; for (char ch : *src) if (ch == '\n') ++c; return c;
}

// Update per-line timestamps to now for lines modified by local updates
static void update_local_line_timestamps_after(const std::vector<Update>& updates) {
    uint64_t nowt = now_ns();
    for (const auto &u : updates) {
        switch (u.type) {
            case Update::Type::Insert:
            case Update::Type::Delete:
            case Update::Type::Replace:
                if (u.line_no < g_local_line_ts.size()) g_local_line_ts[u.line_no] = nowt;
                break;
            case Update::Type::LineInsert: {
                size_t pos = std::min(u.line_no, g_local_line_ts.size());
                g_local_line_ts.insert(g_local_line_ts.begin()+ (long)pos, nowt);
                break;
            }
            case Update::Type::LineDelete: {
                if (u.line_no < g_local_line_ts.size()) {
                    g_local_line_ts.erase(g_local_line_ts.begin()+ (long)u.line_no);
                }
                break;
            }
            case Update::Type::BlockInsert: {
                size_t count = block_line_count(u);
                size_t pos = std::min(u.line_no, g_local_line_ts.size());
                for (size_t i=0;i<count;++i) g_local_line_ts.insert(g_local_line_ts.begin()+ (long)pos + (long)i, nowt);
                break;
            }
            case Update::Type::BlockDelete: {
                size_t count = block_line_count(u);
                if (count && u.line_no < g_local_line_ts.size()) {
                    size_t end = std::min(u.line_no + count, g_local_line_ts.size());
                    g_local_line_ts.erase(g_local_line_ts.begin()+ (long)u.line_no, g_local_line_ts.begin()+ (long)end);
                }
                break;
            }
        }
    }
}

static std::string time_str_from(std::time_t t) {
    char buf[64];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    return buf;
}

// --- CRDT Merge helpers (LWW with user-id tiebreak) ---
static inline bool is_structural(const Update& u) {
    return u.type == Update::Type::LineInsert || u.type == Update::Type::LineDelete ||
           u.type == Update::Type::BlockInsert || u.type == Update::Type::BlockDelete;
}
static inline bool ranges_overlap(const Update& a, const Update& b) {
    if (is_structural(a) || is_structural(b)) {
        // Structural ops: treat same line or adjacent delete/delete & insert collisions as conflict to prevent duplication.
        if (a.line_no == b.line_no) return true;
        // For block ops, consider range spans
        auto span = [](const Update& u)->std::pair<size_t,size_t>{
            if (u.type == Update::Type::BlockInsert || u.type == Update::Type::BlockDelete) {
                // Parse new_content (for BlockInsert) or old_content (for BlockDelete) as lines separated by '\n'
                size_t count = 0;
                const std::string& txt = (u.type == Update::Type::BlockInsert) ? u.new_content : u.old_content;
                if (!txt.empty()) {
                    count = 1; for (char c : txt) if (c == '\n') ++count;
                }
                return {u.line_no, u.line_no + (count ? count-1 : 0)}; // inclusive end
            }
            return {u.line_no, u.line_no};
        };
        auto [a0,a1] = span(a);
        auto [b0,b1] = span(b);
    bool line_conflict = !(a1 < b0 || b1 < a0);
    if (line_conflict) return true;
        // If one deletes a line and another inserts immediately before/after, indexes will shift; consider conflict.
        bool aDel = a.type == Update::Type::LineDelete;
        bool bDel = b.type == Update::Type::LineDelete;
        bool aIns = a.type == Update::Type::LineInsert;
        bool bIns = b.type == Update::Type::LineInsert;
        // Extend to block ops: treat block boundary adjacency as conflict
        bool aBlockDel = a.type == Update::Type::BlockDelete;
        bool bBlockDel = b.type == Update::Type::BlockDelete;
        bool aBlockIns = a.type == Update::Type::BlockInsert;
        bool bBlockIns = b.type == Update::Type::BlockInsert;
        // insert at i vs delete at i-1 or i treated as overlap to simplify resolution
        if (aIns && bDel && (a.line_no == b.line_no || (b.line_no + 1 == a.line_no))) return true;
        if (bIns && aDel && (b.line_no == a.line_no || (a.line_no + 1 == b.line_no))) return true;
    // consecutive deletes collapsing region; treat as conflict if adjacent (different users only)
    if (aDel && bDel && (a.line_no + 1 == b.line_no || b.line_no + 1 == a.line_no) && a.user_id != b.user_id) return true;
        // block/line boundary adjacency
        if (aBlockIns || aBlockDel || bBlockIns || bBlockDel) {
            if ((std::max(a0,a1)+1 == std::min(b0,b1) || std::max(b0,b1)+1 == std::min(a0,a1)) && a.user_id != b.user_id) return true;
        }
        return false;
    }
    if (a.line_no != b.line_no) return false;
    size_t a0 = a.col_start, a1 = a.col_end;
    size_t b0 = b.col_start, b1 = b.col_end;
    return !(a1 <= b0 || b1 <= a0);
}
static inline int lww_compare(const Update& a, const Update& b) {
    if (a.timestamp_ns < b.timestamp_ns) return -1;
    if (a.timestamp_ns > b.timestamp_ns) return 1;
    // tie-breaker: smaller user_id wins
    if (a.user_id < b.user_id) return 1; // a wins
    if (a.user_id > b.user_id) return -1;
    return 0;
}

/**
 * @brief Check if update `a` wins over `b` in LWW arbitration
 * @param a First update
 * @param b Second update
 * @return true if `a` should be applied (wins), false otherwise
 * 
 * Simple wrapper around lww_compare() for boolean result.
 */
static inline bool a_wins_over_b(const Update& a, const Update& b) {
    return lww_compare(a,b) >= 0;
}

// ============================================================================
// SECTION 4: CRDT MERGE - LAST-WRITER-WINS WITH STRUCTURAL-FIRST ORDERING
// ============================================================================
/**
 * This section implements the core CRDT merge logic. Key principles:
 * 
 * 1. **Last-Writer-Wins (LWW)**: Highest timestamp wins conflicts
 * 2. **Structural-First**: Apply line insert/delete before inline edits
 * 3. **Index Stability**: Descending deletes, ascending inserts
 * 4. **Stale Suppression**: Ignore updates older than local line timestamp
 * 5. **Batch Threshold**: Merge only when N updates accumulated
 * 
 * This approach ensures deterministic convergence across all peers.
 */

/**
 * @brief Apply a single update to document lines
 * @param[in,out] lines Document line array (modified in place)
 * @param u Update to apply
 * 
 * Handles all update types:
 * - LineInsert: Insert new line at position
 * - LineDelete: Remove line at position (or find by content if index invalid)
 * - BlockInsert: Insert multiple lines from payload
 * - BlockDelete: Remove multiple lines
 * - Insert/Delete/Replace: Character-level modifications within a line
 * 
 * @note For inline ops, validates line_no < lines.size() before applying
 * @note For structural ops, adjusts indices to prevent out-of-bounds
 */
static void apply_update_to_lines(std::vector<std::string>& lines, const Update& u) {
    if (u.type == Update::Type::LineInsert) {
        size_t pos = std::min(u.line_no, lines.size());
        lines.insert(lines.begin() + (long)pos, u.new_content);
        return;
    }
    if (u.type == Update::Type::LineDelete) {
        if (u.line_no < lines.size()) {
            lines.erase(lines.begin() + (long)u.line_no);
        } else if (!u.old_content.empty()) {
            // Line number out of bounds - try to find by content
            for (size_t i = 0; i < lines.size(); ++i) {
                if (lines[i] == u.old_content) {
                    lines.erase(lines.begin() + (long)i);
                    break;
                }
            }
        }
        return;
    }
    if (u.type == Update::Type::BlockInsert) {
        // new_content contains one or more lines separated by '\n'
        size_t pos = std::min(u.line_no, lines.size());
        std::vector<std::string> block;
        if (!u.new_content.empty()) {
            size_t start = 0;
            for (size_t i = 0; i <= u.new_content.size(); ++i) {
                if (i == u.new_content.size() || u.new_content[i] == '\n') {
                    block.push_back(u.new_content.substr(start, i - start));
                    start = i + 1;
                }
            }
        }
        lines.insert(lines.begin()+ (long)pos, block.begin(), block.end());
        return;
    }
    if (u.type == Update::Type::BlockDelete) {
        // old_content holds the deleted lines separated by '\n' so we know count
        size_t count = 0;
        if (!u.old_content.empty()) { count = 1; for (char c : u.old_content) if (c=='\n') ++count; }
        if (count == 0) return;
        if (u.line_no >= lines.size()) return;
        size_t end = std::min(u.line_no + count, lines.size());
        lines.erase(lines.begin()+ (long)u.line_no, lines.begin()+ (long)end);
        return;
    }
    // Ignore inline ops that target non-existent lines; upstream rebase should prevent this.
    if (u.line_no >= lines.size()) return;
    std::string &line = lines[u.line_no];
    size_t s = std::min(u.col_start, line.size());
    size_t e = std::min(u.col_end, line.size());
    switch (u.type) {
        case Update::Type::Insert:
            line = line.substr(0, s) + u.new_content + line.substr(s);
            break;
        case Update::Type::Delete:
            line = line.substr(0, s) + line.substr(e);
            break;
        case Update::Type::Replace:
            line = line.substr(0, s) + u.new_content + line.substr(e);
            break;
        default: break; // structural handled earlier
    }
}

static void clear_screen() {
    std::cout << "\033[2J\033[H"; // clear + home
}

static bool stdout_supports_color() {
    if (std::getenv("SYNCTEXT_NO_COLOR")) return false;
    return isatty(STDOUT_FILENO);
}

static const char* col_green() { return stdout_supports_color() ? "\033[32m" : ""; }
static const char* col_red() { return stdout_supports_color() ? "\033[31m" : ""; }
static const char* col_yellow() { return stdout_supports_color() ? "\033[33m" : ""; }
static const char* col_cyan() { return stdout_supports_color() ? "\033[36m" : ""; }
static const char* col_reset() { return stdout_supports_color() ? "\033[0m" : ""; }

static void render_document(const std::vector<std::string>& lines,
                            const std::vector<Update>& updates,
                            struct timespec last_change_time,
                            bool show_spinner) {
    clear_screen();
    std::cout << "Document: " << g_doc_path << "\n";
    std::cout << "Last change: " << time_str_from(last_change_time.tv_sec) << "\n";
    std::cout << "----------------------------------------\n";
    // mark modified lines
    std::vector<char> modified(lines.size(), 0);
    for (const auto& u : updates) {
        if (u.line_no < modified.size()) modified[u.line_no] = 1;
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        std::cout << "Line " << i << ": " << lines[i];
        if (i < modified.size() && modified[i]) std::cout << "  " << col_yellow() << "[MODIFIED]" << col_reset();
        std::cout << "\n";
    }
    std::cout << "----------------------------------------\n";
    // Active users (fresh heartbeat and live PID). Also cleanup stale entries opportunistically.
    std::cout << "Active users: ";
    bool first = true;
    uint64_t now = now_ns();
    for (size_t i = 0; i < MAX_USERS; ++i) {
        auto &slot = g_registry->users[i];
        if (slot.active.load(std::memory_order_relaxed) == 2) {
            uint64_t hb = slot.last_heartbeat_ns.load(std::memory_order_relaxed);
            bool fresh = (now >= hb) && ((now - hb) <= g_stale_ns);
            bool alive = (kill(slot.pid, 0) == 0) || (errno == EPERM);
            if (fresh && alive) {
                if (!first) std::cout << ", ";
                std::cout << slot.user_id;
                first = false;
            } else {
                // Attempt to clear stale slot via CAS (lock-free best-effort)
                uint32_t expected = 2;
                slot.active.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
            }
        }
    }
    if (first) std::cout << "(none)";
    std::cout << "\n";

    // Changes
    for (const auto& u : updates) {
        std::cout << col_cyan() << "Change detected:" << col_reset() << " Line " << u.line_no
                  << ", columns " << u.col_start << "-" << u.col_end
                  << ", \"" << u.old_content << "\" → \"" << u.new_content << "\""
                  << "  (" << type_to_string(u.type) << ")\n";
    }
    // Status toggles
    auto onoff = [](bool b){ return b ? "ON" : "OFF"; };
    // Minimal status: only spinner & periodic flags retained (debug counters removed per request)
    std::cout << "Spinner: " << (g_spinner ? col_green() : col_red()) << onoff(g_spinner) << col_reset()
              << "  Periodic: " << (g_periodic_mtime ? col_green() : col_red()) << onoff(g_periodic_mtime) << col_reset() << "\n";
    if (g_hotkeys) {
        // Removed broadcast hotkey '[b]'; other hotkeys retained
        std::cout << col_green() << "[q] quit  [o] open editor  [s] toggle spinner  [p] toggle periodic" << col_reset() << "\n";
    }
    if (show_spinner) {
        static const char spin_chars[] = {'|','/','-','\\'};
        static int spin_idx = 0;
        char s = spin_chars[spin_idx++ & 3];
        std::cout << "Monitoring for changes... [" << s << "]\n";
    } else {
        std::cout << "Monitoring for changes...\n";
    }
    std::cout.flush();
}

static bool copy_doc_from_peer_if_possible() {
    uint64_t nowt = now_ns();
    for (size_t i = 0; i < MAX_USERS; ++i) {
        auto &slot = g_registry->users[i];
        if (slot.active.load(std::memory_order_relaxed) != 2) continue;
        if (std::strncmp(slot.user_id, g_user_id.c_str(), USER_ID_MAX) == 0) continue;
        uint64_t hb = slot.last_heartbeat_ns.load(std::memory_order_relaxed);
        bool fresh = (nowt >= hb) && ((nowt - hb) <= g_stale_ns);
        bool alive = (kill(slot.pid, 0) == 0) || (errno == EPERM);
        if (!(fresh && alive)) continue;
        std::string peer_path = std::string("user_docs/") + std::string(slot.user_id) + "_doc.txt";
        struct stat ps{};
        if (stat(peer_path.c_str(), &ps) != 0) continue;
        auto lines = read_file_lines(peer_path);
        FILE* f = std::fopen(g_doc_path.c_str(), "wb");
        if (!f) continue;
        for (size_t li = 0; li < lines.size(); ++li) {
            std::fwrite(lines[li].c_str(), 1, lines[li].size(), f);
            std::fwrite("\n",1,1,f);
        }
        std::fclose(f);
        return true;
    }
    return false;
}

static bool ensure_local_doc() {
    struct stat st{};
    if (stat(g_doc_path.c_str(), &st) != 0) {
        // Prefer peer sync over template initialization for new users
        if (copy_doc_from_peer_if_possible()) return true;
        write_initial_doc(g_doc_path);
        return true;
    }
    // If file exists, optionally adopt freshest active peer snapshot on startup to converge immediately
    bool do_startup_sync = parse_env_bool("SYNCTEXT_STARTUP_SYNC", true);
    if (do_startup_sync) {
        // Find freshest peer (by mtime) among active, fresh heartbeats
        time_t local_mtime = st.st_mtime;
        time_t best_peer_mtime = 0;
        std::string best_peer_user;
        std::vector<std::string> best_peer_lines;
        uint64_t nowt = now_ns();
        for (size_t i = 0; i < MAX_USERS; ++i) {
            auto &slot = g_registry->users[i];
            if (slot.active.load(std::memory_order_relaxed) != 2) continue;
            if (std::strncmp(slot.user_id, g_user_id.c_str(), USER_ID_MAX) == 0) continue;
            uint64_t hb = slot.last_heartbeat_ns.load(std::memory_order_relaxed);
            bool fresh = (nowt >= hb) && ((nowt - hb) <= g_stale_ns);
            bool alive = (kill(slot.pid, 0) == 0) || (errno == EPERM);
            if (!(fresh && alive)) continue;
            std::string peer_path = std::string("user_docs/") + std::string(slot.user_id) + "_doc.txt";
            struct stat ps{};
            if (stat(peer_path.c_str(), &ps) != 0) continue;
            if (ps.st_mtime >= best_peer_mtime) {
                auto lines = read_file_lines(peer_path);
                best_peer_mtime = ps.st_mtime;
                best_peer_user = slot.user_id;
                best_peer_lines = std::move(lines);
            }
        }
        if (!best_peer_user.empty()) {
            auto local_lines = read_file_lines(g_doc_path);
            bool differ = (local_lines != best_peer_lines);
            bool peer_newer = (best_peer_mtime > local_mtime);
            if (peer_newer || differ) {
                FILE* f = std::fopen(g_doc_path.c_str(), "wb");
                if (f) {
                    for (size_t li = 0; li < best_peer_lines.size(); ++li) {
                        std::fwrite(best_peer_lines[li].c_str(), 1, best_peer_lines[li].size(), f);
                        std::fwrite("\n",1,1,f);
                    }
                    std::fclose(f);
                    return true; // content updated
                }
            }
        }
    }
    return false;
}

// ============================================================================
// SECTION 6: FILE MONITORING - INOTIFY EVENT HANDLING
// ============================================================================

static InotifyState init_inotify_for_file(const std::string& path) {
    InotifyState s{};
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) {
        return s; // unavailable
    }
    int wd = inotify_add_watch(fd, path.c_str(), IN_MODIFY | IN_CLOSE_WRITE | IN_MOVE_SELF | IN_ATTRIB | IN_CREATE | IN_MOVED_TO);
    if (wd < 0) {
        close(fd);
        return s;
    }
    s.fd = fd;
    s.wd = wd;
    return s;
}

static InotifyFlags drain_inotify_events(int fd) {
    InotifyFlags res{};
    if (fd < 0) return res;
    char buf[4096]
        __attribute__ ((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t len = read(fd, buf, sizeof(buf));
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            break;
        }
        if (len == 0) break;
        res.any = true;
        ssize_t i = 0;
        while (i < len) {
            struct inotify_event* ev = (struct inotify_event*)(buf + i);
            if (ev->mask & IN_CLOSE_WRITE) res.close_write = true;
            if (ev->mask & IN_MODIFY) res.modify = true;
            if (ev->mask & IN_MOVED_TO) res.moved_to = true;
            if (ev->mask & IN_MOVE_SELF) res.move_self = true;
            i += sizeof(struct inotify_event) + ev->len;
        }
    }
    return res;
}

static void settle_inotify_quiet(int fd, int quiet_ms, int overall_ms) {
    if (fd < 0) return;
    uint64_t start = now_ns();
    uint64_t last = start;
    const int step = 50; // ms
    // Drain immediately once
    (void)drain_inotify_events(fd);
    for (;;) {
        usleep(step * 1000);
        InotifyFlags fl = drain_inotify_events(fd);
        uint64_t nowt = now_ns();
        if (fl.any) last = nowt;
        uint64_t since_last_ms = (nowt - last) / 1000000ull;
        uint64_t elapsed_ms = (nowt - start) / 1000000ull;
        if (since_last_ms >= (uint64_t)quiet_ms) break;
        if (elapsed_ms >= (uint64_t)overall_ms) break;
    }
}

static bool spawn_bg_argv(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        // child
        setsid();
        // detach stdio
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        std::vector<char*> cargs;
        cargs.reserve(args.size() + 1);
        for (auto& s : args) cargs.push_back(const_cast<char*>(s.c_str()));
        cargs.push_back(nullptr);
        execvp(cargs[0], cargs.data());
        _exit(127);
    }
    // parent – don't wait, fire-and-forget
    return true;
}

static bool spawn_bg_shell(const std::string& cmdline) {
    return spawn_bg_argv({"/bin/sh", "-c", cmdline});
}

static void open_local_editor_if_possible() {
    const char* disable = std::getenv("SYNCTEXT_NO_AUTO_OPEN");
    if (disable && *disable) return;

    // 1) Allow explicit override via environment
    if (const char* forced = std::getenv("SYNCTEXT_EDITOR")) {
        if (*forced) {
            if (spawn_bg_shell(std::string(forced) + " \"" + g_doc_path + "\"")) return;
        }
    }

    // 2) Respect VISUAL / EDITOR if set (via shell to handle args)
    if (const char* visual = std::getenv("VISUAL")) {
        if (*visual) {
            if (spawn_bg_shell(std::string(visual) + " \"" + g_doc_path + "\"")) return;
        }
    }
    if (const char* editor = std::getenv("EDITOR")) {
        if (*editor) {
            if (spawn_bg_shell(std::string(editor) + " \"" + g_doc_path + "\"")) return;
        }
    }

    // 3) Prefer Ubuntu's default text editors before falling back to xdg-open
    // Guard on DISPLAY for GUI attempts
    bool has_display = std::getenv("DISPLAY") && *std::getenv("DISPLAY");
    if (has_display) {
        const char* guis[] = {"gnome-text-editor", "gedit", "xed", "pluma", "mousepad", "leafpad", "kate", "code"};
        for (const char* gui : guis) {
            if (spawn_bg_argv({gui, g_doc_path})) return;
        }
        // 4) As last GUI resort, let OS association decide
        if (spawn_bg_argv({"xdg-open", g_doc_path})) return;
    }

    // 5) Fallback: open terminal editor in a new terminal emulator if available
    if (spawn_bg_argv({"x-terminal-emulator", "-e", "nano", g_doc_path})) return;
    if (spawn_bg_argv({"gnome-terminal", "--", "nano", g_doc_path})) return;
    if (spawn_bg_argv({"xterm", "-e", "nano", g_doc_path})) return;
    // If all fail, silently give up. User can open manually.
}

static void set_terminal_rawmode() {
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &g_term_orig) != 0) return;
    struct termios raw = g_term_orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) g_tty_ok = true;
}

static void restore_terminal_mode() {
    if (g_tty_ok) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_term_orig);
        g_tty_ok = false;
    }
}

static void handle_keypresses_nonblock() {
    char buf[32];
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return;
    for (ssize_t i = 0; i < n; ++i) {
        char c = buf[i];
        if (c == 'q' || c == 'Q') {
            g_stop = 1;
        } else if (c == 'o' || c == 'O') {
            open_local_editor_if_possible();
        } else if (c == 's' || c == 'S') {
            g_spinner = !g_spinner; // toggle spinner
            g_force_refresh = true;
        } else if (c == 'p' || c == 'P') {
            g_periodic_mtime = !g_periodic_mtime; // toggle periodic mtime
            g_force_refresh = true;
        }
    }
}

static uint64_t parse_env_seconds(const char* name, uint64_t def_sec) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def_sec;
    char* end = nullptr;
    unsigned long long s = std::strtoull(v, &end, 10);
    if (end == v || *end != '\0') return def_sec;
    return s;
}

static int parse_env_millis(const char* name, int def_ms) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def_ms;
    char* end = nullptr;
    long long ms = std::strtoll(v, &end, 10);
    if (end == v || *end != '\0') return def_ms;
    if (ms < 0) ms = 0;
    if (ms > 60000) ms = 60000; // clamp to 60s
    return (int)ms;
}

static bool parse_env_bool(const char* name, bool defVal=true) {
    const char* v = std::getenv(name);
    if (!v) return defVal;
    if (*v == '\0') return false;
    if (v[0]=='0' || v[0]=='n' || v[0]=='N' || v[0]=='f' || v[0]=='F') return false;
    return true;
}

// ============================================================================
// SECTION 5: MAIN EVENT LOOP & PROGRAM ENTRY POINT
// ============================================================================
/**
 * The main loop orchestrates all subsystems:
 * - File monitoring (inotify or polling)
 * - Change detection and diffing
 * - Broadcasting local updates
 * - Receiving and merging remote updates
 * - UI rendering and interaction
 * - Heartbeat and cleanup
 * 
 * Flow:
 * 1. Initialize registry, queue, listener thread
 * 2. Loop until SIGINT/SIGTERM:
 *    a. Check for file changes (inotify or mtime)
 *    b. Diff and broadcast if changed
 *    c. Drain received updates from ring buffer
 *    d. Merge if batch threshold reached
 *    e. Render UI
 *    f. Heartbeat every ~1 second
 *    g. Handle hotkeys
 * 3. Cleanup: deregister, close queues, restore terminal
 */

/**
 * @brief Main program entry point
 * @param argc Argument count
 * @param argv Argument vector
 * @return Exit code (0 on success)
 * 
 * Arguments:
 *   argv[1]: user_id (required) - Unique identifier for this editor instance
 *   argv[2]: merge_batch_n (optional) - Merge threshold (default 1)
 * 
 * Example:
 *   ./build/editor alice      # Immediate merge
 *   ./build/editor bob 5      # Merge after 5 updates
 */
int run_main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << argv[0] << " <user_id> [merge_batch_n]\n";
        std::cerr << "  user_id       : Unique identifier for this user\n";
        std::cerr << "  merge_batch_n : Optional merge batch size (default=1, immediate merge)\n";
        std::cerr << "                  Set to N>1 to merge only after N updates accumulate\n";
        std::cerr << "Examples:\n";
        std::cerr << "  " << argv[0] << " u1           # Merge every update (default)\n";
        std::cerr << "  " << argv[0] << " u1 5         # Merge after 5 updates accumulate\n";
        return 1;
    }
    
    // Create directories for user documents and log files if they don't exist
    mkdir("user_docs", 0755);
    mkdir("log_files", 0755);
    
    g_user_id = argv[1];
    
    // Parse optional merge_batch_n argument
    if (argc == 3) {
        int n = std::atoi(argv[2]);
        if (n < 1) {
            std::cerr << "Warning: merge_batch_n must be >= 1, using default (1)\n";
            n = 1;
        }
        if (n > 1000) {
            std::cerr << "Warning: merge_batch_n too large, capping at 1000\n";
            n = 1000;
        }
        g_merge_batch_n = n;
        std::cout << "Merge batch size set to: " << g_merge_batch_n << "\n";
    }
    
    g_doc_path = std::string("user_docs/") + g_user_id + std::string("_doc.txt");
    g_queue_name = make_queue_name(g_user_id);

    install_signal_handlers();
    init_registry();
    init_wake_fd();
    // Create/open our own message queue for receiving updates
    open_message_queue(g_queue_name);
    if (g_mq != (mqd_t)-1) {
        std::cout << "Opened receive queue: " << g_queue_name << "\n";
        g_listener_thread = std::thread(listener_loop);
    }

    // Registration with optional interactive prompt on duplicate session (default on when TTY)
    g_interactive_login = isatty(STDIN_FILENO);
    if (std::getenv("SYNCTEXT_INTERACTIVE_LOGIN")) {
        g_interactive_login = parse_env_bool("SYNCTEXT_INTERACTIVE_LOGIN", true) && isatty(STDIN_FILENO);
    }
    bool force_reclaim_env = parse_env_bool("SYNCTEXT_FORCE_RECLAIM", false);
    for (;;) {
        RegisterResult rr = register_user_try(g_user_id, force_reclaim_env);
        if (rr == RegisterResult::Ok) break;
        if (rr == RegisterResult::NoSlots) {
            std::cerr << "No free user slots available (max " << MAX_USERS << ")\n";
            return 2;
        }
        // DuplicateActive
        if (!g_interactive_login) {
            bool fresh = false;
            pid_t pid = find_active_pid_for_user(g_user_id, fresh);
            std::cerr << "User '" << g_user_id << "' is already active";
            if (pid > 0) std::cerr << " (pid=" << pid << ")";
            std::cerr << ". Close the other instance, use a different user_id, set SYNCTEXT_FORCE_RECLAIM=1, or enable interactive prompt with SYNCTEXT_INTERACTIVE_LOGIN=1.\n";
            return 3;
        }
        // Interactive prompt
        std::cout << "User '" << g_user_id << "' is already active. Press "
                  << "f to force reclaim, q to quit, any other key to retry: " << std::flush;
        std::string line;
        std::getline(std::cin, line);
        int c = line.empty() ? '\n' : (unsigned char)line[0];
        if (c == 'q' || c == 'Q') {
            return 3;
        } else if (c == 'f' || c == 'F') {
            force_reclaim_env = true;
        } else {
            // brief backoff before retry
            usleep(300 * 1000);
        }
    }

    bool created = ensure_local_doc();
    if (created) {
        std::cout << "Initialized new document for user '" << g_user_id << "' at " << g_doc_path << "\n";
    } else {
        std::cout << "Resuming existing document for user '" << g_user_id << "' at " << g_doc_path << "\n";
    }
    if (parse_env_bool("SYNCTEXT_STATUS", false)) {
        std::cout << "[status] protocol=2 block_ops_mode=" << g_block_ops_mode
                  << " block_ops_min_lines=" << g_block_ops_min_lines
                  << " mq_maxmsg=" << g_mq_maxmsg
                  << " broadcast_batch=" << g_broadcast_batch_n
                  << " merge_batch=" << g_merge_batch_n
                  << " max_broadcast_per_cycle=" << g_max_broadcast_per_cycle
                  << " send_retry_count=" << g_send_retry_count
                  << " send_retry_delay_ms=" << g_send_retry_delay_ms
                  << " merge_write_grace_ms=" << g_merge_write_grace_ms
                  << " mass_diff_recheck_ms=" << g_mass_diff_recheck_ms
                  << " suspicious_min_lines=" << g_suspicious_min_lines
                  << " suspicious_ratio=" << g_suspicious_ratio
                  << "\n";
    }
    // Configure staleness threshold from env (seconds)
    uint64_t stale_sec = parse_env_seconds("SYNCTEXT_STALE_SEC", 10);
    g_stale_ns = stale_sec * 1000000000ull;

    // Enable hotkeys by default when attached to TTY; allow explicit disable via env
    // Hotkeys enabled by default when attached to TTY unless SYNCTEXT_HOTKEYS explicitly set
    if (std::getenv("SYNCTEXT_HOTKEYS")) {
        g_hotkeys = parse_env_bool("SYNCTEXT_HOTKEYS", true);
    } else {
        g_hotkeys = isatty(STDIN_FILENO);
    }
    if (g_hotkeys) set_terminal_rawmode();
    auto prev_lines = read_file_lines(g_doc_path);
    // Initialize per-line timestamp vector to 0 (unknown), so remote ops aren't incorrectly treated as stale
    g_local_line_ts.assign(prev_lines.size(), 0);

    struct stat st{};
    struct timespec last_mtime = {0, 0};
    if (stat(g_doc_path.c_str(), &st) == 0) last_mtime = st.st_mtim;

    // Set up inotify (best-effort). Fallback to timed polling if unavailable.
    InotifyState ino = init_inotify_for_file(g_doc_path);

    // Spinner flag from env (off by default; enable with SYNCTEXT_SPINNER=1)
    g_spinner = parse_env_bool("SYNCTEXT_SPINNER", false);
    // Idle refresh interval (seconds) when spinner is off
    g_idle_refresh_sec = parse_env_seconds("SYNCTEXT_IDLE_REFRESH_SEC", 5);
    // Periodic mtime check even if inotify is available (off by default)
    g_periodic_mtime = parse_env_bool("SYNCTEXT_PERIODIC_MTIME", false);
    // Debounce and settle durations after FS events to avoid transient full-delete/full-insert flicker
    g_debounce_ms = parse_env_millis("SYNCTEXT_DEBOUNCE_MS", g_debounce_ms);
    g_settle_ms = parse_env_millis("SYNCTEXT_SETTLE_MS", g_settle_ms);
    g_settle_overall_ms = parse_env_millis("SYNCTEXT_SETTLE_OVERALL_MS", g_settle_overall_ms);
    g_mass_diff_recheck_ms = parse_env_millis("SYNCTEXT_MASS_DIFF_RECHECK_MS", g_mass_diff_recheck_ms);
    g_suspicious_min_lines = parse_env_millis("SYNCTEXT_SUSPICIOUS_MIN_LINES", g_suspicious_min_lines);
    if (g_suspicious_min_lines < 1) g_suspicious_min_lines = 1;
    g_suspicious_ratio = parse_env_millis("SYNCTEXT_SUSPICIOUS_RATIO", g_suspicious_ratio);
    if (g_suspicious_ratio < 50) g_suspicious_ratio = 50;
    if (g_suspicious_ratio > 100) g_suspicious_ratio = 100;
    // Broadcast batch size (>=1). If set to 1, each local change is broadcast immediately.
    {
        int n = parse_env_millis("SYNCTEXT_BROADCAST_BATCH_N", g_broadcast_batch_n);
        if (n < 1) n = 1;
        if (n > 1000) n = 1000; // clamp reuse millis parser for convenience
        g_broadcast_batch_n = n;
    }
    // Merge batch size (>=1). After total of local+remote ops reaches this, attempt CRDT merge.
    {
        int n = parse_env_millis("SYNCTEXT_MERGE_BATCH_N", g_merge_batch_n);
        if (n < 1) n = 1;
        if (n > 1000) n = 1000;
        g_merge_batch_n = n;
    }
    // Grace window before writing merged content (ms) to avoid racing local editor writes
    g_merge_write_grace_ms = parse_env_millis("SYNCTEXT_MERGE_WRITE_GRACE_MS", g_merge_write_grace_ms);
    // mq_send retry config
    g_send_retry_count = parse_env_millis("SYNCTEXT_SEND_RETRY_COUNT", g_send_retry_count);
    if (g_send_retry_count < 0) g_send_retry_count = 0;
    if (g_send_retry_count > 200) g_send_retry_count = 200;
    g_send_retry_delay_ms = parse_env_millis("SYNCTEXT_SEND_RETRY_DELAY_MS", g_send_retry_delay_ms);
    if (g_send_retry_delay_ms < 0) g_send_retry_delay_ms = 0;
    if (g_send_retry_delay_ms > 500) g_send_retry_delay_ms = 500;
    g_max_broadcast_per_cycle = parse_env_millis("SYNCTEXT_MAX_BROADCAST_PER_CYCLE", g_max_broadcast_per_cycle);
    if (g_max_broadcast_per_cycle < 1) g_max_broadcast_per_cycle = 1;
    if (g_max_broadcast_per_cycle > 1000) g_max_broadcast_per_cycle = 1000;
    // MQ maxmsg size for receiver queue creation
    g_mq_maxmsg = parse_env_millis("SYNCTEXT_MQ_MAXMSG", g_mq_maxmsg);
    if (g_mq_maxmsg < 8) g_mq_maxmsg = 8;
    if (g_mq_maxmsg > 1024) g_mq_maxmsg = 1024;
    // Debug flag for verbose messaging
    g_debug_msg = parse_env_bool("SYNCTEXT_DEBUG_MSG", false);
    g_debug_level = parse_env_millis("SYNCTEXT_DEBUG_LEVEL", g_debug_level);
    if (g_debug_level < 0) g_debug_level = 0;
    if (g_debug_level > 3) g_debug_level = 3;
    // Backward compatibility: if legacy flag enabled and explicit level not set (level==0) elevate to verbose
    if (g_debug_msg && g_debug_level == 0) g_debug_level = 2;
    // Block ops configuration from env (0=never,1=auto,2=always) and threshold
    g_block_ops_mode = parse_env_millis("SYNCTEXT_BLOCK_OPS_MODE", g_block_ops_mode);
    if (g_block_ops_mode < 0) g_block_ops_mode = 0;
    if (g_block_ops_mode > 2) g_block_ops_mode = 2;
    g_block_ops_min_lines = parse_env_millis("SYNCTEXT_BLOCK_OPS_MIN_LINES", g_block_ops_min_lines);
    if (g_block_ops_min_lines < 2) g_block_ops_min_lines = 2;
    if (g_block_ops_min_lines > 1000) g_block_ops_min_lines = 1000;

    // Open the local editor (best-effort, non-blocking)
    open_local_editor_if_possible();

    // Initialize receive ring
    g_rx_ring.resize(RX_RING_CAP);
    g_rx_local.clear();
    // Initial render
    render_document(prev_lines, {}, last_mtime, g_spinner);

    // Main loop: event-driven with 1s timeout; fallback to stat check. Also poll stdin for hotkeys.
    uint64_t last_rx_total = g_rx_total.load(std::memory_order_relaxed);
    bool merge_due_remote = false;
    while (!g_stop) {
        heartbeat();

        bool changed = false;
        // Unconditional deferred-merge flush: ensure we don't depend on status refresh to commit merged view
        if (!g_pending_merge_lines.empty()) {
            uint64_t nowt = now_ns();
            if ((nowt - g_pending_merge_ns)/1000000ull >= (uint64_t)g_merge_write_grace_ms) {
                FILE* f = std::fopen(g_doc_path.c_str(), "wb");
                if (f) {
                    for (size_t li = 0; li < g_pending_merge_lines.size(); ++li) {
                        std::fwrite(g_pending_merge_lines[li].c_str(), 1, g_pending_merge_lines[li].size(), f);
                        std::fwrite("\n",1,1,f);
                    }
                    std::fclose(f);
                    struct stat st_upd; if (stat(g_doc_path.c_str(), &st_upd) == 0) last_mtime = st_upd.st_mtim;
                    g_last_merge_write_ns.store(now_ns(), std::memory_order_relaxed);
                }
                g_pending_merge_lines.clear();
                g_pending_merge_ns = 0;
            }
        }
        struct pollfd pfds[3];
        int nfds = 0;
        if (ino.fd >= 0) { pfds[nfds++] = {ino.fd, POLLIN, 0}; }
    if (g_hotkeys && isatty(STDIN_FILENO)) { pfds[nfds++] = {STDIN_FILENO, POLLIN, 0}; }
        if (g_wake_fd >= 0) { pfds[nfds++] = {g_wake_fd, POLLIN, 0}; }

        int timeout_ms = g_spinner ? 1000 : (int)(g_idle_refresh_sec * 1000);
        if (timeout_ms < 100) timeout_ms = 100;

        bool saw_move_self = false;
        if (nfds > 0) {
            int pr = poll(pfds, nfds, timeout_ms);
            if (pr > 0) {
                for (int i = 0; i < nfds; ++i) {
                    if ((pfds[i].fd == ino.fd) && (pfds[i].revents & POLLIN)) {
                        InotifyFlags fl = drain_inotify_events(ino.fd);
                        // Prefer acting only on close_write/moved_to to avoid mid-save snapshots
                        if (fl.close_write || fl.moved_to || (g_periodic_mtime && fl.modify) || fl.move_self) {
                            changed = true;
                        }
                        saw_move_self = fl.move_self;
                    } else if (g_hotkeys && (pfds[i].fd == STDIN_FILENO) && (pfds[i].revents & POLLIN)) {
                        handle_keypresses_nonblock();
                    } else if ((pfds[i].fd == g_wake_fd) && (pfds[i].revents & POLLIN)) {
                        drain_wake_fd();
                        g_force_refresh = true; // immediate status refresh (and, in future, apply inbound updates)
                    }
                }
            } else if (pr == 0) {
                // On timeout: optional periodic mtime check and react to inbound messages by refreshing UI
                if (g_periodic_mtime) {
                    if (stat(g_doc_path.c_str(), &st) == 0 && !timespec_equal(st.st_mtim, last_mtime)) {
                        changed = true;
                    }
                }
                uint64_t cur_rx = g_rx_total.load(std::memory_order_relaxed);
                if (cur_rx != last_rx_total) {
                    last_rx_total = cur_rx;
                    g_force_refresh = true; // show updated inbound counter
                }
            }
        } else {
            // Fallback: 1s polling on mtime
            usleep(timeout_ms * 1000);
            if (stat(g_doc_path.c_str(), &st) == 0 && !timespec_equal(st.st_mtim, last_mtime)) {
                changed = true;
            }
            uint64_t cur_rx = g_rx_total.load(std::memory_order_relaxed);
            if (cur_rx != last_rx_total) {
                last_rx_total = cur_rx;
                g_force_refresh = true;
            }
            if (g_hotkeys) handle_keypresses_nonblock();
        }

        if (changed && g_debounce_ms > 0) {
            usleep(g_debounce_ms * 1000);
            settle_inotify_quiet(ino.fd, g_settle_ms, g_settle_overall_ms);
        }

        if (saw_move_self && ino.fd >= 0) {
            // Re-establish the watch in case the file was replaced via rename
            if (ino.wd >= 0) inotify_rm_watch(ino.fd, ino.wd);
            ino = init_inotify_for_file(g_doc_path);
        }

        // Drain any received updates into local vector (for future CRDT merge logic)
        {
            Update tmp;
            bool any = false;
            while (rx_ring_pop(tmp)) {
                g_rx_local.push_back(std::move(tmp));
                any = true;
            }
            if (any) {
                // trigger status refresh to show updated rx-buf size
                g_force_refresh = true;
                // Only trigger merge if batch threshold reached
                if ((int)g_rx_local.size() >= g_merge_batch_n) {
                    merge_due_remote = true;
                }
            }
        }

        if (stat(g_doc_path.c_str(), &st) == 0) {
            if (changed || !timespec_equal(st.st_mtim, last_mtime)) {
                last_mtime = st.st_mtim;
                // Perform a stable read to avoid transient empty/full content during saves
                int stable_wait_ms = std::max(g_debounce_ms, 120);
                auto new_lines = read_file_lines_stable(g_doc_path, stable_wait_ms);
                // Detect transient empty snapshot (common during editor save truncate + rewrite)
                bool transient_empty = false;
                if (new_lines.empty() && !prev_lines.empty()) {
                    auto retry_lines = read_file_lines_stable(g_doc_path, 80); // short second read
                    if (!retry_lines.empty()) {
                        new_lines = std::move(retry_lines); // recovered; treat as normal
                    } else {
                        // Still empty after quick retry: likely a real delete OR large truncate-in-progress.
                        // Mark transient only if file size is non-zero (mid-write) but lines parse empty.
                        struct stat st_tmp{}; if (stat(g_doc_path.c_str(), &st_tmp)==0 && st_tmp.st_size>0) transient_empty = true;
                    }
                }

                // Heuristic: if nearly all lines appear changed (or were all deleted), re-read once after short delay.
                auto assess_mass_change = [&](const std::vector<std::string>& oldL, const std::vector<std::string>& newL)->bool {
                    if (oldL.empty()) return false; // initial load
                    if (newL.empty()) return true; // apparent full delete
                    size_t same = 0;
                    size_t maxn = std::max(oldL.size(), newL.size());
                    for (size_t i = 0; i < maxn; ++i) {
                        if (i < oldL.size() && i < newL.size() && oldL[i] == newL[i]) same++;
                    }
                    size_t changed = maxn - same;
                    // consider suspicious if >= configured ratio% changed and at least configured min lines
                    size_t ratio_threshold = (oldL.size() * (size_t)g_suspicious_ratio) / 100;
                    if (ratio_threshold == 0) ratio_threshold = 1;
                    return (changed >= ratio_threshold) && (changed >= (size_t)g_suspicious_min_lines);
                };
                bool suspicious = assess_mass_change(prev_lines, new_lines);
                if (suspicious && g_mass_diff_recheck_ms > 0) {
                    size_t oldN = prev_lines.size();
                    size_t newN = new_lines.size();
                    size_t diffN = (oldN > newN) ? (oldN - newN) : (newN - oldN);
                    bool new_is_empty = newN == 0 && oldN > 0; // typical transient-save empty snapshot
                    bool small_structural = diffN > 0 && diffN <= 20;
                    // Always recheck if the new snapshot is empty; otherwise skip recheck only for modest changes
                    if (new_is_empty || !small_structural) {
                        usleep(g_mass_diff_recheck_ms * 1000);
                        auto confirm_lines = read_file_lines_stable(g_doc_path, std::max(g_debounce_ms, 120));
                        bool still_suspicious = assess_mass_change(prev_lines, confirm_lines);
                        // If second read is less suspicious (more overlap), prefer it
                        if (!still_suspicious) {
                            new_lines = std::move(confirm_lines);
                        }
                    }
                }

                if (transient_empty) {
                    // Skip broadcasting mass deletes for transient empty snapshot; wait for next stable cycle.
                    // Render unchanged document to avoid flicker.
                    render_document(prev_lines, {}, last_mtime, g_spinner);
                    g_force_refresh = false;
                    continue; // do not advance baseline
                }
                auto updates = diff_documents(prev_lines, new_lines);
                // Record timestamp for true local changes (used by merge grace window logic)
                if (!updates.empty()) {
                    g_last_local_change_ns.store(now_ns(), std::memory_order_relaxed);
                }
                // Record a coarse-grained timestamp for last local structural delete to avoid stale remote reinserts
                bool saw_local_delete = false;
                for (const auto &uu : updates) {
                    if (uu.type == Update::Type::LineDelete || uu.type == Update::Type::BlockDelete) { saw_local_delete = true; break; }
                }
                if (saw_local_delete) {
                    g_last_local_delete_ns.store(now_ns(), std::memory_order_relaxed);
                }
                // If only one small inline update (not structural) and debounce disabled, force immediate broadcast/render for snappier UX
                if (g_debounce_ms == 0 && updates.size() == 1 && !is_structural(updates[0]) && updates[0].new_content.size() < 32 && updates[0].old_content.size() < 32) {
                    g_force_refresh = true;
                }
                if (suspicious && updates.size() >= prev_lines.size() && g_mass_diff_recheck_ms > 0) {
                    // Tag these updates as suppressed noise (we'll drop them entirely if still mass changes)
                    // If still full delete/insert pattern after retry, accept; else if replaced by stable content above updates already reflect corrected snapshot.
                    bool allDelete = true;
                    for (auto &u : updates) if (u.type != Update::Type::Delete) { allDelete = false; break; }
                    if (allDelete) {
                        // After confirmation, if content is empty we accept; else we ignore this mass delete burst.
                        if (!new_lines.empty()) {
                            updates.clear(); // discard noisy mass delete
                            new_lines = prev_lines; // keep previous view
                        }
                    }
                }
                // Accumulate for broadcasting
                for (auto &u : updates) {
                    g_tx_pending.push_back(u);
                    g_local_since_merge.push_back(u);
                }
                // Flush pending broadcasts in chunks, honoring g_max_broadcast_per_cycle, without dropping remainder
                if ((int)g_tx_pending.size() >= g_broadcast_batch_n) {
                    // Drain a few chunks this iteration to improve latency for structural storms
                    int chunks = 0;
                    while (!g_tx_pending.empty() && chunks < 4) {
                        size_t sent = broadcast_updates_to_all(g_tx_pending);
                        if (sent == 0) break;
                        g_tx_pending.erase(g_tx_pending.begin(), g_tx_pending.begin() + (long)sent);
                        ++chunks;
                        // If below batch threshold, pause until next cycle
                        if ((int)g_tx_pending.size() < g_broadcast_batch_n) break;
                    }
                }
                // Render only when we actually have updates to show to avoid immediate duplicate no-op frame
                if (!updates.empty()) {
                    render_document(new_lines, updates, last_mtime, g_spinner);
                    // We just rendered; suppress a status-only refresh right after
                    g_force_refresh = false;
                }
                // Preserve previous baseline correctly (avoid destructive move which empties the source vector)
                prev_lines = new_lines;
                update_local_line_timestamps_after(updates);
                // no-op
            }
        }

        // Status/merge phase: previously gated on (!did_render && (g_force_refresh||g_spinner)) which could starve remote merges
        // when a local render already occurred this iteration. Include merge_due_remote and drop the !did_render guard so LWW
        // remote replacements apply promptly even after a local change render.
            if ((g_force_refresh || g_spinner || merge_due_remote)) {
                // Flush deferred merge after grace window
                if (!g_pending_merge_lines.empty()) {
                    uint64_t nowt = now_ns();
                    if ((nowt - g_pending_merge_ns)/1000000ull >= (uint64_t)g_merge_write_grace_ms) {
                        FILE* f = std::fopen(g_doc_path.c_str(), "wb");
                        if (f) {
                            for (size_t li = 0; li < g_pending_merge_lines.size(); ++li) {
                                std::fwrite(g_pending_merge_lines[li].c_str(), 1, g_pending_merge_lines[li].size(), f);
                                std::fwrite("\n",1,1,f);
                            }
                            std::fclose(f);
                            struct stat st_upd; if (stat(g_doc_path.c_str(), &st_upd) == 0) last_mtime = st_upd.st_mtim;
                            g_last_merge_write_ns.store(now_ns(), std::memory_order_relaxed);
                        }
                        g_pending_merge_lines.clear();
                        g_pending_merge_ns = 0;
                    }
                }
            // Attempt CRDT merge if threshold reached
            if (merge_due_remote) {
                size_t pending_remote = g_rx_local.size();
                // Merge only remote updates to avoid local-vs-remote arbitration divergence across peers
                std::vector<Update> all;
                all.reserve(pending_remote);
                all.insert(all.end(), g_rx_local.begin(), g_rx_local.end());
                // Conflict detection & LWW filtering
                std::vector<int> keep(all.size(), 1);
                for (size_t i = 0; i < all.size(); ++i) {
                    if (!keep[i]) continue;
                    for (size_t j = i + 1; j < all.size(); ++j) {
                        if (!keep[j]) continue;
                        if (ranges_overlap(all[i], all[j])) {
                            // Do not consider updates from the same user as conflicting; allow co-application
                            if (all[i].user_id == all[j].user_id) continue;
                            bool i_wins = a_wins_over_b(all[i], all[j]);
                            if (i_wins) keep[j] = 0; else keep[i] = 0;
                        }
                    }
                }
                std::vector<Update> winners;
                for (size_t i = 0; i < all.size(); ++i) if (keep[i]) winners.push_back(all[i]);
                // Partition structural vs inline
                std::vector<Update> structural, inlineOps;
                structural.reserve(winners.size()); inlineOps.reserve(winners.size());
                for (auto &u : winners) {
                    if (is_structural(u)) structural.push_back(u); else inlineOps.push_back(u);
                }
                // Drop stale remote reinserts that are older than our last local delete epoch
                uint64_t last_del = g_last_local_delete_ns.load(std::memory_order_relaxed);
                if (last_del) {
                    auto is_stale_insert = [&](const Update& u)->bool{
                        if (u.type == Update::Type::LineInsert || u.type == Update::Type::BlockInsert) {
                            return u.timestamp_ns < last_del;
                        }
                        return false;
                    };
                    structural.erase(std::remove_if(structural.begin(), structural.end(), is_stale_insert), structural.end());
                }
                // Further drop remote ops that are older than our local per-line timestamps (prevents stale resurrect/overwrites)
                auto is_remote_stale = [&](const Update& u)->bool{
                    if (u.user_id == g_user_id) return false;
                    if (u.type == Update::Type::BlockInsert || u.type == Update::Type::BlockDelete) {
                        size_t cnt = 0;
                        if (u.type == Update::Type::BlockInsert) cnt = block_line_count(u);
                        else cnt = block_line_count(u);
                        for (size_t i=0;i<cnt;++i) {
                            size_t ln = u.line_no + i;
                            if (ln < g_local_line_ts.size() && u.timestamp_ns < g_local_line_ts[ln]) return true;
                        }
                        return false;
                    }
                    if (u.line_no < g_local_line_ts.size()) return u.timestamp_ns < g_local_line_ts[u.line_no];
                    return false;
                };
                structural.erase(std::remove_if(structural.begin(), structural.end(), is_remote_stale), structural.end());
                inlineOps.erase(std::remove_if(inlineOps.begin(), inlineOps.end(), is_remote_stale), inlineOps.end());
                // Apply structural deletes descending, then inserts ascending
                std::vector<Update> deletes, inserts;
                for (auto &u : structural) {
                    if (u.type == Update::Type::LineDelete || u.type == Update::Type::BlockDelete) deletes.push_back(u); else inserts.push_back(u);
                }
                std::sort(deletes.begin(), deletes.end(), [](const Update& a, const Update& b){ return a.line_no > b.line_no; });
                std::sort(inserts.begin(), inserts.end(), [](const Update& a, const Update& b){ return a.line_no < b.line_no; });
                // Build merged view without mutating prev_lines until fully computed.
                auto merged_lines = prev_lines;
                // Track cumulative line shifts as we apply remote structural ops only.
                // Local structural ops already reflected in prev_lines; don't double-apply.
                if (g_debug_level >= 2 && !deletes.empty()) {
                    std::cerr << "[debug] Processing " << deletes.size() << " deletes, doc has " << merged_lines.size() << " lines\n";
                }
                for (auto &u : deletes) {
                    if (g_debug_level >= 2) {
                        std::cerr << "[debug]   Delete line=" << u.line_no << " user=" << u.user_id << " g_user_id=" << g_user_id << " skip=" << (u.user_id == g_user_id ? "YES" : "NO") << "\n";
                    }
                    if (u.user_id == g_user_id) continue; // skip local
                    apply_update_to_lines(merged_lines, u);
                    if (g_debug_level >= 2) {
                        std::cerr << "[debug]   Doc size after delete: " << merged_lines.size() << "\n";
                    }
                }
                if (g_debug_level >= 2 && !inserts.empty()) {
                    std::cerr << "[debug] Applying " << inserts.size() << " inserts to doc with " << merged_lines.size() << " lines\n";
                }
                for (auto &u : inserts) {
                    if (u.user_id == g_user_id) continue; // skip local
                    if (g_debug_level >= 2) {
                        std::cerr << "[debug]   Insert at line " << u.line_no << ", content_len=" << u.new_content.size() << ", doc_size_before=" << merged_lines.size() << "\n";
                    }
                    apply_update_to_lines(merged_lines, u);
                    if (g_debug_level >= 2) {
                        std::cerr << "[debug]   Doc size after: " << merged_lines.size() << "\n";
                    }
                }
                // Rebase inline remote ops: earlier structural ops shift subsequent line numbers.
                for (auto u : inlineOps) {
                    if (u.user_id == g_user_id) continue;
                    long shift = 0;
                    auto count_lines = [](const Update& op)->size_t{
                        if (op.type == Update::Type::BlockInsert) {
                            const std::string& t = op.new_content; if (t.empty()) return 0; size_t c=1; for (char ch : t) if (ch=='\n') ++c; return c;
                        } else if (op.type == Update::Type::BlockDelete) {
                            const std::string& t = op.old_content; if (t.empty()) return 0; size_t c=1; for (char ch : t) if (ch=='\n') ++c; return c;
                        } else {
                            return 1;
                        }
                    };
                    for (auto &d : deletes) if (d.user_id != g_user_id) {
                        size_t k = (d.type == Update::Type::BlockDelete) ? count_lines(d) : 1;
                        if (u.line_no > d.line_no) shift -= (long)k;
                    }
                    for (auto &ins : inserts) if (ins.user_id != g_user_id) {
                        size_t k = (ins.type == Update::Type::BlockInsert) ? count_lines(ins) : 1;
                        if (u.line_no >= ins.line_no) shift += (long)k;
                    }
                    long rebased = (long)u.line_no + (long)shift;
                    if (rebased < 0) rebased = 0;
                    u.line_no = (size_t)rebased;
                    if (u.line_no < merged_lines.size()) apply_update_to_lines(merged_lines, u);
                }
                bool changed_any = (merged_lines != prev_lines);
                bool merged_applied = false;
                if (changed_any) {
                    // Avoid racing with very recent local changes: small grace window
                    uint64_t nowt = now_ns();
                    uint64_t last_local = g_last_local_change_ns.load(std::memory_order_relaxed);
                    bool recent_local = (nowt >= last_local) && ((nowt - last_local)/1000000ull < (uint64_t)g_merge_write_grace_ms);
                    // If only remote inline ops are present (no structural winners), it's safe to commit immediately
                    bool only_inline_remote = structural.empty();
                    if (!recent_local || only_inline_remote) {
                        FILE* f = std::fopen(g_doc_path.c_str(), "wb");
                        if (f) {
                            for (size_t li = 0; li < merged_lines.size(); ++li) {
                                std::fwrite(merged_lines[li].c_str(), 1, merged_lines[li].size(), f);
                                std::fwrite("\n",1,1,f);
                            }
                            std::fclose(f);
                            struct stat st_upd;
                            if (stat(g_doc_path.c_str(), &st_upd) == 0) last_mtime = st_upd.st_mtim;
                        }
                        prev_lines = std::move(merged_lines);
                        // Update local timestamps for structurally changed lines
                        update_local_line_timestamps_after(structural);
                        // Mark the time so we skip echoing our own merge write
                        g_last_merge_write_ns.store(now_ns(), std::memory_order_relaxed);
                        render_document(prev_lines, {}, last_mtime, g_spinner);
                        merged_applied = true;
                    } else {
                        // Adopt merged view in memory and defer write to avoid racing editor; schedule flush
                        prev_lines = merged_lines;
                        update_local_line_timestamps_after(structural);
                        g_pending_merge_lines = prev_lines;
                        g_pending_merge_ns = nowt;
                        g_force_refresh = true;
                    }
                }
                // Only clear remote buffer if applied or nothing to do; otherwise, keep and retry
                if (merged_applied || !changed_any) {
                    g_rx_local.clear();
                    merge_due_remote = false;
                }
            }
            // Background-drain any pending local broadcasts even when no new local change triggered this cycle
            if (!g_tx_pending.empty()) {
                size_t sent = broadcast_updates_to_all(g_tx_pending);
                if (sent > 0) {
                    g_tx_pending.erase(g_tx_pending.begin(), g_tx_pending.begin() + (long)sent);
                }
            }
            render_document(prev_lines, {}, last_mtime, g_spinner);
            g_force_refresh = false;
        }
    }

    // Final cleanup
    deregister_user();
    if (ino.fd >= 0) {
        if (ino.wd >= 0) inotify_rm_watch(ino.fd, ino.wd);
        close(ino.fd);
    }
    if (g_listener_thread.joinable()) {
        g_stop = 1;
        g_listener_thread.join();
    }
    // Flush any pending updates on shutdown (in chunks)
    if (!g_tx_pending.empty()) {
        while (!g_tx_pending.empty()) {
            size_t sent = broadcast_updates_to_all(g_tx_pending);
            if (sent == 0) break;
            g_tx_pending.erase(g_tx_pending.begin(), g_tx_pending.begin() + (long)sent);
        }
    }
    if (g_hotkeys) restore_terminal_mode();
    if (g_wake_fd >= 0) close(g_wake_fd);
    if (g_mq != (mqd_t)-1) mq_close(g_mq);
    if (!g_queue_name.empty()) mq_unlink(g_queue_name.c_str());
    if (g_registry) munmap(g_registry, g_registry_size);
    if (g_shm_fd >= 0) close(g_shm_fd);
    return 0;
}

} // namespace synctext

int main(int argc, char** argv) {
    return synctext::run_main(argc, argv);
}