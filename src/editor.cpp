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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <mutex>
#include <memory>
#include <map>
#include <pwd.h>
#include <sys/ioctl.h>
#include <limits.h>

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
// Set from a signal handler AND read by the network/listener threads, so it needs to be
// atomic rather than merely volatile. Lock-free on every platform we target, which is what
// makes it safe to touch from a signal handler.
static std::atomic<int> g_stop{0};           ///< Global flag to signal graceful shutdown. Set by `handle_signal`.
static_assert(std::atomic<int>::is_always_lock_free, "g_stop must be lock-free to be signal-safe");
static Registry* g_registry = nullptr;       ///< Pointer to the mmapped shared memory registry.
static int g_shm_fd = -1;                    ///< File descriptor for the shared memory object.
static std::string g_user_id;                ///< Identifier for the current user.
static std::string g_doc_path;               ///< Absolute or relative path to the document being edited.
static std::string g_queue_name;             ///< Name of this instance's POSIX message queue.
static mqd_t g_mq = (mqd_t)-1;               ///< Descriptor for our own message queue.
static bool g_network_mode = false;          ///< True when running a LAN session (--host/--join).
static std::atomic<bool> g_network_host{false}; ///< True while we are the session host; flips on promotion.
static std::string g_access_code;            ///< Shared secret every participant presents in HELLO.
static int g_network_port = 9000;            ///< Session port: host listens here, joiners dial it.
static int g_listen_fd = -1;                 ///< Our own listening socket (used now, or after promotion).
static int g_listen_port = 0;                ///< Port actually bound by g_listen_fd (may be ephemeral).
static int g_conn_fd = -1;                   ///< Joiner: socket to the current host. -1 when we are host.
static std::thread g_network_thread;         ///< Host accept loop, or the joiner's failover supervisor.
static std::mutex g_network_clients_mutex;
static std::mutex g_network_send_mutex;
static std::mutex g_network_rx_mutex;
struct ServedDoc;   // multi-document server, defined further down
struct NetworkClient {
    int fd = -1;
    std::string user_id;
    std::atomic<bool> finished{false};
    ServedDoc* doc = nullptr;   ///< Non-null only in --serve mode: which document this client joined.
    std::string ip;             ///< Peer address, published in that document's roster.
    int port = 0;               ///< Peer's listening port, for failover.
};
static std::vector<std::shared_ptr<NetworkClient>> g_network_clients;
static std::mutex g_client_threads_mutex;
/// Per-joiner service threads. Kept joinable (not detached) so shutdown can wait for
/// them: a detached thread can outlive main and touch these globals mid-destruction.
static std::vector<std::pair<std::shared_ptr<NetworkClient>, std::thread>> g_client_threads;
/// One participant in the session, as known to everybody. Order is the succession order.
struct RosterEntry { std::string user_id; std::string ip; int port = 0; };
static std::mutex g_roster_mutex;
static std::vector<RosterEntry> g_roster;    ///< [0] is the current host; [1..] joiners in join order.
static std::atomic<int> g_host_generation{0}; ///< Bumped on every promotion; shown in the UI.
static struct timespec g_link_lost_mtime{};  ///< Doc mtime when the host link dropped (rejoin guard).
static int g_slot_index = -1;                ///< The index of our slot in the shared `g_registry`.

// --- UI & Interaction ---
static bool g_spinner = false;               ///< If true, an animated spinner is shown in the UI.
static bool g_hotkeys = false;               ///< If true, hotkey support for interaction is enabled.
static std::atomic<bool> g_force_refresh{false}; ///< Forces a UI re-render; set by network threads too, so atomic.
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

// ----------------------------------------------------------------------------
// Document scope
//
// Without --doc, everything keeps the original single-scope layout: one registry,
// one logical document per machine. With --doc <id>, the registry, the message
// queues and the files all carry the id, so sessions on different documents never
// discover each other and can run side by side.
// ----------------------------------------------------------------------------
static std::string g_doc_id;                 ///< Empty means the original single-document layout.
static std::string g_base_user_id;           ///< Identity before any "-2" collision suffix.
static bool g_user_id_explicit = false;      ///< True when SYNCTEXT_USER named the identity.
static std::vector<std::string> g_doc_ids;   ///< Every --doc given; more than one only makes sense with --serve.
static bool g_serve_mode = false;            ///< True for a headless multi-document server.
static constexpr size_t DOC_ID_MAX = 64;     ///< Longest accepted document id.

/// Who this session is, worked out rather than typed.
///
/// The document is what the user names; their own identity is an implementation
/// detail, so it comes from the OS account. The same account opening one document
/// twice on a machine would collide, so register_user_try() appends a numeric suffix
/// until the id is free. SYNCTEXT_USER overrides it, which scripts and tests use.
static std::string default_user_id() {
    const char* env = std::getenv("SYNCTEXT_USER");
    if (env && *env) return env;
    const char* name = std::getenv("USER");
    if (!name || !*name) name = std::getenv("LOGNAME");
    if (!name || !*name) {
        struct passwd* pw = getpwuid(getuid());
        if (pw && pw->pw_name && *pw->pw_name) name = pw->pw_name;
    }
    std::string out;
    for (char c : std::string(name ? name : "user")) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out.push_back(ok ? c : '_');
    }
    if (out.empty()) out = "user";
    if (out.size() > USER_ID_MAX - 5) out.resize(USER_ID_MAX - 5);  // room for a "-NN" suffix
    return out;
}

/// Validate a document id. Rejects rather than rewrites: silently mapping '/' to '_'
/// would let "a/b" and "a_b" land in the same document without anyone noticing, and
/// "." or ".." would point the scope at a directory that is already in use.
/// Returns true and fills `out` on success; on failure `why` explains the problem.
static bool validate_doc_id(const std::string& in, std::string& out, std::string& why) {
    if (in.empty()) { why = "document id is empty"; return false; }
    // Otherwise "--doc --host" would quietly consume the mode flag as an id.
    if (in[0] == '-') { why = "document id cannot start with '-' (missing the id after --doc?)"; return false; }
    if (in.size() > DOC_ID_MAX) {
        why = "document id is longer than " + std::to_string(DOC_ID_MAX) + " characters";
        return false;
    }
    for (char c : in) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) {
            why = "document id may only contain letters, digits, '-', '_' and '.'";
            return false;
        }
    }
    // "." and ".." resolve to a directory that is already somebody's scope, which
    // would give two sessions the same files under different registries.
    if (in.find_first_not_of('.') == std::string::npos) {
        why = "document id cannot be '" + in + "' (it would point outside its own directory)";
        return false;
    }
    out = in;
    return true;
}

/// Short, collision-free stand-in for the id inside POSIX queue names.
///
/// A queue name is capped at QUEUE_NAME_MAX, and it has to hold "/synctext_",
/// the scope, a separator and the user id. A long id would otherwise truncate the
/// user id away and hand every participant in the document the same queue.
static std::string doc_scope_tag() {
    const size_t prefix = sizeof("/synctext_") - 1;
    const size_t budget = QUEUE_NAME_MAX - 1 - prefix - 1 - (USER_ID_MAX - 1);
    if (g_doc_id.size() <= budget) return g_doc_id;
    uint32_t h = 2166136261u;                       // FNV-1a
    for (char c : g_doc_id) { h ^= (unsigned char)c; h *= 16777619u; }
    char hex[9];
    std::snprintf(hex, sizeof(hex), "%08x", h);
    size_t keep = (budget > 9) ? budget - 9 : 0;    // room for '_' + 8 hex digits
    return g_doc_id.substr(0, keep) + "_" + hex;
}
/// Directory holding this scope's documents.
static std::string doc_dir() {
    return g_doc_id.empty() ? std::string("user_docs")
                            : std::string("user_docs/") + g_doc_id;
}
/// Where a given participant's copy of this document lives.
static std::string doc_path_for(const std::string& user_id) {
    return doc_dir() + "/" + user_id + "_doc.txt";
}
/// Same, resolved to an absolute path. Sessions are routinely started from different
/// working directories, and peers must be able to find each other's documents.
static std::string resolve_doc_path(const std::string& user_id) {
    std::string rel = doc_path_for(user_id);
    char cwd[PATH_MAX];
    if (!rel.empty() && rel[0] != '/' && getcwd(cwd, sizeof(cwd))) {
        std::string abs = std::string(cwd) + "/" + rel;
        if (abs.size() < DOC_PATH_MAX) return abs;   // otherwise keep the relative form
    }
    return rel;
}
/// Peers are discovered through the registry, so a per-document registry is what
/// actually separates one document's participants from another's.
static std::string registry_shm_name() {
    return g_doc_id.empty() ? std::string(REGISTRY_SHM_NAME)
                            : std::string("/synctext_reg_") + g_doc_id;
}

// Forward decl for env parsing used early
static bool parse_env_bool(const char* name, bool defVal);
static bool spawn_bg_shell(const std::string& cmdline);
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
    g_shm_fd = shm_open(registry_shm_name().c_str(), O_RDWR | O_CREAT, 0666);
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

    // Lazy init if needed. A registry left behind by a build with a different
    // layout is reinitialized rather than misread.
    uint32_t expected = REGISTRY_VERSION;
    if (!g_registry->version.compare_exchange_strong(expected, REGISTRY_VERSION)) {
        if (g_registry->version.load() != REGISTRY_VERSION) {
            // Zero out safely
            std::memset(mem, 0, g_registry_size);
            g_registry->max_users = MAX_USERS;
            g_registry->version.store(REGISTRY_VERSION);
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
    // The queue namespace is global, so the document id has to be part of the name
    // or the same user id in two documents would collide.
    std::string name = g_doc_id.empty() ? std::string("/synctext_user_")
                                        : std::string("/synctext_") + doc_scope_tag() + "_";
    size_t user_chars = 0;
    for (char c : user_id) {
        if (user_chars++ >= USER_ID_MAX - 1) break;  // the registry stores no more than this
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

static void enqueue_received_update(Update&& u) {
    g_rx_total.fetch_add(1, std::memory_order_relaxed);
    if (g_network_mode) {
        std::lock_guard<std::mutex> lock(g_network_rx_mutex);
        if (!rx_ring_push(std::move(u))) g_rx_dropped.fetch_add(1, std::memory_order_relaxed);
    } else {
        if (!rx_ring_push(std::move(u))) g_rx_dropped.fetch_add(1, std::memory_order_relaxed);
    }
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

enum NetworkFrameKind : uint8_t { HELLO = 1, SNAPSHOT = 2, UPDATE = 3, ERROR = 4, ROSTER = 5, DOCINFO = 6 };
static constexpr uint32_t NETWORK_MAX_FRAME = 1024 * 1024;
static constexpr int RECONNECT_ATTEMPTS = 20;      ///< Tries per successor candidate during failover.
static constexpr int RECONNECT_DELAY_MS = 250;     ///< Pause between those tries.

static std::mutex g_conn_mutex;  ///< Serializes use/replacement of g_conn_fd so a closed fd is never reused mid-send.
static int g_connect_timeout_ms = 10000;  ///< Bound on a single connect attempt.
static bool g_port_explicit = false;      ///< True when the user named a port on the command line.
static int g_port_base = 9000;            ///< First port tried when none was named.
static int g_port_span = 16;              ///< How many consecutive ports to try / scan.

/// connect() with a deadline that also honours shutdown requests.
///
/// A blocking connect to an address that silently drops packets (wrong IP, host
/// down, firewall DROP rather than REJECT) parks for the full TCP SYN timeout --
/// minutes -- and a SIGTERM in the middle changes nothing, because the syscall is
/// restarted. Connecting non-blocking and polling in short slices keeps a typo in
/// the host address from looking like a hang, and lets Ctrl-C work.
static bool connect_with_timeout(int fd, const sockaddr_in& addr, int timeout_ms) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    bool ok = false;
    if (connect(fd, (const sockaddr*)&addr, sizeof(addr)) == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        const int slice_ms = 200;
        int waited = 0;
        while (waited < timeout_ms && !g_stop) {
            struct pollfd pf{fd, POLLOUT, 0};
            int pr = poll(&pf, 1, slice_ms);
            if (pr > 0) {
                int err = 0; socklen_t len = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) { errno = EIO; break; }
                if (err == 0) { ok = true; }
                else errno = err;
                break;
            }
            if (pr < 0 && errno != EINTR) break;
            waited += slice_ms;
        }
        if (!ok && waited >= timeout_ms) errno = ETIMEDOUT;
    }
    (void)fcntl(fd, F_SETFL, flags);   // the rest of the protocol expects blocking I/O
    return ok;
}

/// Bound how long a handshake read may block. A peer that accepts the connection
/// and then says nothing -- a half-open host, a blackholing middlebox, or our own
/// listener after a self-connect -- must not park the joiner forever.
static void set_recv_timeout(int fd, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/// True when a socket ended up connected to itself. A joiner listens on the session
/// port before it dials, so pointing --join at this machine's own address connects
/// to our own listener, which never answers a handshake.
static bool socket_connected_to_self(int fd) {
    sockaddr_in local{}, peer{};
    socklen_t ll = sizeof(local), pl = sizeof(peer);
    if (getsockname(fd, (sockaddr*)&local, &ll) != 0) return false;
    if (getpeername(fd, (sockaddr*)&peer, &pl) != 0) return false;
    // Same address on both ends means the destination resolved to this machine: the
    // kernel only picks a source address equal to the destination for a local target.
    if (local.sin_addr.s_addr != peer.sin_addr.s_addr) return false;
    // Either a simultaneous self-connect (identical ports), or -- the case that
    // actually bites -- we dialled the listening socket we opened for failover.
    return local.sin_port == peer.sin_port || ntohs(peer.sin_port) == g_listen_port;
}

/// Why the LAN session could not be established. A wrong access code, an unreachable
/// host and a busy port are very different problems; the operator should be told which.
static std::string g_network_error;

static bool send_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len) { ssize_t n = send(fd, p, len, MSG_NOSIGNAL); if (n <= 0) return false; p += n; len -= (size_t)n; }
    return true;
}
static bool recv_all(int fd, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    while (len) { ssize_t n = recv(fd, p, len, 0); if (n <= 0) return false; p += n; len -= (size_t)n; }
    return true;
}
static bool send_frame(int fd, uint8_t kind, const void* payload, size_t len) {
    if (fd < 0 || len > NETWORK_MAX_FRAME) return false;
    uint8_t h[5] = {kind, (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    std::lock_guard<std::mutex> lock(g_network_send_mutex);
    return send_all(fd, h, sizeof(h)) && (len == 0 || send_all(fd, payload, len));
}
static bool recv_frame(int fd, uint8_t& kind, std::vector<char>& payload) {
    uint8_t h[5]; if (!recv_all(fd, h, sizeof(h))) return false;
    uint32_t len = ((uint32_t)h[1] << 24) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 8) | h[4];
    if (len > NETWORK_MAX_FRAME) return false;
    kind = h[0]; payload.resize(len); return len == 0 || recv_all(fd, payload.data(), len);
}
static bool network_send_update(int fd, const Update& u) {
    if (fd < 0) return false;
    // Per-thread scratch buffer: NETWORK_MAX_FRAME is too large to place on the stack.
    static thread_local std::vector<char> data(NETWORK_MAX_FRAME);
    size_t len = 0;
    return serialize_update(u, data.data(), data.size(), len) && send_frame(fd, UPDATE, data.data(), len);
}

// ----------------------------------------------------------------------------
// Roster: the participant list every node keeps, and the succession order.
// g_roster[0] is the current host; [1..] are joiners in the order they connected.
// The host owns the list and rebroadcasts it on every membership change, so all
// survivors elect the same successor when the host disappears.
// ----------------------------------------------------------------------------
static std::string roster_serialize() {
    std::string out;
    std::lock_guard<std::mutex> lock(g_roster_mutex);
    for (const auto& e : g_roster) {
        out += e.user_id; out += '\t'; out += e.ip; out += '\t'; out += std::to_string(e.port); out += '\n';
    }
    return out;
}
static void roster_apply(const std::vector<char>& payload) {
    std::vector<RosterEntry> parsed;
    std::string text(payload.begin(), payload.end());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        RosterEntry e;
        e.user_id = line.substr(0, t1);
        e.ip = line.substr(t1 + 1, t2 - t1 - 1);
        e.port = std::atoi(line.c_str() + t2 + 1);
        if (!e.user_id.empty()) parsed.push_back(std::move(e));
    }
    std::lock_guard<std::mutex> lock(g_roster_mutex);
    g_roster = std::move(parsed);
}
/// Host only: push the current roster to every connected joiner.
static void roster_broadcast() {
    std::string blob = roster_serialize();
    std::lock_guard<std::mutex> lock(g_network_clients_mutex);
    for (const auto& client : g_network_clients) send_frame(client->fd, ROSTER, blob.data(), blob.size());
}
/// Host only: record (or refresh) a joiner. Keeps existing rank if the user is already listed.
static void roster_upsert(const std::string& user_id, const std::string& ip, int port) {
    std::lock_guard<std::mutex> lock(g_roster_mutex);
    if (g_roster.empty() || g_roster[0].user_id != g_user_id) {
        RosterEntry self; self.user_id = g_user_id; self.port = g_listen_port;
        g_roster.insert(g_roster.begin(), self);
    }
    for (auto& e : g_roster) {
        if (e.user_id == user_id) { e.ip = ip; e.port = port; return; }
    }
    RosterEntry e; e.user_id = user_id; e.ip = ip; e.port = port;
    g_roster.push_back(std::move(e));
}
static void roster_remove(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(g_roster_mutex);
    g_roster.erase(std::remove_if(g_roster.begin(), g_roster.end(),
                                  [&](const RosterEntry& e){ return e.user_id == user_id; }),
                   g_roster.end());
}

static std::string make_network_hello(bool rejoin) {
    // The trailing document id lets the host refuse a joiner that asked for a
    // different document, instead of silently handing over the wrong one.
    return g_access_code + "\n" + g_user_id + "\n" + std::to_string(g_listen_port) + "\n" +
           (rejoin ? "1" : "0") + "\n" + g_doc_id;
}
static bool parse_network_hello(const std::vector<char>& payload, std::string& out_user_id, int& out_port,
                                bool& out_rejoin, std::string& out_doc_id) {
    std::string hello(payload.begin(), payload.end());
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= hello.size()) {
        size_t eol = hello.find('\n', pos);
        if (eol == std::string::npos) { parts.push_back(hello.substr(pos)); break; }
        parts.push_back(hello.substr(pos, eol - pos));
        pos = eol + 1;
    }
    if (parts.empty() || parts[0] != g_access_code) return false;
    out_user_id = (parts.size() > 1 && !parts[1].empty()) ? parts[1] : "(unknown)";
    out_port = (parts.size() > 2) ? std::atoi(parts[2].c_str()) : 0;
    out_rejoin = (parts.size() > 3) && parts[3] == "1";
    out_doc_id = (parts.size() > 4) ? parts[4] : std::string();
    return true;
}
static std::vector<char> read_doc_bytes() {
    std::ifstream file(g_doc_path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}
static bool write_doc_bytes(const std::vector<char>& data) {
    std::ofstream file(g_doc_path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(data.data(), (std::streamsize)data.size());
    return file.good();
}
static bool doc_modified_since(const struct timespec& ts) {
    struct stat st{};
    if (stat(g_doc_path.c_str(), &st) != 0) return false;
    if (st.st_mtim.tv_sec != ts.tv_sec) return st.st_mtim.tv_sec > ts.tv_sec;
    return st.st_mtim.tv_nsec > ts.tv_nsec;
}

/// Consume the host's DOCINFO frame and settle our document scope.
///
/// A joiner that passed --doc has already been validated by the host, so the ids
/// match. A joiner that passed nothing adopts whatever the host is serving, which
/// is what makes "join user1's doc1" work without having to know its id: the copy
/// lands in that document's directory rather than the default one.
static bool adopt_host_document(int fd, std::string& why) {
    uint8_t kind = 0; std::vector<char> payload;
    if (!recv_frame(fd, kind, payload)) { why = "host sent no document information"; return false; }
    if (kind == ERROR) { why = std::string(payload.begin(), payload.end()); return false; }
    if (kind != DOCINFO) { why = "unexpected reply from host during handshake"; return false; }
    std::string blob(payload.begin(), payload.end());
    std::string host_doc = blob;
    size_t nl = blob.find('\n');
    if (nl != std::string::npos) {
        host_doc = blob.substr(0, nl);
        std::string assigned = blob.substr(nl + 1);
        if (!assigned.empty() && assigned != g_user_id) {
            g_user_id = assigned;          // renamed by the host to keep identities unique
            g_base_user_id = assigned;
        }
    }
    if (host_doc == g_doc_id) { g_doc_path = resolve_doc_path(g_user_id); return true; }
    if (!g_doc_id.empty()) {          // the host should have rejected us; belt and braces
        why = "host serves document '" + (host_doc.empty() ? std::string("(default)") : host_doc) +
              "', not '" + g_doc_id + "'";
        return false;
    }
    std::string checked, bad;
    if (!host_doc.empty() && !validate_doc_id(host_doc, checked, bad)) {
        why = "host advertised an unusable document id: " + bad;
        return false;
    }
    g_doc_id = checked;
    if (!g_doc_id.empty()) { mkdir("user_docs", 0755); mkdir(doc_dir().c_str(), 0755); }
    g_doc_path = resolve_doc_path(g_user_id);   // re-file our copy under the host's scope
    return true;
}

/// Read frames off one connection until it dies. Hosts relay UPDATEs to the other joiners.
static void network_read_loop(std::shared_ptr<NetworkClient> client) {
    for (;;) {
        uint8_t kind = 0; std::vector<char> payload;
        if (!recv_frame(client->fd, kind, payload)) break;
        if (kind == ROSTER) { roster_apply(payload); g_force_refresh = true; wake_main_loop(); continue; }
        if (kind != UPDATE) break;
        Update u{};
        if (!deserialize_update(payload.data(), payload.size(), u)) continue;
        enqueue_received_update(Update(u));
        wake_main_loop();
        if (g_network_host) {
            std::lock_guard<std::mutex> lock(g_network_clients_mutex);
            for (const auto& other : g_network_clients) if (other != client) network_send_update(other->fd, u);
        }
    }
}
/// Join the threads of departed joiners. With join_all, wait for every one of them.
/// Threads are moved out under the lock and joined outside it, so a client thread
/// finishing its own cleanup can still take the roster locks.
static void reap_client_threads(bool join_all) {
    std::vector<std::thread> to_join;
    {
        std::lock_guard<std::mutex> lock(g_client_threads_mutex);
        for (auto it = g_client_threads.begin(); it != g_client_threads.end(); ) {
            if (join_all || it->first->finished.load(std::memory_order_acquire)) {
                to_join.push_back(std::move(it->second));
                it = g_client_threads.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& t : to_join) if (t.joinable()) t.join();
}
/// Wake every connected joiner's blocking recv so its thread can wind down.
static void shutdown_client_sockets() {
    std::lock_guard<std::mutex> lock(g_network_clients_mutex);
    for (const auto& client : g_network_clients) {
        if (client->fd >= 0) shutdown(client->fd, SHUT_RDWR);
    }
}

/// Host-side thread for one joiner: pump the connection, then retire it from the roster.
static void network_client_thread(std::shared_ptr<NetworkClient> client) {
    network_read_loop(client);
    // Retire the fd under the roster lock so no broadcaster can pick it up after close().
    int fd = client->fd;
    {
        std::lock_guard<std::mutex> lock(g_network_clients_mutex);
        client->fd = -1;
        g_network_clients.erase(std::remove(g_network_clients.begin(), g_network_clients.end(), client), g_network_clients.end());
    }
    if (fd >= 0) close(fd);
    roster_remove(client->user_id);
    if (!g_stop) { roster_broadcast(); g_force_refresh = true; wake_main_loop(); }
    client->finished.store(true, std::memory_order_release);
}
static void network_host_loop() {
    while (!g_stop) {
        sockaddr_in peer{}; socklen_t peer_len = sizeof(peer);
        int fd = accept(g_listen_fd, (sockaddr*)&peer, &peer_len);
        if (fd < 0) { if (errno == EINTR) continue; if (g_stop) break; usleep(50000); continue; }
        uint8_t kind = 0; std::vector<char> code;
        std::string client_user_id; int client_port = 0; bool rejoin = false; std::string want_doc;
        // Never let one silent client block every other joiner from being accepted.
        set_recv_timeout(fd, g_connect_timeout_ms);
        if (!recv_frame(fd, kind, code) || kind != HELLO ||
            !parse_network_hello(code, client_user_id, client_port, rejoin, want_doc)) {
            const char* msg = "invalid access code"; send_frame(fd, ERROR, msg, std::strlen(msg)); close(fd); continue;
        }
        // A joiner that named a document must be told plainly if this host serves a
        // different one, rather than being handed the wrong document's contents.
        if (!want_doc.empty() && want_doc != g_doc_id) {
            std::string msg = "this host is serving document '" +
                              (g_doc_id.empty() ? std::string("(default)") : g_doc_id) +
                              "', not '" + want_doc + "'";
            send_frame(fd, ERROR, msg.data(), msg.size()); close(fd); continue;
        }
        // Tell the joiner which document it is getting, and under what identity.
        // Identities come from OS accounts and collide easily (the same person on two
        // terminals, or two machines with the same username); echo suppression compares
        // user ids, so a clash would make participants ignore each other's edits.
        {
            std::lock_guard<std::mutex> lock(g_network_clients_mutex);
            std::string base = client_user_id;
            int n = 2;
            auto taken = [&]() {
                if (client_user_id == g_user_id) return true;
                for (const auto& c : g_network_clients) if (c->user_id == client_user_id) return true;
                return false;
            };
            while (taken() && n < 100) client_user_id = base + "-" + std::to_string(n++);
        }
        std::string docinfo = g_doc_id + "\n" + client_user_id;
        if (!send_frame(fd, DOCINFO, docinfo.data(), docinfo.size())) { close(fd); continue; }
        std::vector<char> snapshot = read_doc_bytes();
        if (snapshot.size() > NETWORK_MAX_FRAME || !send_frame(fd, SNAPSHOT, snapshot.data(), snapshot.size())) { close(fd); continue; }
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        set_recv_timeout(fd, 0);   // handshake done; live traffic blocks indefinitely
        auto client = std::make_shared<NetworkClient>(); client->fd = fd; client->user_id = client_user_id;
        { std::lock_guard<std::mutex> lock(g_network_clients_mutex); g_network_clients.push_back(client); }
        roster_upsert(client_user_id, ip, client_port);
        roster_broadcast();
        g_force_refresh = true;
        wake_main_loop();   // repaint now, not at the next idle timeout
        {
            std::lock_guard<std::mutex> lock(g_client_threads_mutex);
            g_client_threads.emplace_back(client, std::thread(network_client_thread, client));
        }
        reap_client_threads(false);
    }
}

/// Take over the session. Our listening socket has been bound since startup, so
/// survivors dialling the address the old host published for us will be served.
static void network_promote_to_host() {
    {
        std::lock_guard<std::mutex> lock(g_conn_mutex);
        if (g_conn_fd >= 0) close(g_conn_fd);
        g_conn_fd = -1;
    }
    {
        std::lock_guard<std::mutex> lock(g_roster_mutex);
        g_roster.erase(std::remove_if(g_roster.begin(), g_roster.end(),
                                      [](const RosterEntry& e){ return e.user_id == g_user_id; }),
                       g_roster.end());
        RosterEntry self; self.user_id = g_user_id; self.port = g_listen_port;
        g_roster.insert(g_roster.begin(), self);
    }
    g_network_host = true;
    g_host_generation.fetch_add(1, std::memory_order_relaxed);
    g_force_refresh = true;
    wake_main_loop();
}
/// Dial a candidate successor and resume as a joiner. Returns true once connected.
static bool network_reconnect_to(const RosterEntry& target) {
    if (target.ip.empty() || target.port <= 0) return false;
    for (int attempt = 0; attempt < RECONNECT_ATTEMPTS && !g_stop; ++attempt) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)target.port);
        if (inet_pton(AF_INET, target.ip.c_str(), &addr.sin_addr) != 1) { close(fd); return false; }
        if (connect_with_timeout(fd, addr, g_connect_timeout_ms) && !socket_connected_to_self(fd)) {
            set_recv_timeout(fd, g_connect_timeout_ms);
            std::string hello = make_network_hello(true);
            std::string ignored;
            uint8_t kind = 0; std::vector<char> snapshot;
            if (send_frame(fd, HELLO, hello.data(), hello.size()) &&
                adopt_host_document(fd, ignored) &&
                recv_frame(fd, kind, snapshot) && kind == SNAPSHOT) {
                // Only adopt the new host's copy if we made no edits while the link was down;
                // otherwise keep ours and let the normal diff/merge path reconcile.
                if (!doc_modified_since(g_link_lost_mtime)) write_doc_bytes(snapshot);
                set_recv_timeout(fd, 0);
                { std::lock_guard<std::mutex> lock(g_conn_mutex); g_conn_fd = fd; }
                g_host_generation.fetch_add(1, std::memory_order_relaxed);
                g_force_refresh = true;
                wake_main_loop();
                return true;
            }
        }
        close(fd);
        usleep(RECONNECT_DELAY_MS * 1000);
    }
    return false;
}
/// The host went away. Walk the succession order: promote if we are next, else
/// dial the next survivor, skipping candidates that never come up.
static bool network_failover() {
    std::vector<RosterEntry> order;
    {
        std::lock_guard<std::mutex> lock(g_roster_mutex);
        if (!g_roster.empty()) g_roster.erase(g_roster.begin());  // drop the dead host
        order = g_roster;
    }
    while (!order.empty() && !g_stop) {
        RosterEntry next = order.front();
        if (next.user_id == g_user_id) { network_promote_to_host(); return true; }
        if (network_reconnect_to(next)) return false;
        order.erase(order.begin());
        roster_remove(next.user_id);
    }
    if (g_stop) return false;
    network_promote_to_host();  // last one standing
    return true;
}
/// Joiner-side supervisor: pump the host link, and on loss run the election.
static void network_supervisor_loop(std::shared_ptr<NetworkClient> host_conn) {
    for (;;) {
        network_read_loop(host_conn);
        host_conn->fd = -1;
        if (g_stop) break;
        struct stat st{};
        if (stat(g_doc_path.c_str(), &st) == 0) g_link_lost_mtime = st.st_mtim;
        if (network_failover()) { network_host_loop(); break; }
        std::lock_guard<std::mutex> lock(g_conn_mutex);
        host_conn->fd = g_conn_fd;
    }
}

/// Bind our listening socket.
///
/// With no port on the command line a host takes the first free port in
/// [g_port_base, g_port_base + g_port_span) and prints it, so nobody has to pick a
/// number that might already be taken. A joiner only needs *some* port to be
/// reachable on for failover, so it always takes an ephemeral one -- that also means
/// it can never collide with, or accidentally dial, a port a host wants.
static bool network_open_listener() {
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) return false;
    int yes = 1; setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY);

    bool bound_ok = false;
    if (!g_network_host) {
        addr.sin_port = g_port_explicit ? htons((uint16_t)g_network_port) : 0;
        bound_ok = bind(g_listen_fd, (sockaddr*)&addr, sizeof(addr)) == 0;
        if (!bound_ok && g_port_explicit) {         // named port busy: any port will do for a joiner
            addr.sin_port = 0;
            bound_ok = bind(g_listen_fd, (sockaddr*)&addr, sizeof(addr)) == 0;
        }
    } else if (g_port_explicit) {
        addr.sin_port = htons((uint16_t)g_network_port);
        bound_ok = bind(g_listen_fd, (sockaddr*)&addr, sizeof(addr)) == 0;
        if (!bound_ok) {
            g_network_error = "Could not listen on port " + std::to_string(g_network_port) + " (" +
                              std::strerror(errno) + "). Is another SyncText host already running?";
            return false;
        }
    } else {
        for (int p = g_port_base; p < g_port_base + g_port_span && !bound_ok; ++p) {
            addr.sin_port = htons((uint16_t)p);
            if (bind(g_listen_fd, (sockaddr*)&addr, sizeof(addr)) == 0) { g_network_port = p; bound_ok = true; }
        }
        if (!bound_ok) {
            g_network_error = "No free port in " + std::to_string(g_port_base) + "-" +
                              std::to_string(g_port_base + g_port_span - 1) +
                              ". Name one explicitly, or set SYNCTEXT_PORT_BASE.";
            return false;
        }
    }
    if (!bound_ok) return false;
    if (listen(g_listen_fd, 16) < 0) return false;
    sockaddr_in bound{}; socklen_t blen = sizeof(bound);
    g_listen_port = (getsockname(g_listen_fd, (sockaddr*)&bound, &blen) == 0)
                        ? ntohs(bound.sin_port) : g_network_port;
    return true;
}
/// Try to complete a handshake with one host:port. On success returns the connected
/// socket (>= 0); on failure returns -1 and leaves the reason in g_network_error.
/// `fatal` is set when retrying other ports would be pointless.
static int try_join_port(const std::string& host, int port, bool& fatal) {
    fatal = false;
    g_network_port = port;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { g_network_error = "Could not create a socket"; fatal = true; return -1; }
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        g_network_error = "'" + host + "' is not a valid IPv4 address (use the host's numeric address, not its name)";
        fatal = true; close(fd); return -1;
    }
    if (!connect_with_timeout(fd, addr, g_connect_timeout_ms)) {
        g_network_error = "Could not reach " + host + ":" + std::to_string(port) + " (" + std::strerror(errno) +
                          "). Check the host is running with --host, that you are on the same network, "
                          "and that the port is open on the host's firewall.";
        close(fd); return -1;
    }
    if (socket_connected_to_self(fd)) {
        g_network_error = "Connected to this machine's own listener on " + host + ":" +
                          std::to_string(port) +
                          ". Point --join at the HOST's address, not this machine's.";
        close(fd); return -1;
    }
    set_recv_timeout(fd, g_connect_timeout_ms);
    std::string hello = make_network_hello(false);
    if (!send_frame(fd, HELLO, hello.data(), hello.size())) {
        g_network_error = "Connected to " + host + " but the handshake could not be sent";
        close(fd); return -1;
    }
    std::string why;
    if (!adopt_host_document(fd, why)) {
        g_network_error = "Host " + host + ":" + std::to_string(port) + ": " + why;
        close(fd); return -1;
    }
    uint8_t kind = 0; std::vector<char> snapshot;
    if (!recv_frame(fd, kind, snapshot)) {
        g_network_error = "Host " + host + ":" + std::to_string(port) +
                          " accepted the connection but sent no reply within " +
                          std::to_string(g_connect_timeout_ms) + "ms";
        close(fd); return -1;
    }
    if (kind == ERROR) {
        std::string reason(snapshot.begin(), snapshot.end());
        g_network_error = "Host rejected the connection: " + (reason.empty() ? std::string("no reason given") : reason);
        close(fd); return -1;
    }
    if (kind != SNAPSHOT) { g_network_error = "Unexpected reply from host during handshake"; close(fd); return -1; }
    if (!write_doc_bytes(snapshot)) {
        g_network_error = "Could not write the received document to " + g_doc_path;
        fatal = true; close(fd); return -1;
    }
    set_recv_timeout(fd, 0);   // live traffic blocks indefinitely again
    return fd;
}

static bool start_network_session(const std::string& host) {
    // Everyone listens from the start: a joiner's socket is idle until it is elected host.
    if (!network_open_listener()) return false;
    if (g_network_host) {
        roster_upsert(g_user_id, "", g_listen_port);
        g_network_thread = std::thread(network_host_loop);
        return true;
    }
    int fd = -1;
    if (g_port_explicit) {
        bool fatal = false;
        fd = try_join_port(host, g_network_port, fatal);
    } else {
        // No port named: walk the range. Ports that answer with the wrong document or a
        // bad code are skipped, so the search lands on the host actually serving what
        // was asked for rather than merely the first SyncText process it meets.
        std::string first_error;
        for (int p = g_port_base; p < g_port_base + g_port_span; ++p) {
            bool fatal = false;
            fd = try_join_port(host, p, fatal);
            if (fd >= 0) break;
            if (fatal) break;
            if (first_error.empty()) first_error = g_network_error;
        }
        if (fd < 0) {
            g_network_error = "No session on " + host + " ports " + std::to_string(g_port_base) + "-" +
                              std::to_string(g_port_base + g_port_span - 1) +
                              " accepted this request. Last reason: " + g_network_error +
                              (first_error.empty() ? "" : " | First: " + first_error);
        }
    }
    if (fd < 0) return false;
    { std::lock_guard<std::mutex> lock(g_conn_mutex); g_conn_fd = fd; }
    auto host_conn = std::make_shared<NetworkClient>();
    host_conn->fd = fd;
    host_conn->user_id = host;
    g_network_thread = std::thread(network_supervisor_loop, host_conn);
    return true;
}
static void stop_network_session() {
    if (!g_network_mode) return;
    g_stop = 1;
    // Unblock accept()/recv() in the network threads; the fds themselves are closed after the join.
    if (g_listen_fd >= 0) shutdown(g_listen_fd, SHUT_RDWR);
    {
        std::lock_guard<std::mutex> lock(g_conn_mutex);
        if (g_conn_fd >= 0) shutdown(g_conn_fd, SHUT_RDWR);
    }
    shutdown_client_sockets();
    if (g_network_thread.joinable()) {
        g_network_thread.join();     // no new joiners can be accepted past this point
    }
    shutdown_client_sockets();       // catch anything accepted during the race window
    reap_client_threads(true);
    {
        std::lock_guard<std::mutex> lock(g_conn_mutex);
        if (g_conn_fd >= 0) close(g_conn_fd);
        g_conn_fd = -1;
    }
    if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
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
    if (g_network_mode) {
        for (size_t k = 0; k < to_send; ++k) {
            if (g_network_host) {
                std::lock_guard<std::mutex> lock(g_network_clients_mutex);
                for (const auto& client : g_network_clients) network_send_update(client->fd, ops[k]);
            } else {
                std::lock_guard<std::mutex> lock(g_conn_mutex);
                network_send_update(g_conn_fd, ops[k]);
            }
            g_tx_sent_total.fetch_add(1, std::memory_order_relaxed);
        }
        return to_send;
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
            std::snprintf(g_registry->users[i].doc_path, DOC_PATH_MAX, "%s", g_doc_path.c_str());
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
        case Update::Type::Cursor: return "cursor";
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
            case Update::Type::Cursor:
                break;  // caret reports never touch the document
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

// ============================================================================
// SECTION 4b: MULTI-DOCUMENT SERVER (--serve)
//
// A --host process *is* an editor, so it can only share the one document it has
// open. --serve is the other shape: a headless process that holds several
// documents and relays between their participants without editing any of them.
// It keeps a replica of each document on disk purely so a late joiner has
// something to be seeded with.
// ============================================================================

struct ServedDoc {
    std::string id;                                        ///< Document id ("" for the default scope).
    std::string path;                                      ///< Replica this server keeps on disk.
    std::mutex mu;                                         ///< Guards clients and the replica file.
    std::vector<std::shared_ptr<NetworkClient>> clients;
};
static std::vector<std::unique_ptr<ServedDoc>> g_served;
static std::mutex g_serve_threads_mu;
static std::vector<std::pair<std::shared_ptr<NetworkClient>, std::thread>> g_serve_threads;

static ServedDoc* served_find(const std::string& id) {
    for (auto& d : g_served) if (d->id == id) return d.get();
    return nullptr;
}
static std::string served_list() {
    std::string out;
    for (size_t i = 0; i < g_served.size(); ++i) {
        if (i) out += ", ";
        out += g_served[i]->id.empty() ? "(default)" : g_served[i]->id;
    }
    return out;
}

/// Roster for one document: this server first, then its participants.
/// Caller holds d->mu.
static std::string served_roster_blob(ServedDoc* d) {
    std::string out = g_user_id + "\t\t" + std::to_string(g_listen_port) + "\n";
    for (const auto& c : d->clients) {
        out += c->user_id + "\t" + c->ip + "\t" + std::to_string(c->port) + "\n";
    }
    return out;
}
static void served_broadcast_roster(ServedDoc* d) {
    std::lock_guard<std::mutex> lock(d->mu);
    std::string blob = served_roster_blob(d);
    for (const auto& c : d->clients) send_frame(c->fd, ROSTER, blob.data(), blob.size());
}

/// Keep the replica current so a joiner arriving later is seeded with something
/// close to the live document. Participants still run the real merge locally; this
/// is a relay's best-effort copy, applied in arrival order.
static void served_apply(ServedDoc* d, const Update& u) {
    auto lines = read_file_lines(d->path);
    apply_update_to_lines(lines, u);
    FILE* f = std::fopen(d->path.c_str(), "wb");
    if (!f) return;
    for (const auto& line : lines) {
        std::fwrite(line.c_str(), 1, line.size(), f);
        std::fwrite("\n", 1, 1, f);
    }
    std::fclose(f);
}

static void served_client_thread(std::shared_ptr<NetworkClient> client) {
    ServedDoc* d = client->doc;
    for (;;) {
        uint8_t kind = 0; std::vector<char> payload;
        if (!recv_frame(client->fd, kind, payload)) break;
        if (kind != UPDATE) continue;
        Update u{};
        if (!deserialize_update(payload.data(), payload.size(), u)) continue;
        std::lock_guard<std::mutex> lock(d->mu);
        if (u.type != Update::Type::Cursor) served_apply(d, u);   // carets are not edits
        for (const auto& other : d->clients) {
            if (other != client) network_send_update(other->fd, u);
        }
    }
    int fd = client->fd;
    {
        std::lock_guard<std::mutex> lock(d->mu);
        client->fd = -1;
        d->clients.erase(std::remove(d->clients.begin(), d->clients.end(), client), d->clients.end());
    }
    if (fd >= 0) close(fd);
    if (!g_stop) {
        served_broadcast_roster(d);
        std::cout << "  - " << client->user_id << " left "
                  << (d->id.empty() ? "(default)" : d->id) << "\n" << std::flush;
    }
    client->finished.store(true, std::memory_order_release);
}

static void serve_reap_threads(bool join_all) {
    std::vector<std::thread> to_join;
    {
        std::lock_guard<std::mutex> lock(g_serve_threads_mu);
        for (auto it = g_serve_threads.begin(); it != g_serve_threads.end(); ) {
            if (join_all || it->first->finished.load(std::memory_order_acquire)) {
                to_join.push_back(std::move(it->second));
                it = g_serve_threads.erase(it);
            } else ++it;
        }
    }
    for (auto& t : to_join) if (t.joinable()) t.join();
}

static void serve_accept_loop() {
    while (!g_stop) {
        sockaddr_in peer{}; socklen_t plen = sizeof(peer);
        int fd = accept(g_listen_fd, (sockaddr*)&peer, &plen);
        if (fd < 0) { if (errno == EINTR) continue; if (g_stop) break; usleep(50000); continue; }

        uint8_t kind = 0; std::vector<char> code;
        std::string uid; int cport = 0; bool rejoin = false; std::string want;
        set_recv_timeout(fd, g_connect_timeout_ms);
        if (!recv_frame(fd, kind, code) || kind != HELLO ||
            !parse_network_hello(code, uid, cport, rejoin, want)) {
            const char* msg = "invalid access code";
            send_frame(fd, ERROR, msg, std::strlen(msg)); close(fd); continue;
        }
        // With several documents on one port the joiner has to say which one it wants;
        // with exactly one there is no ambiguity, so an unnamed joiner gets it.
        ServedDoc* d = nullptr;
        if (want.empty() && g_served.size() == 1) d = g_served[0].get();
        else d = served_find(want);
        if (!d) {
            std::string msg = want.empty()
                ? ("this server holds several documents (" + served_list() + "); name one with --doc")
                : ("this server does not hold document '" + want + "' (it has: " + served_list() + ")");
            send_frame(fd, ERROR, msg.data(), msg.size()); close(fd); continue;
        }
        {
            std::lock_guard<std::mutex> lock(d->mu);
            std::string base = uid;
            int n = 2;
            bool taken = (uid == g_user_id);
            for (const auto& c : d->clients) if (c->user_id == uid) taken = true;
            while (taken && n < 100) {
                uid = base + "-" + std::to_string(n++);
                taken = (uid == g_user_id);
                for (const auto& c : d->clients) if (c->user_id == uid) taken = true;
            }
        }
        std::string docinfo = d->id + "\n" + uid;
        if (!send_frame(fd, DOCINFO, docinfo.data(), docinfo.size())) { close(fd); continue; }

        std::vector<char> snap;
        {
            std::lock_guard<std::mutex> lock(d->mu);
            std::ifstream in(d->path, std::ios::binary);
            snap.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
        if (snap.size() > NETWORK_MAX_FRAME || !send_frame(fd, SNAPSHOT, snap.data(), snap.size())) { close(fd); continue; }
        set_recv_timeout(fd, 0);

        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        auto client = std::make_shared<NetworkClient>();
        client->fd = fd; client->user_id = uid; client->doc = d; client->ip = ip; client->port = cport;
        { std::lock_guard<std::mutex> lock(d->mu); d->clients.push_back(client); }
        served_broadcast_roster(d);
        std::cout << "  + " << uid << " joined " << (d->id.empty() ? "(default)" : d->id)
                  << " from " << ip << "\n" << std::flush;
        {
            std::lock_guard<std::mutex> lock(g_serve_threads_mu);
            g_serve_threads.emplace_back(client, std::thread(served_client_thread, client));
        }
        serve_reap_threads(false);
    }
}

/// Entry point for --serve. Runs until interrupted; there is no editor UI.
static int serve_main() {
    mkdir("user_docs", 0755);
    for (const auto& id : g_doc_ids) {
        auto d = std::make_unique<ServedDoc>();
        d->id = id;
        g_doc_id = id;                      // doc_dir()/resolve_doc_path() read the global
        if (!id.empty()) mkdir(doc_dir().c_str(), 0755);
        d->path = resolve_doc_path(g_user_id);
        struct stat st{};
        if (stat(d->path.c_str(), &st) != 0) write_initial_doc(d->path);
        g_served.push_back(std::move(d));
    }
    g_doc_id.clear();

    g_network_mode = true;
    g_network_host = true;                  // we own a listening socket
    if (!network_open_listener()) {
        std::cerr << (g_network_error.empty() ? "Could not open a listening socket" : g_network_error) << "\n";
        return 2;
    }
    std::cout << "Serving " << g_served.size() << " document(s) on port " << g_network_port;
    if (!g_port_explicit) std::cout << " (chosen automatically)";
    std::cout << "\n";
    for (const auto& d : g_served) {
        std::cout << "  - " << (d->id.empty() ? "(default)" : d->id) << "  ->  " << d->path << "\n";
    }
    std::cout << "  Others join with:  ./build/editor --doc <id> --join <this machine's IP> "
              << g_access_code;
    if (g_port_explicit) std::cout << " " << g_network_port;
    std::cout << "\n  Ctrl-C to stop.\n" << std::flush;

    install_signal_handlers();
    std::thread accept_thread(serve_accept_loop);
    while (!g_stop) usleep(200000);

    if (g_listen_fd >= 0) shutdown(g_listen_fd, SHUT_RDWR);
    for (auto& d : g_served) {
        std::lock_guard<std::mutex> lock(d->mu);
        for (const auto& c : d->clients) if (c->fd >= 0) shutdown(c->fd, SHUT_RDWR);
    }
    if (accept_thread.joinable()) accept_thread.join();
    serve_reap_threads(true);
    if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
    std::cout << "\nServer stopped.\n";
    return 0;
}

static bool stdout_supports_color() {
    if (std::getenv("SYNCTEXT_NO_COLOR")) return false;
    return isatty(STDOUT_FILENO);
}

/// Colour gate for the monitor view, which writes to stdout rather than the editor tty.
static const char* MC(const char* seq) { return stdout_supports_color() ? seq : ""; }

// Shared TUI presentation primitives used by both the monitor renderer and the
// built-in editor renderer below.
struct TuiGlyphs { const char* gutter; const char* caret_row; const char* dot; const char* tilde; const char* trunc; };

#define TUI_BAR       "\033[48;5;238m\033[38;5;253m"
#define TUI_BAR_ACC   "\033[48;5;238m\033[38;5;81m\033[1m"
#define TUI_BAR_DIM   "\033[48;5;238m\033[38;5;245m"
#define TUI_GUTTER    "\033[38;5;242m"
#define TUI_GUTTER_ON "\033[38;5;222m"
#define TUI_CURLINE   "\033[48;5;236m"
#define TUI_TILDE     "\033[38;5;238m"
#define TUI_HINT      "\033[38;5;244m"
#define TUI_KEY       "\033[38;5;81m"
#define TUI_WARN      "\033[38;5;215m"
#define TUI_OFF       "\033[0m"

static void term_size_fd(int fd, int& rows, int& cols);
static TuiGlyphs tui_glyphs();
static std::string pad_bar(const std::string& left, const std::string& right, int cols,
                           const char* bar_col, const char* off_col);

/// The monitor view: what the terminal shows when the editor has a window of its own
/// (and the whole UI when there is no editor at all). Same visual language as the
/// editor -- status bars top and bottom, a dim gutter -- so the two read as one app.
static void render_document(const std::vector<std::string>& lines,
                            const std::vector<Update>& updates,
                            struct timespec last_change_time,
                            bool show_spinner) {
    int rows = 24, cols = 80;
    term_size_fd(STDOUT_FILENO, rows, cols);
    TuiGlyphs gl = tui_glyphs();

    std::vector<char> modified(lines.size(), 0);
    for (const auto& u : updates) {
        if (u.line_no < modified.size()) modified[u.line_no] = 1;
    }

    // Who is here. Also clears stale registry slots as it goes.
    std::vector<std::string> who;
    std::string role;
    if (g_network_mode) {
        std::vector<RosterEntry> snap;
        { std::lock_guard<std::mutex> lock(g_roster_mutex); snap = g_roster; }
        for (size_t i = 0; i < snap.size(); ++i) {
            who.push_back(snap[i].user_id + (i == 0 ? " (host)" : ""));
        }
        if (who.empty()) who.push_back(g_user_id);
        role = g_network_host ? " host" : " joined";
    } else if (g_registry) {
        uint64_t now = now_ns();
        for (size_t i = 0; i < MAX_USERS; ++i) {
            auto &slot = g_registry->users[i];
            if (slot.active.load(std::memory_order_relaxed) != 2) continue;
            uint64_t hb = slot.last_heartbeat_ns.load(std::memory_order_relaxed);
            bool fresh = (now >= hb) && ((now - hb) <= g_stale_ns);
            bool alive = (kill(slot.pid, 0) == 0) || (errno == EPERM);
            if (fresh && alive) {
                who.push_back(slot.user_id);
            } else {
                uint32_t expected = 2;
                slot.active.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
            }
        }
    }

    std::string doc_name = g_doc_path;
    size_t slash = doc_name.find_last_of('/');
    if (slash != std::string::npos) doc_name = doc_name.substr(slash + 1);

    std::string out;
    out.reserve(4096);
    out += "\033[H\033[2J";

    // ---- top bar ----
    std::string left = std::string(" ") + MC(TUI_BAR_ACC) + "SyncText" + MC(TUI_BAR) + "  " + g_user_id +
                       MC(TUI_BAR_DIM) + (role.empty() ? "  monitor" : ("  monitor ·" + role)) + MC(TUI_BAR);
    std::string right = doc_name + MC(TUI_BAR_DIM) + "  " + std::to_string(lines.size()) +
                        (lines.size() == 1 ? " line" : " lines") + " ";
    out += pad_bar(left, right, cols, MC(TUI_BAR), MC(TUI_OFF));
    out += "\n";

    // ---- document ----
    int digits = 1; for (size_t n = lines.size(); n >= 10; n /= 10) ++digits;
    if (digits < 3) digits = 3;
    if (digits > 7) digits = 7;
    size_t shown = lines.size();
    size_t room = (rows > 8) ? (size_t)(rows - 6) : lines.size();
    bool clipped = shown > room;
    if (clipped) shown = room;
    for (size_t i = 0; i < shown; ++i) {
        std::string num = std::to_string(i + 1);
        if ((int)num.size() < digits) num.insert(num.begin(), (size_t)(digits - (int)num.size()), ' ');
        out += MC(TUI_GUTTER); out += " "; out += num; out += " "; out += gl.gutter; out += " ";
        out += MC(TUI_OFF);
        std::string body = lines[i];
        int budget = cols - (digits + 4) - (modified[i] ? 11 : 0);
        if (budget > 0 && (int)body.size() > budget) { body = body.substr(0, (size_t)budget - 1); body += gl.trunc; }
        out += body;
        if (i < modified.size() && modified[i]) { out += MC(TUI_WARN); out += "  modified"; out += MC(TUI_OFF); }
        out += "\n";
    }
    if (clipped) {
        out += MC(TUI_HINT);
        out += " ... " + std::to_string(lines.size() - shown) + " more lines";
        out += MC(TUI_OFF); out += "\n";
    }

    // ---- most recent change, if any ----
    if (!updates.empty()) {
        const Update& u = updates.back();
        out += MC(TUI_HINT);
        out += " " + type_to_string(u.type) + " at line " + std::to_string(u.line_no + 1);
        if (updates.size() > 1) out += "  (+" + std::to_string(updates.size() - 1) + " more)";
        out += MC(TUI_OFF); out += "\n";
    }

    // ---- bottom bar: Active users + last change ----
    std::string users = " " + std::string(MC(TUI_BAR_DIM)) + "Active users " + MC(TUI_BAR);
    if (who.empty()) {
        users += std::string(MC(TUI_BAR_DIM)) + "(none)" + MC(TUI_BAR);
    } else {
        for (size_t i = 0; i < who.size(); ++i) {
            if (i) users += "  ";
            users += std::string(MC(TUI_BAR_ACC)) + gl.dot + MC(TUI_BAR) + " " + who[i];
        }
    }
    std::string stamp = std::string(MC(TUI_BAR_DIM)) + "last change " + MC(TUI_BAR) +
                        time_str_from(last_change_time.tv_sec) + " ";
    out += pad_bar(users, stamp, cols, MC(TUI_BAR), MC(TUI_OFF));
    out += "\n";

    // ---- hint line ----
    auto key = [&](const char* k, const char* what) {
        return std::string(MC(TUI_KEY)) + k + MC(TUI_HINT) + " " + what + "   ";
    };
    out += MC(TUI_HINT); out += " ";
    if (g_hotkeys) {
        out += key("[q]", "quit") + key("[o]", "open editor") +
               key("[s]", "spinner") + key("[p]", "periodic");
    }
    out += MC(TUI_HINT);
    if (show_spinner) {
        static const char spin_chars[] = {'|','/','-','\\'};
        static int spin_idx = 0;
        out += std::string("watching ") + spin_chars[spin_idx++ & 3];
    } else {
        out += "watching";
    }
    out += MC(TUI_OFF);
    out += "\n";

    std::cout << out;
    std::cout.flush();
}

// ============================================================================
// SECTION 5b: BUILT-IN TERMINAL EDITOR (TUI)
//
// An external GUI editor only tells us anything when the user saves, so it can
// never show per-character updates and has nowhere to draw a peer's caret. This
// editor keeps the document in the terminal: every keystroke mutates the shared
// buffer, is diffed immediately, and is broadcast as an Update. Caret positions
// travel the same way, as Update::Type::Cursor records that the merge stage
// never sees.
// ============================================================================

static bool g_tui = false;                   ///< True when the built-in editor is active.
/// The editor draws to, and reads keys from, these descriptors. They default to our
/// own terminal, but when the editor gets its own window they point at that window's
/// tty instead, leaving stdout free for the monitor view.
static int g_tui_in_fd = STDIN_FILENO;
static int g_tui_out_fd = STDOUT_FILENO;
static bool g_edit_window = false;           ///< True when the editor has a window of its own.
static pid_t g_edit_window_pid = -1;         ///< Helper process holding that window open.
static std::string g_edit_window_marker;     ///< File the helper reports its tty path in.
static struct termios g_edit_term_orig{};    ///< Saved settings for the editor tty.
static bool g_edit_term_saved = false;
static size_t g_cur_line = 0;                ///< Local caret line (0-based index into the buffer).
static size_t g_cur_col = 0;                 ///< Local caret column (0-based byte offset in the line).
static int g_view_top = 0;                   ///< First buffer line shown (vertical scroll).
static int g_view_left = 0;                  ///< First column shown (horizontal scroll).
static std::string g_key_pending;            ///< Bytes of a partially-read escape sequence.
static std::string g_tui_status;             ///< Transient message shown in the footer.
static uint64_t g_tui_status_ns = 0;
static uint64_t g_tui_self_write_ns = 0;     ///< When we last wrote the doc ourselves (suppresses self-echo).
static uint64_t g_cursor_sent_ns = 0;        ///< Rate limit for outbound cursor updates.
static size_t g_cursor_sent_line = (size_t)-1;
static size_t g_cursor_sent_col = (size_t)-1;
static constexpr uint64_t CURSOR_MIN_INTERVAL_NS = 60ull * 1000000ull;   ///< At most ~16 cursor msgs/sec.
static constexpr uint64_t CURSOR_STALE_NS = 15ull * 1000000000ull;       ///< Forget a peer caret after 15s.

struct PeerCursor { size_t line = 0; size_t col = 0; uint64_t seen_ns = 0; };
static std::map<std::string, PeerCursor> g_peer_cursors;

static void tui_note(const std::string& msg) { g_tui_status = msg; g_tui_status_ns = now_ns(); }

/// Put stdout in non-blocking mode for the duration of the session.
///
/// A frame is a few KB, and if whatever is on the other end stops reading (a paused
/// terminal, a stalled ssh link, a harness that never drains its pty) a blocking
/// write() parks the main loop indefinitely -- which in a collaborative editor also
/// stops remote edits from being merged. Since every frame is a full repaint, giving
/// up on one we cannot write costs nothing: the next one paints the current state.
static void tui_set_output_nonblocking(bool on) {
    int flags = fcntl(g_tui_out_fd, F_GETFL, 0);
    if (flags < 0) return;
    if (on) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
    (void)fcntl(g_tui_out_fd, F_SETFL, flags);
}

/// Write one frame, tolerating partial writes and abandoning it if the terminal
/// will not accept more. Returns false if the frame was dropped.
static bool tui_write_frame(const std::string& frame) {
    size_t off = 0;
    int stalls = 0;
    while (off < frame.size()) {
        ssize_t n = write(g_tui_out_fd, frame.data() + off, frame.size() - off);
        if (n > 0) { off += (size_t)n; stalls = 0; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++stalls > 20) return false;      // ~20ms of no progress: drop the rest
            usleep(1000);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static void term_size_fd(int fd, int& rows, int& cols) {
    struct winsize ws{};
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        rows = ws.ws_row; cols = ws.ws_col;
    } else { rows = 24; cols = 80; }
}
static void tui_term_size(int& rows, int& cols) { term_size_fd(g_tui_out_fd, rows, cols); }

// ---------------------------------------------------------------------------
// Presentation: palette, glyphs, and the frame itself.
// ---------------------------------------------------------------------------

static bool tui_color() {
    static int cached = -1;
    if (cached < 0) cached = (!std::getenv("SYNCTEXT_NO_COLOR") && isatty(g_tui_out_fd)) ? 1 : 0;
    return cached == 1;
}
static const char* C(const char* seq) { return tui_color() ? seq : ""; }

// Box-drawing and marker glyphs, with an ASCII fallback for terminals or fonts
// that make a mess of them (SYNCTEXT_ASCII=1).
static TuiGlyphs tui_glyphs() {
    static bool ascii = std::getenv("SYNCTEXT_ASCII") != nullptr;
    if (ascii) return {"|", ">", "*", "~", ">"};
    return {"│", "▸", "●", "~", "›"};
}

/// Stable per-user colour index so a peer keeps the same hue across redraws.
static size_t peer_slot(const std::string& user_id) {
    size_t h = 0; for (char c : user_id) h = h * 131 + (unsigned char)c;
    return h % 6;
}
/// Background used to paint the cell a peer's caret sits on.
static const char* peer_caret_bg(const std::string& user_id) {
    static const char* bg[] = {"\033[48;5;170m\033[38;5;16m", "\033[48;5;111m\033[38;5;16m",
                               "\033[48;5;150m\033[38;5;16m", "\033[48;5;209m\033[38;5;16m",
                               "\033[48;5;180m\033[38;5;16m", "\033[48;5;117m\033[38;5;16m"};
    return tui_color() ? bg[peer_slot(user_id)] : "";
}
/// Matching foreground, for the peer's chip in the status bar.
static const char* peer_fg(const std::string& user_id) {
    static const char* fg[] = {"\033[48;5;238m\033[38;5;170m", "\033[48;5;238m\033[38;5;111m",
                               "\033[48;5;238m\033[38;5;150m", "\033[48;5;238m\033[38;5;209m",
                               "\033[48;5;238m\033[38;5;180m", "\033[48;5;238m\033[38;5;117m"};
    return tui_color() ? fg[peer_slot(user_id)] : "";
}

/// Visible width of UTF-8 text: counts characters, not bytes, and skips escapes.
/// Good enough for the glyphs used here, all of which are single-width.
static size_t vis_width(const std::string& t) {
    size_t w = 0;
    for (size_t i = 0; i < t.size(); ) {
        if (t[i] == '\033') { while (i < t.size() && !std::isalpha((unsigned char)t[i])) ++i; if (i < t.size()) ++i; continue; }
        unsigned char c = (unsigned char)t[i];
        i += (c < 0x80) ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
        ++w;
    }
    return w;
}

static void tui_clamp_cursor(const std::vector<std::string>& buf) {
    if (buf.empty()) { g_cur_line = 0; g_cur_col = 0; return; }
    if (g_cur_line >= buf.size()) g_cur_line = buf.size() - 1;
    if (g_cur_col > buf[g_cur_line].size()) g_cur_col = buf[g_cur_line].size();
}

/// Record a peer caret. Called from the rx drain before updates reach the merge stage.
static void tui_note_peer_cursor(const Update& u) {
    if (u.user_id == g_user_id) return;
    PeerCursor pc; pc.line = u.line_no; pc.col = u.col_start; pc.seen_ns = now_ns();
    g_peer_cursors[u.user_id] = pc;
    g_force_refresh = true;
}

/// Tell everyone where our caret is, throttled and only when it actually moved.
static void tui_broadcast_cursor(bool force) {
    if (!g_tui) return;
    uint64_t now = now_ns();
    if (!force && g_cur_line == g_cursor_sent_line && g_cur_col == g_cursor_sent_col) return;
    if (!force && (now - g_cursor_sent_ns) < CURSOR_MIN_INTERVAL_NS) return;
    Update u{};
    u.type = Update::Type::Cursor;
    u.line_no = g_cur_line; u.col_start = g_cur_col; u.col_end = g_cur_col;
    u.timestamp_ns = now; u.user_id = g_user_id;
    std::vector<Update> one{std::move(u)};
    broadcast_updates_to_all(one);
    g_cursor_sent_ns = now; g_cursor_sent_line = g_cur_line; g_cursor_sent_col = g_cur_col;
}

/// Re-announce our caret every few seconds so peers' copies never expire while we idle.
static void tui_cursor_heartbeat() {
    if (!g_tui) return;
    if (now_ns() - g_cursor_sent_ns < 5ull * 1000000000ull) return;
    tui_broadcast_cursor(true);
}

static void tui_write_buffer(const std::vector<std::string>& buf, struct timespec& last_mtime) {
    FILE* f = std::fopen(g_doc_path.c_str(), "wb");
    if (!f) { tui_note("write failed"); return; }
    for (const auto& line : buf) {
        std::fwrite(line.c_str(), 1, line.size(), f);
        std::fwrite("\n", 1, 1, f);
    }
    std::fclose(f);
    struct stat st{};
    if (stat(g_doc_path.c_str(), &st) == 0) last_mtime = st.st_mtim;  // our own write is not a change
    g_tui_self_write_ns = now_ns();
}

/// Pad a status bar to the full terminal width: left-aligned and right-aligned
/// halves, with the bar colour running edge to edge.
static std::string pad_bar(const std::string& left, const std::string& right, int cols,
                           const char* bar_col, const char* off_col) {
    size_t lw = vis_width(left), rw = vis_width(right);
    std::string out = bar_col;
    out += left;
    long gap = (long)cols - (long)lw - (long)rw;
    if (gap < 1) { out += off_col; return out; }   // too narrow for both halves
    out.append((size_t)gap, ' ');
    out += right;
    out += off_col;
    return out;
}
static std::string tui_bar(const std::string& left, const std::string& right, int cols) {
    return pad_bar(left, right, cols, C(TUI_BAR), C(TUI_OFF));
}

static void tui_render(const std::vector<std::string>& buf) {
    int rows = 24, cols = 80;
    tui_term_size(rows, cols);
    TuiGlyphs gl = tui_glyphs();
    const int header = 1, footer = 2;              // top bar; peers bar + hint line
    int view_rows = std::max(1, rows - header - footer);

    tui_clamp_cursor(buf);
    if ((int)g_cur_line < g_view_top) g_view_top = (int)g_cur_line;
    if ((int)g_cur_line >= g_view_top + view_rows) g_view_top = (int)g_cur_line - view_rows + 1;
    if (g_view_top < 0) g_view_top = 0;

    int digits = 1; for (size_t n = buf.size(); n >= 10; n /= 10) ++digits;
    if (digits < 3) digits = 3;
    if (digits > 7) digits = 7;
    const int gutter = digits + 4;                 // " N │ " plus the current-row marker
    int text_cols = std::max(8, cols - gutter);
    if ((int)g_cur_col < g_view_left) g_view_left = (int)g_cur_col;
    if ((int)g_cur_col >= g_view_left + text_cols) g_view_left = (int)g_cur_col - text_cols + 1;
    if (g_view_left < 0) g_view_left = 0;

    uint64_t now = now_ns();
    std::map<size_t, std::vector<std::pair<std::string, size_t>>> carets;
    for (auto it = g_peer_cursors.begin(); it != g_peer_cursors.end(); ) {
        if (now - it->second.seen_ns > CURSOR_STALE_NS) { it = g_peer_cursors.erase(it); continue; }
        carets[it->second.line].push_back({it->first, it->second.col});
        ++it;
    }

    std::string out;
    out.reserve(8192);
    out += "\033[?25l\033[H\033[2J";

    // ---- top bar: who we are, what we are editing ----
    std::string doc_name = g_doc_path;
    size_t slash = doc_name.find_last_of('/');
    if (slash != std::string::npos) doc_name = doc_name.substr(slash + 1);
    std::string left = std::string(" ") + C(TUI_BAR_ACC) + "SyncText" + C(TUI_BAR) + "  " + g_user_id;
    if (g_network_mode) left += std::string(C(TUI_BAR_DIM)) + (g_network_host ? "  host" : "  joined") + C(TUI_BAR);
    std::string right = doc_name + C(TUI_BAR_DIM) + "  " + std::to_string(buf.size()) +
                        (buf.size() == 1 ? " line" : " lines") + " ";
    out += tui_bar(left, right, cols);
    out += "\n";

    // ---- document ----
    for (int r = 0; r < view_rows; ++r) {
        size_t li = (size_t)(g_view_top + r);
        if (li >= buf.size()) {
            out += C(TUI_TILDE); out += gl.tilde; out += C(TUI_OFF); out += "\n";
            continue;
        }
        bool current = (li == g_cur_line);
        if (current) out += C(TUI_CURLINE);

        char num[16];
        std::snprintf(num, sizeof(num), "%*zu", digits, li + 1);   // 1-based, like every editor
        out += current ? C(TUI_GUTTER_ON) : C(TUI_GUTTER);
        out += current ? gl.caret_row : " ";
        out += num;
        out += " "; out += gl.gutter; out += " ";
        out += C(TUI_OFF);
        if (current) out += C(TUI_CURLINE);

        const std::string& line = buf[li];
        std::string visible = (g_view_left < (int)line.size())
                                ? line.substr((size_t)g_view_left, (size_t)text_cols) : std::string();
        bool truncated = (int)line.size() > g_view_left + text_cols;

        std::map<size_t, std::string> marks;
        auto found = carets.find(li);
        if (found != carets.end()) {
            for (const auto& c : found->second) {
                if ((int)c.second < g_view_left) continue;
                size_t vis_col = c.second - (size_t)g_view_left;
                if ((int)vis_col > text_cols) continue;
                marks[vis_col] = c.first;
            }
        }
        for (size_t i = 0; i <= visible.size(); ++i) {
            auto mk = marks.find(i);
            if (mk != marks.end()) {
                out += peer_caret_bg(mk->second);
                out += (i < visible.size()) ? visible[i] : ' ';
                out += C(TUI_OFF);
                if (current) out += C(TUI_CURLINE);
            } else if (i < visible.size()) {
                out += visible[i];
            }
        }
        if (truncated) { out += C(TUI_HINT); out += gl.trunc; out += C(TUI_OFF); }
        if (current) {
            // Extend the highlight to the right edge so the active line reads as a band.
            long pad = (long)cols - (long)gutter - (long)vis_width(visible) - (truncated ? 1 : 0);
            if (pad > 0) out.append((size_t)pad, ' ');
            out += C(TUI_OFF);
        }
        out += "\n";
    }

    // ---- peers bar ----
    std::string peers;
    if (g_peer_cursors.empty()) {
        peers = std::string(" ") + C(TUI_BAR_DIM) + "no peers connected" + C(TUI_BAR);
    } else {
        peers = " ";
        bool first = true;
        for (const auto& kv : g_peer_cursors) {
            if (!first) peers += "  ";
            first = false;
            peers += peer_fg(kv.first);
            peers += gl.dot;
            peers += C(TUI_BAR);
            peers += " " + kv.first + C(TUI_BAR_DIM) + " " +
                     std::to_string(kv.second.line + 1) + ":" + std::to_string(kv.second.col + 1) +
                     C(TUI_BAR);
        }
    }
    std::string pos = std::string(C(TUI_BAR_DIM)) + "Ln " + C(TUI_BAR) + std::to_string(g_cur_line + 1) +
                      C(TUI_BAR_DIM) + "  Col " + C(TUI_BAR) + std::to_string(g_cur_col + 1) + " ";
    out += tui_bar(peers, pos, cols);
    out += "\n";

    // ---- hint line ----
    auto key = [&](const char* k, const char* what) {
        return std::string(C(TUI_KEY)) + k + C(TUI_HINT) + " " + what + "   ";
    };
    std::string hints = std::string(C(TUI_HINT)) + " " + key("^Q", "quit") + key("^S", "flush") +
                        key("^L", "redraw") + C(TUI_HINT) + "arrows/Home/End/PgUp/PgDn move";
    if (!g_tui_status.empty()) {
        if (now - g_tui_status_ns < 4000000000ull) {
            hints += std::string("   ") + C(TUI_WARN) + g_tui_status;
        } else {
            g_tui_status.clear();
        }
    }
    hints += C(TUI_OFF);
    out += hints;

    // Park the real caret where the user is typing, then show it again.
    int screen_row = header + 1 + ((int)g_cur_line - g_view_top);
    int screen_col = gutter + 1 + ((int)g_cur_col - g_view_left);
    char park[32];
    std::snprintf(park, sizeof(park), "\033[%d;%dH", screen_row, screen_col);
    out += park;
    out += "\033[?25h";
    if (!tui_write_frame(out)) g_force_refresh = true;   // dropped: repaint next time round
}

enum class TuiKey { None, Char, Up, Down, Left, Right, Home, End, PgUp, PgDn, Enter, Backspace, DelFwd, Quit, Flush, Redraw };

static uint64_t g_esc_seen_ns = 0;   ///< When the current incomplete escape sequence first appeared.
static constexpr uint64_t ESC_TIMEOUT_NS = 40ull * 1000000ull;  ///< Give up on a partial sequence after 40ms.
static constexpr size_t ESC_MAX_LEN = 32;                       ///< Longest escape sequence we will buffer.

/// Map a decoded CSI sequence to a key. `params` is what sat between "\033[" and the
/// final byte; `final` is the terminating character.
static TuiKey tui_decode_csi(const std::string& params, char final_byte) {
    switch (final_byte) {
        case 'A': return TuiKey::Up;
        case 'B': return TuiKey::Down;
        case 'C': return TuiKey::Right;
        case 'D': return TuiKey::Left;
        case 'H': return TuiKey::Home;
        case 'F': return TuiKey::End;
        case '~':
            if (params == "1" || params == "7") return TuiKey::Home;
            if (params == "4" || params == "8") return TuiKey::End;
            if (params == "3") return TuiKey::DelFwd;
            if (params == "5") return TuiKey::PgUp;
            if (params == "6") return TuiKey::PgDn;
            return TuiKey::None;
        default: return TuiKey::None;   // consumed, but nothing we act on
    }
}

/// Pull one key out of g_key_pending.
///
/// Escape sequences arrive split across reads, so an incomplete one has to stay
/// buffered -- but it must never stay buffered *forever*, or a truncated sequence
/// swallows every keystroke that follows it (including ^Q). Two rules keep that
/// from happening: sequences are parsed by the real CSI grammar rather than a
/// fixed length, and an incomplete one is abandoned after ESC_TIMEOUT_NS, with
/// only the ESC byte dropped so the rest is re-read as ordinary input.
///
/// Returns None either when nothing could be decoded yet (buffer unchanged: wait
/// for more bytes) or when bytes were consumed but map to no action. The caller
/// distinguishes the two by watching whether the buffer shrank.
static TuiKey tui_next_key(char& out_ch) {
    if (g_key_pending.empty()) { g_esc_seen_ns = 0; return TuiKey::None; }
    unsigned char c = (unsigned char)g_key_pending[0];

    if (c == 0x1b) {
        if (g_esc_seen_ns == 0) g_esc_seen_ns = now_ns();
        bool expired = (now_ns() - g_esc_seen_ns) > ESC_TIMEOUT_NS;
        auto abandon = [&]() { g_key_pending.erase(0, 1); g_esc_seen_ns = 0; };

        if (g_key_pending.size() < 2) { if (expired) abandon(); return TuiKey::None; }
        char b1 = g_key_pending[1];
        if (b1 != '[' && b1 != 'O') {          // ESC + key (Alt-modified); consume and ignore
            g_key_pending.erase(0, 2); g_esc_seen_ns = 0; return TuiKey::None;
        }
        // CSI: parameter bytes 0x30-0x3F, intermediates 0x20-0x2F, final byte 0x40-0x7E.
        size_t i = 2;
        while (i < g_key_pending.size()) {
            unsigned char b = (unsigned char)g_key_pending[i];
            if (b >= 0x40 && b <= 0x7e) break;
            if (!((b >= 0x30 && b <= 0x3f) || (b >= 0x20 && b <= 0x2f))) break;  // malformed
            ++i;
        }
        if (i >= g_key_pending.size()) {        // still waiting on the final byte
            if (expired || g_key_pending.size() > ESC_MAX_LEN) abandon();
            return TuiKey::None;
        }
        unsigned char fb = (unsigned char)g_key_pending[i];
        if (fb < 0x40 || fb > 0x7e) { abandon(); return TuiKey::None; }  // malformed: drop the ESC only
        std::string params = g_key_pending.substr(2, i - 2);
        g_key_pending.erase(0, i + 1);
        g_esc_seen_ns = 0;
        return tui_decode_csi(params, (char)fb);
    }

    g_esc_seen_ns = 0;
    g_key_pending.erase(0, 1);
    if (c == 0x11) return TuiKey::Quit;      // ^Q
    if (c == 0x13) return TuiKey::Flush;     // ^S
    if (c == 0x0c) return TuiKey::Redraw;    // ^L
    if (c == 0x7f || c == 0x08) return TuiKey::Backspace;
    if (c == '\r' || c == '\n') return TuiKey::Enter;
    if (c == '\t') { out_ch = '\t'; return TuiKey::Char; }
    if (c < 0x20) return TuiKey::None;       // other control bytes are ignored
    out_ch = (char)c;
    return TuiKey::Char;
}

/// Apply pending keystrokes to the buffer. Returns true if the text changed.
static bool tui_handle_keys(std::vector<std::string>& buf, bool& out_moved) {
    char rbuf[256];
    ssize_t n = read(g_tui_in_fd, rbuf, sizeof(rbuf));
    if (n > 0) g_key_pending.append(rbuf, (size_t)n);
    if (g_key_pending.empty()) return false;
    if (buf.empty()) buf.push_back("");

    bool edited = false;
    out_moved = false;
    for (;;) {
        char ch = 0;
        size_t before_bytes = g_key_pending.size();
        TuiKey k = tui_next_key(ch);
        if (k == TuiKey::None) {
            if (g_key_pending.empty()) break;
            if (g_key_pending.size() == before_bytes) break;  // incomplete: wait for more bytes
            continue;                                          // consumed, but nothing to do
        }
        tui_clamp_cursor(buf);
        switch (k) {
            case TuiKey::Quit: g_stop = 1; return edited;
            case TuiKey::Flush: tui_note("flushed"); out_moved = true; break;
            case TuiKey::Redraw: g_force_refresh = true; break;
            case TuiKey::Up:    if (g_cur_line > 0) { --g_cur_line; out_moved = true; } break;
            case TuiKey::Down:  if (g_cur_line + 1 < buf.size()) { ++g_cur_line; out_moved = true; } break;
            case TuiKey::Left:
                if (g_cur_col > 0) { --g_cur_col; }
                else if (g_cur_line > 0) { --g_cur_line; g_cur_col = buf[g_cur_line].size(); }
                out_moved = true; break;
            case TuiKey::Right:
                if (g_cur_col < buf[g_cur_line].size()) { ++g_cur_col; }
                else if (g_cur_line + 1 < buf.size()) { ++g_cur_line; g_cur_col = 0; }
                out_moved = true; break;
            case TuiKey::Home: g_cur_col = 0; out_moved = true; break;
            case TuiKey::End:  g_cur_col = buf[g_cur_line].size(); out_moved = true; break;
            case TuiKey::PgUp: g_cur_line = (g_cur_line > 10) ? g_cur_line - 10 : 0; out_moved = true; break;
            case TuiKey::PgDn: g_cur_line = std::min(buf.size() - 1, g_cur_line + 10); out_moved = true; break;
            case TuiKey::Char:
                buf[g_cur_line].insert(g_cur_col, 1, ch);
                ++g_cur_col; edited = true; break;
            case TuiKey::Enter: {
                std::string tail = buf[g_cur_line].substr(g_cur_col);
                buf[g_cur_line].erase(g_cur_col);
                buf.insert(buf.begin() + (long)g_cur_line + 1, tail);
                ++g_cur_line; g_cur_col = 0; edited = true; break;
            }
            case TuiKey::Backspace:
                if (g_cur_col > 0) {
                    buf[g_cur_line].erase(g_cur_col - 1, 1);
                    --g_cur_col; edited = true;
                } else if (g_cur_line > 0) {
                    size_t join_at = buf[g_cur_line - 1].size();
                    buf[g_cur_line - 1] += buf[g_cur_line];
                    buf.erase(buf.begin() + (long)g_cur_line);
                    --g_cur_line; g_cur_col = join_at; edited = true;
                }
                break;
            case TuiKey::DelFwd:
                if (g_cur_col < buf[g_cur_line].size()) {
                    buf[g_cur_line].erase(g_cur_col, 1); edited = true;
                } else if (g_cur_line + 1 < buf.size()) {
                    buf[g_cur_line] += buf[g_cur_line + 1];
                    buf.erase(buf.begin() + (long)g_cur_line + 1); edited = true;
                }
                break;
            default: break;
        }
        if (g_key_pending.empty()) break;
    }
    if (buf.empty()) buf.push_back("");
    tui_clamp_cursor(buf);
    return edited;
}

// ----------------------------------------------------------------------------
// Dedicated editor window
//
// The monitor view and the editor want the same terminal, so give the editor one
// of its own. Rather than run a second process (which would need a second registry
// slot and message queue for what is one participant), we keep a single process and
// simply point the editor's I/O at another tty: a helper is launched in a new
// terminal window, reports its tty path, and then parks. We open that tty, put it in
// raw mode, and draw there, while stdout keeps showing the monitor.
//
// SYNCTEXT_EDIT_TTY=/dev/pts/N adopts an existing terminal instead of opening one,
// which is how you pair this with tmux, a second ssh session, or a terminal you
// already have open (run `tty` in it to get the path).
// ----------------------------------------------------------------------------

static bool edit_tty_attach(const std::string& tty_path) {
    int fd = open(tty_path.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) return false;
    if (!isatty(fd)) { close(fd); return false; }
    if (tcgetattr(fd, &g_edit_term_orig) == 0) g_edit_term_saved = true;
    struct termios raw = g_edit_term_orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_iflag &= ~(IXON | ICRNL);   // ^S/^Q are editor keys, and Return must stay \r
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    (void)tcsetattr(fd, TCSANOW, &raw);
    g_tui_in_fd = fd;
    g_tui_out_fd = fd;
    g_edit_window = true;
    return true;
}

static void edit_tty_release() {
    if (!g_edit_window) return;
    if (g_edit_term_saved) tcsetattr(g_tui_out_fd, TCSANOW, &g_edit_term_orig);
    if (g_tui_out_fd >= 0 && g_tui_out_fd != STDOUT_FILENO) close(g_tui_out_fd);
    g_tui_in_fd = STDIN_FILENO;
    g_tui_out_fd = STDOUT_FILENO;
    g_edit_window = false;
    if (g_edit_window_pid > 0) { kill(g_edit_window_pid, SIGTERM); g_edit_window_pid = -1; }
    if (!g_edit_window_marker.empty()) { unlink(g_edit_window_marker.c_str()); g_edit_window_marker.clear(); }
}

/// Terminal emulators disagree about how to be told "run this command", so try the
/// common spellings in turn. Override the whole thing with SYNCTEXT_TERMINAL.
static bool spawn_edit_terminal(const std::string& helper_cmd) {
    if (const char* forced = std::getenv("SYNCTEXT_TERMINAL")) {
        if (*forced) return spawn_bg_shell(std::string(forced) + " " + helper_cmd);
    }
    struct { const char* prog; const char* flag; } candidates[] = {
        {"gnome-terminal",   "--"},
        {"kgx",              "--"},
        {"x-terminal-emulator", "-e"},
        {"konsole",          "-e"},
        {"xfce4-terminal",   "-x"},
        {"mate-terminal",    "--"},
        {"alacritty",        "-e"},
        {"kitty",            ""},
        {"xterm",            "-e"},
    };
    for (const auto& c : candidates) {
        std::string probe = std::string("command -v ") + c.prog + " >/dev/null 2>&1";
        if (std::system(probe.c_str()) != 0) continue;
        std::string cmd = std::string(c.prog) + " " + c.flag + " " +
                          "sh -c " + "'" + helper_cmd + "'" + " >/dev/null 2>&1 &";
        if (spawn_bg_shell(cmd)) return true;
    }
    return false;
}

/// Open a window for the editor and attach to it. Returns false if we could not,
/// in which case the caller keeps the editor in the current terminal.
static bool open_edit_window() {
    // An explicitly named terminal always wins: SYNCTEXT_EDIT_WINDOW governs whether
    // we may *open* a window, not whether we may use one the user handed us.
    if (const char* adopt = std::getenv("SYNCTEXT_EDIT_TTY")) {
        if (*adopt) return edit_tty_attach(adopt);
    }
    if (!parse_env_bool("SYNCTEXT_EDIT_WINDOW", true)) return false;
    bool has_display = (std::getenv("DISPLAY") && *std::getenv("DISPLAY")) ||
                       (std::getenv("WAYLAND_DISPLAY") && *std::getenv("WAYLAND_DISPLAY"));
    if (!has_display) return false;

    g_edit_window_marker = std::string("log_files/.") + g_user_id + "_edit_tty";
    unlink(g_edit_window_marker.c_str());
    // The helper publishes its tty, prints a note, then parks until we terminate it.
    std::string helper =
        "tty > \"" + g_edit_window_marker + "\"; "
        "printf \"SyncText editor window for " + g_user_id + " - do not close until you quit SyncText\\n\"; "
        "while :; do sleep 3600; done";
    if (!spawn_edit_terminal(helper)) { g_edit_window_marker.clear(); return false; }

    // Wait briefly for the window to come up and report its tty.
    for (int i = 0; i < 60; ++i) {
        std::ifstream f(g_edit_window_marker);
        std::string path;
        if (f && std::getline(f, path) && !path.empty() && path[0] == '/') {
            if (edit_tty_attach(path)) return true;
        }
        usleep(100000);
    }
    g_edit_window_marker.clear();
    return false;
}

/// One editing turn: consume keystrokes, publish whatever changed, keep the file current.
/// Returns true if the screen needs repainting.
static bool tui_step(std::vector<std::string>& buf, struct timespec& last_mtime) {
    bool moved = false;
    std::vector<std::string> before = buf;
    bool edited = tui_handle_keys(buf, moved);
    if (edited) {
        auto updates = diff_documents(before, buf);
        if (!updates.empty()) {
            g_last_local_change_ns.store(now_ns(), std::memory_order_relaxed);
            for (const auto& u : updates) {
                if (u.type == Update::Type::LineDelete || u.type == Update::Type::BlockDelete) {
                    g_last_local_delete_ns.store(now_ns(), std::memory_order_relaxed);
                    break;
                }
            }
            for (auto& u : updates) { g_tx_pending.push_back(u); g_local_since_merge.push_back(u); }
            // Per-keystroke latency matters more here than batching, so flush straight away.
            while (!g_tx_pending.empty()) {
                size_t sent = broadcast_updates_to_all(g_tx_pending);
                if (sent == 0) break;
                g_tx_pending.erase(g_tx_pending.begin(), g_tx_pending.begin() + (long)sent);
            }
            update_local_line_timestamps_after(updates);
        }
        tui_write_buffer(buf, last_mtime);
    }
    if (edited || moved) tui_broadcast_cursor(false);
    return edited || moved;
}

/// Draw with whichever front-end is active.
static void render_view(const std::vector<std::string>& lines,
                        const std::vector<Update>& updates,
                        struct timespec last_change_time,
                        bool show_spinner) {
    if (g_tui) {
        tui_render(lines);
        // With a window of its own, the editor no longer occupies this terminal,
        // so the original one goes back to showing the monitor view.
        if (g_edit_window) render_document(lines, updates, last_change_time, show_spinner);
    } else {
        render_document(lines, updates, last_change_time, show_spinner);
    }
}

/// Where a peer keeps its document. Peers publish an absolute path precisely because
/// each session resolves its own document relative to its working directory, and two
/// terminals rarely share one; the relative form is only a fallback for a slot
/// registered by an older build.
static std::string peer_doc_path(const UserEntry& slot) {
    if (slot.doc_path[0] != '\0') {
        return std::string(slot.doc_path, strnlen(slot.doc_path, DOC_PATH_MAX));
    }
    return doc_path_for(std::string(slot.user_id));
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
        std::string peer_path = peer_doc_path(slot);
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
            std::string peer_path = peer_doc_path(slot);
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
    if (g_tui) {
        // The tty eats ^S/^Q as XON/XOFF flow control, and ICRNL rewrites Return.
        // The built-in editor needs all three as ordinary keystrokes.
        raw.c_iflag &= ~(IXON | ICRNL);
    }
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
            if (!g_tui) open_local_editor_if_possible();
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
 *   argv[1]: document id (required) - Opens or creates user_docs/<doc>/<user>_doc.txt
 *   --host <doc> <code> - Opens a document and shares it over LAN
 *   --join <host_ip> <code> - Joins the document selected by that access code
 * 
 * Example:
 *   ./build/editor notes
 *   ./build/editor --host notes hunter2
 *   ./build/editor --join 192.168.1.25 hunter2
 */
int run_main(int argc, char** argv) {
    // Pull "--doc <id>" out wherever it appears, so every positional form below is
    // parsed exactly as before. SYNCTEXT_DOC does the same job for the environment.
    std::vector<char*> args;
    for (int i = 0; i < argc; ++i) {
        if (std::string(argv[i]) == "--doc") {
            if (i + 1 >= argc) { std::cerr << "--doc needs a document id\n"; return 1; }
            std::string checked, why;
            if (!validate_doc_id(argv[++i], checked, why)) {
                std::cerr << "Invalid --doc value: " << why << "\n";
                return 1;
            }
            g_doc_ids.push_back(checked);
            continue;
        }
        args.push_back(argv[i]);
    }
    argc = (int)args.size();
    argv = args.data();

    // --serve is the one mode that takes several documents, so it is decided before
    // the single-document rule below is applied.
    bool is_serve = argc >= 2 && std::string(argv[1]) == "--serve";
    if (!is_serve) {
        if (g_doc_ids.size() > 1) {
            std::cerr << "Only one --doc is meaningful outside --serve (got " << g_doc_ids.size() << ")\n";
            return 1;
        }
        if (!g_doc_ids.empty()) g_doc_id = g_doc_ids.front();
        if (g_doc_id.empty()) {
            if (const char* env_doc = std::getenv("SYNCTEXT_DOC")) {
                if (*env_doc) {
                    std::string why;
                    if (!validate_doc_id(env_doc, g_doc_id, why)) {
                        std::cerr << "Invalid SYNCTEXT_DOC value: " << why << "\n";
                        return 1;
                    }
                }
            }
        }
    }

    if (is_serve) {
        // Advanced: hold several documents on one port without editing any of them.
        //   ./build/editor --serve <access_code> --doc <id> [--doc <id> ...]
        if (argc != 3) {
            std::cerr << "Usage: " << argv[0] << " --serve <access_code> --doc <id> [--doc <id> ...]\n";
            return 1;
        }
        g_serve_mode = true;
        g_user_id = default_user_id();
        g_access_code = argv[2];
        if (g_access_code.empty() || g_access_code.size() > 128) { std::cerr << "Invalid access code\n"; return 1; }
        g_port_base = parse_env_millis("SYNCTEXT_PORT_BASE", g_port_base);
        g_port_span = parse_env_millis("SYNCTEXT_PORT_SPAN", g_port_span);
        if (g_port_base < 1 || g_port_base > 65000) g_port_base = 9000;
        if (g_port_span < 1 || g_port_span > 64) g_port_span = 16;
        g_connect_timeout_ms = parse_env_millis("SYNCTEXT_CONNECT_TIMEOUT_MS", g_connect_timeout_ms);
        if (g_connect_timeout_ms < 200) g_connect_timeout_ms = 200;
        if (g_doc_ids.empty()) {
            std::cerr << "--serve needs at least one --doc <id> to hold\n";
            return 1;
        }
        std::sort(g_doc_ids.begin(), g_doc_ids.end());
        g_doc_ids.erase(std::unique(g_doc_ids.begin(), g_doc_ids.end()), g_doc_ids.end());
        mkdir("log_files", 0755);
        return serve_main();
    }

    bool is_host = argc >= 2 && std::string(argv[1]) == "--host";
    bool is_join = argc >= 2 && std::string(argv[1]) == "--join";
    //   editor <doc>                  open a document on this machine
    //   editor --host <doc> <code>    open it and share it
    //   editor --join <ip>   <code>   join whatever document that code opens
    // No user id (taken from the OS account) and no port (chosen automatically).
    if ((!is_host && !is_join && argc != 2) ||
        (is_host && argc != 4) || (is_join && argc != 4)) {
        std::cerr << "Usage: " << argv[0] << " <doc>                     open a document\n";
        std::cerr << "       " << argv[0] << " --host <doc> <code>       open it and share it\n";
        std::cerr << "       " << argv[0] << " --join <host_ip> <code>   join a shared document\n";
        std::cerr << "\n";
        std::cerr << "  <doc>   name of the document, e.g. notes  (files in user_docs/<doc>/)\n";
        std::cerr << "  <code>  shared password for that document; it is also what a joiner\n";
        std::cerr << "          presents, so each shared document wants its own code\n";
        std::cerr << "\n";
        std::cerr << "Examples:\n";
        std::cerr << "  " << argv[0] << " notes                       # work on it locally\n";
        std::cerr << "  " << argv[0] << " --host notes hunter2        # share it, prints how to join\n";
        std::cerr << "  " << argv[0] << " --join 192.168.1.25 hunter2 # join from another machine\n";
        return 1;
    }

    mkdir("user_docs", 0755);
    mkdir("log_files", 0755);

    g_user_id = default_user_id();
    g_base_user_id = g_user_id;
    { const char* e = std::getenv("SYNCTEXT_USER"); g_user_id_explicit = (e && *e); }
    g_port_base = parse_env_millis("SYNCTEXT_PORT_BASE", g_port_base);
    g_port_span = parse_env_millis("SYNCTEXT_PORT_SPAN", g_port_span);
    if (g_port_base < 1 || g_port_base > 65000) g_port_base = 9000;
    if (g_port_span < 1 || g_port_span > 64) g_port_span = 16;

    std::string network_host;
    if (is_join) {
        // The document comes from whoever answers: the access code selects it.
        g_network_mode = true; g_network_host = false;
        network_host = argv[2];
        g_access_code = argv[3];
    } else {
        std::string why;
        if (!validate_doc_id(argv[is_host ? 2 : 1], g_doc_id, why)) {
            std::cerr << "Invalid document name: " << why << "\n";
            return 1;
        }
        mkdir(doc_dir().c_str(), 0755);
        if (is_host) {
            g_network_mode = true; g_network_host = true;
            g_access_code = argv[3];
        }
    }
    if (g_network_mode && (g_access_code.empty() || g_access_code.size() > 128)) {
        std::cerr << "Invalid access code\n";
        return 1;
    }

    
    // Resolve the document to an absolute path. Sessions are routinely started from
    // different working directories (a second terminal rarely opens in the first
    // one's), and peers have to be able to find each other's documents to seed a
    // newly joined session.
    g_doc_path = resolve_doc_path(g_user_id);
    g_queue_name = make_queue_name(g_user_id);

    install_signal_handlers();
    init_wake_fd();
    g_rx_ring.resize(RX_RING_CAP);
    if (g_network_mode) {
        if (g_network_host) {
            struct stat st{};
            if (stat(g_doc_path.c_str(), &st) != 0) write_initial_doc(g_doc_path);
        }
        g_connect_timeout_ms = parse_env_millis("SYNCTEXT_CONNECT_TIMEOUT_MS", g_connect_timeout_ms);
        if (g_connect_timeout_ms < 200) g_connect_timeout_ms = 200;
        if (!start_network_session(network_host)) {
            if (!g_network_error.empty()) std::cerr << g_network_error << "\n";
            else std::cerr << "Unable to establish network session on port " << g_network_port << "\n";
            if (g_listen_fd >= 0) close(g_listen_fd);
            return 2;
        }
        if (g_network_host) {
            std::cout << "Hosting on port " << g_network_port;
            if (!g_port_explicit) std::cout << " (chosen automatically)";
            std::cout << "\n";
            char hbuf[256];
            std::string addr_hint = (gethostname(hbuf, sizeof(hbuf)) == 0) ? std::string(hbuf) : std::string("<this machine>");
            std::cout << "  Others join with:  ./build/editor --join <this machine's IP> "
                      << g_access_code;
            if (g_port_explicit) std::cout << " " << g_network_port;
            std::cout << "\n";
            if (!g_port_explicit) {
                std::cout << "  (no port needed - joiners scan " << g_port_base << "-"
                          << (g_port_base + g_port_span - 1) << "; find the IP with: hostname -I)\n";
            }
        } else {
            std::cout << "Joined session on port " << g_network_port << "\n";
        }
    } else {
        init_registry();
    }
    // Registration first: it is what settles our final identity (a second session of
    // the same account is renamed), and the queue name is derived from that. Opening
    // the queue any earlier would leave the listener on a name nobody publishes.
    // Registration with optional interactive prompt on duplicate session (default on when TTY)
    g_interactive_login = isatty(STDIN_FILENO);
    if (std::getenv("SYNCTEXT_INTERACTIVE_LOGIN")) {
        g_interactive_login = parse_env_bool("SYNCTEXT_INTERACTIVE_LOGIN", true) && isatty(STDIN_FILENO);
    }
    bool force_reclaim_env = parse_env_bool("SYNCTEXT_FORCE_RECLAIM", false);
    for (;;) {
        if (g_network_mode) break;
        RegisterResult rr = register_user_try(g_user_id, force_reclaim_env);
        if (rr == RegisterResult::Ok) break;
        if (rr == RegisterResult::NoSlots) {
            std::cerr << "No free user slots available (max " << MAX_USERS << ")\n";
            return 2;
        }
        // DuplicateActive. The user never chose this id -- it came from their OS
        // account -- so opening the same document twice should just work: take the
        // next free suffix instead of asking them to invent a name.
        if (!g_user_id_explicit) {
            bool renamed = false;
            for (int suffix = 2; suffix < 2 + (int)MAX_USERS + 1; ++suffix) {
                std::string candidate = g_base_user_id + "-" + std::to_string(suffix);
                if (candidate.size() >= USER_ID_MAX) break;
                if (register_user_try(candidate, false) == RegisterResult::Ok) {
                    g_user_id = candidate;
                    g_doc_path = resolve_doc_path(g_user_id);
                    g_queue_name = make_queue_name(g_user_id);
                    renamed = true;
                    break;
                }
            }
            if (renamed) break;
        }
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

    // Create/open our own message queue for receiving updates
    if (!g_network_mode) {
        open_message_queue(g_queue_name);
        if (g_mq != (mqd_t)-1) {
            std::cout << "Opened receive queue: " << g_queue_name << "\n";
            g_listener_thread = std::thread(listener_loop);
        }
    }

    bool created = false;
    if (g_network_mode) {
        created = false;
    } else {
        created = ensure_local_doc();
    }
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
    // Built-in editor: on by default in a real terminal, off when piped/redirected
    // (which is what keeps the file-driven test suite on the external-editor path).
    g_tui = parse_env_bool("SYNCTEXT_TUI", isatty(STDIN_FILENO) && isatty(STDOUT_FILENO));
    if (g_tui) {
        g_hotkeys = true;                        // the editor needs the keyboard
        if (open_edit_window()) {
            std::cout << "Editing in a separate window (" << (g_edit_window_marker.empty()
                          ? "adopted terminal" : "new terminal") << "). This window shows status.\n";
        }
        tui_set_output_nonblocking(true);
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

    // Open the local editor (best-effort, non-blocking). Skipped when we are the editor.
    if (!g_tui) open_local_editor_if_possible();

    // Initialize receive ring
    g_rx_local.clear();
    // Initial render
    if (g_tui) { tui_clamp_cursor(prev_lines); tui_broadcast_cursor(true); }
    render_view(prev_lines, {}, last_mtime, g_spinner);

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
        struct pollfd pfds[4];
        int nfds = 0;
        if (ino.fd >= 0) { pfds[nfds++] = {ino.fd, POLLIN, 0}; }
        if (g_hotkeys && isatty(STDIN_FILENO)) { pfds[nfds++] = {STDIN_FILENO, POLLIN, 0}; }
        if (g_edit_window && g_tui_in_fd >= 0) { pfds[nfds++] = {g_tui_in_fd, POLLIN, 0}; }
        if (g_wake_fd >= 0) { pfds[nfds++] = {g_wake_fd, POLLIN, 0}; }

        int timeout_ms = g_spinner ? 1000 : (int)(g_idle_refresh_sec * 1000);
        if (timeout_ms < 100) timeout_ms = 100;
        // A half-read escape sequence has to be revisited even if no more input arrives,
        // so it can time out instead of swallowing the keystrokes that follow it.
        if (g_tui && !g_key_pending.empty() && timeout_ms > 20) timeout_ms = 20;

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
                    } else if (g_edit_window && (pfds[i].fd == g_tui_in_fd) && (pfds[i].revents & POLLIN)) {
                        if (tui_step(prev_lines, last_mtime)) g_force_refresh = true;
                    } else if (g_hotkeys && (pfds[i].fd == STDIN_FILENO) && (pfds[i].revents & POLLIN)) {
                        // With a dedicated editor window this terminal is the monitor,
                        // so its keys stay the monitor hotkeys.
                        if (g_tui && !g_edit_window) { if (tui_step(prev_lines, last_mtime)) g_force_refresh = true; }
                        else handle_keypresses_nonblock();
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
                if (g_tui && !g_key_pending.empty()) {
                    if (tui_step(prev_lines, last_mtime)) g_force_refresh = true;
                }
                tui_cursor_heartbeat();
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
            if (g_tui) { if (tui_step(prev_lines, last_mtime)) g_force_refresh = true; }
            else if (g_hotkeys) handle_keypresses_nonblock();
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
                if (tmp.type == Update::Type::Cursor) { tui_note_peer_cursor(tmp); continue; }
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
            // In TUI mode the buffer is the source of truth; our own flush is not an edit.
            if (g_tui && (now_ns() - g_tui_self_write_ns) < 300000000ull) {
                changed = false;
                last_mtime = st.st_mtim;
            }
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
                    render_view(prev_lines, {}, last_mtime, g_spinner);
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
                    render_view(new_lines, updates, last_mtime, g_spinner);
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
                        render_view(prev_lines, {}, last_mtime, g_spinner);
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
            render_view(prev_lines, {}, last_mtime, g_spinner);
            g_force_refresh = false;
        }
    }

    // Flush any pending updates on shutdown (in chunks)
    if (!g_tx_pending.empty()) {
        while (!g_tx_pending.empty()) {
            size_t sent = broadcast_updates_to_all(g_tx_pending);
            if (sent == 0) break;
            g_tx_pending.erase(g_tx_pending.begin(), g_tx_pending.begin() + (long)sent);
        }
    }
    // Final cleanup
    stop_network_session();
    deregister_user();
    if (ino.fd >= 0) {
        if (ino.wd >= 0) inotify_rm_watch(ino.fd, ino.wd);
        close(ino.fd);
    }
    if (g_listener_thread.joinable()) {
        g_stop = 1;
        g_listener_thread.join();
    }
    if (g_tui) {
        // Emit the restore sequence while writes still cannot block, then put the
        // descriptor back the way we found it -- O_NONBLOCK lives on the open file
        // description we share with the parent shell, so leaving it set would follow
        // us out of the process.
        (void)tui_write_frame("\033[?25h\033[0m\n");
        tui_set_output_nonblocking(false);
        edit_tty_release();
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
