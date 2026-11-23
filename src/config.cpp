/**
 * @file config.cpp
 * @brief Settings and process-wide state read by several modules.
 */
#include "shared.h"

namespace synctext {


// ============================================================================
// :: Internal Globals & State
// ============================================================================

// --- Core State ---
// Set from a signal handler AND read by the network/listener threads, so it needs to be
// atomic rather than merely volatile. Lock-free on every platform we target, which is what
// makes it safe to touch from a signal handler.
std::atomic<int> g_stop{0};           ///< Global flag to signal graceful shutdown. Set by `handle_signal`.
static_assert(std::atomic<int>::is_always_lock_free, "g_stop must be lock-free to be signal-safe");
Registry* g_registry = nullptr;       ///< Pointer to the mmapped shared memory registry.
int g_shm_fd = -1;                    ///< File descriptor for the shared memory object.
std::string g_user_id;                ///< Identifier for the current user.
std::string g_doc_path;               ///< Absolute or relative path to the document being edited.
std::string g_queue_name;             ///< Name of this instance's POSIX message queue.
mqd_t g_mq = (mqd_t)-1;               ///< Descriptor for our own message queue.
bool g_network_mode = false;          ///< True when running a LAN session (--host/--join).
std::atomic<bool> g_network_host{false}; ///< True if we started the session (--host) or serve it.
std::string g_access_code;            ///< Shared secret every participant presents in HELLO.
int g_network_port = 9000;            ///< Port --host listens on, and the port a joiner dialled.
int g_listen_fd = -1;                 ///< Our own listening socket: peers link to us here.
int g_listen_port = 0;                ///< Port actually bound by g_listen_fd (may be ephemeral).
std::mutex g_network_clients_mutex;
std::vector<std::shared_ptr<NetworkClient>> g_network_clients;
std::mutex g_roster_mutex;
std::vector<RosterEntry> g_roster;    ///< Shown in the UI: us, then everyone we hold a link to.

// --- UI & Interaction ---
bool g_spinner = false;               ///< If true, an animated spinner is shown in the UI.
bool g_hotkeys = false;               ///< If true, hotkey support for interaction is enabled.
std::atomic<bool> g_force_refresh{false}; ///< Forces a UI re-render; set by network threads too, so atomic.
int g_send_retry_count = 200;         ///< Number of retries for a blocked `mq_send` call.
int g_send_retry_delay_ms = 5;        ///< Delay in milliseconds between `mq_send` retries.
uint64_t g_stale_ns = 10ull * 1000000000ull; ///< Nanoseconds after which an inactive peer is considered stale. Default: 10s.

// --- Heuristics & Advanced Tuning ---
bool g_periodic_mtime = false;        ///< If true, periodically check file modification time even if `inotify` is active.
int g_max_broadcast_per_cycle = 128;  ///< Maximum number of updates to broadcast in a single main loop iteration.

// --- Internal State Management ---
size_t g_registry_size = sizeof(Registry); ///< The expected size of the shared memory registry.
std::atomic<uint64_t> g_last_local_change_ns{0}; ///< Timestamp of the last detected local file modification.

// ----------------------------------------------------------------------------
// Document scope
//
// Without --doc, everything keeps the original single-scope layout: one registry,
// one logical document per machine. With --doc <id>, the registry, the message
// queues and the files all carry the id, so sessions on different documents never
// discover each other and can run side by side.
// ----------------------------------------------------------------------------
std::string g_doc_id;                 ///< Empty means the original single-document layout.
std::string g_base_user_id;           ///< Identity before any "-2" collision suffix.
std::vector<std::string> g_doc_ids;   ///< Every --doc given; more than one only makes sense with --serve.

} // namespace synctext
