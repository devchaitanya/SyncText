/**
 * @file shared.h
 * @brief Everything the source files share with each other.
 *
 * Each section lists what one .cpp file offers the rest of the program. Anything
 * not listed here is private (`static`) to its file.
 */
#ifndef SYNCTEXT_SHARED_H
#define SYNCTEXT_SHARED_H

#include "synctext.h"
#include "rga.h"

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

namespace synctext {

// ============================================================================
// Types, constants and macros used by more than one file
// ============================================================================

struct ServedDoc;   // multi-document server, defined further down

struct NetworkClient {
    int fd = -1;
    std::string user_id;
    std::atomic<bool> finished{false};
    ServedDoc* doc = nullptr;   ///< Non-null only in --serve mode: which document this client joined.
    std::string ip;             ///< Peer address, published in that document's roster.
    int port = 0;               ///< Peer's listening port, published in rosters.
};

/// One participant in the session: a user and the address its peers link to.
struct RosterEntry { std::string user_id; std::string ip; int port = 0; };

enum NetworkFrameKind : uint8_t { HELLO = 1, SNAPSHOT = 2, UPDATE = 3, ERROR = 4, ROSTER = 5, DOCINFO = 6 };

constexpr uint32_t NETWORK_MAX_FRAME = 1024 * 1024;

struct ServedDoc {
    std::string id;                                        ///< Document id ("" for the default scope).
    std::string path;                                      ///< Replica this server keeps on disk.
    std::mutex mu;                                         ///< Guards clients and the replica file.
    std::vector<std::shared_ptr<NetworkClient>> clients;
    std::unique_ptr<crdt::Replica> replica;               ///< The server's own copy (guarded by mu).
};

// Shared TUI presentation primitives used by both the monitor renderer and the
// built-in editor renderer below.
struct TuiGlyphs { const char* gutter; const char* caret_row; const char* dot; const char* tilde; const char* trunc; };

#define TUI_BAR       "\033[48;5;238m\033[38;5;253m"
#define TUI_BAR_ACC   "\033[48;5;238m\033[38;5;81m\033[1m"
#define TUI_BAR_DIM   "\033[48;5;238m\033[38;5;245m"
#define TUI_GUTTER    "\033[38;5;242m"
#define TUI_HINT      "\033[38;5;244m"
#define TUI_KEY       "\033[38;5;81m"
#define TUI_WARN      "\033[38;5;215m"
#define TUI_OFF       "\033[0m"

// ============================================================================
// config.cpp: Settings and process-wide state read by several modules
// ============================================================================
extern std::atomic<int> g_stop;
extern Registry* g_registry;
extern int g_shm_fd;
extern std::string g_user_id;
extern std::string g_doc_path;
extern std::string g_queue_name;
extern mqd_t g_mq;
extern bool g_network_mode;
extern std::atomic<bool> g_network_host;
extern std::string g_access_code;
extern int g_network_port;
extern int g_listen_fd;
extern int g_listen_port;
extern std::mutex g_network_clients_mutex;
extern std::vector<std::shared_ptr<NetworkClient>> g_network_clients;
extern std::mutex g_roster_mutex;
extern std::vector<RosterEntry> g_roster;
extern bool g_spinner;
extern bool g_hotkeys;
extern std::atomic<bool> g_force_refresh;
extern int g_send_retry_count;
extern int g_send_retry_delay_ms;
extern uint64_t g_stale_ns;
extern bool g_periodic_mtime;
extern int g_max_broadcast_per_cycle;
extern size_t g_registry_size;
extern std::atomic<uint64_t> g_last_local_change_ns;
extern std::string g_doc_id;
extern std::string g_base_user_id;
extern std::vector<std::string> g_doc_ids;

// ============================================================================
// paths.cpp: Identities, document ids and where files live
// ============================================================================
/// Who this session is, worked out rather than typed.
///
/// The document is what the user names; their own identity is an implementation
/// detail, so it comes from the OS account. The same account opening one document
/// twice on a machine would collide, so register_user_try() appends a numeric suffix
/// until the id is free. SYNCTEXT_USER overrides it, which scripts and tests use.
std::string default_user_id();
/// Validate a document id. Rejects rather than rewrites: silently mapping '/' to '_'
/// would let "a/b" and "a_b" land in the same document without anyone noticing, and
/// "." or ".." would point the scope at a directory that is already in use.
/// Returns true and fills `out` on success; on failure `why` explains the problem.
bool validate_doc_id(const std::string& in, std::string& out, std::string& why);
/// Short, collision-free stand-in for the id inside POSIX queue names.
///
/// A queue name is capped at QUEUE_NAME_MAX, and it has to hold "/synctext_",
/// the scope, a separator and the user id. A long id would otherwise truncate the
/// user id away and hand every participant in the document the same queue.
std::string doc_scope_tag();
/// Directory holding this scope's documents.
std::string doc_dir();
/// Where a given participant's copy of this document lives.
std::string doc_path_for(const std::string& user_id);
/// Same, resolved to an absolute path. Sessions are routinely started from different
/// working directories, and peers must be able to find each other's documents.
std::string resolve_doc_path(const std::string& user_id);
/// Peers are discovered through the registry, so a per-document registry is what
/// actually separates one document's participants from another's.
std::string registry_shm_name();

// ============================================================================
// common.cpp: Clock and signal handling
// ============================================================================
/**
 * @brief Get current time in nanoseconds since Unix epoch
 * @return Nanosecond timestamp (uint64_t)
 * 
 * Uses CLOCK_REALTIME (wall clock). Used for heartbeats, timeouts and caret expiry.
 * Merge order does NOT depend on it: the CRDT orders edits by Lamport counters.
 */
uint64_t now_ns();
/**
 * @brief Install signal handlers for clean exit
 * 
 * Registers handle_signal() for SIGINT and SIGTERM.
 * Allows deregistration from registry and queue cleanup before exit.
 */
void install_signal_handlers();

// ============================================================================
// registry.cpp: Shared-memory registry and per-user message queues (peer discovery)
// ============================================================================
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
void init_registry();
/**
 * @brief Update heartbeat timestamp for current user
 * 
 * Writes now_ns() to registry slot's last_heartbeat_ns field.
 * Called periodically (every ~1 second) to prove liveness.
 * Stale users (no heartbeat for >10 seconds) may be reclaimed.
 * 
 * @note No-op if not registered (g_slot_index < 0)
 */
void heartbeat();
/**
 * @brief Deregister current user from registry
 * 
 * Atomically sets slot state to 0 (empty), releasing for other users.
 * Should be called during graceful shutdown.
 * 
 * @note No-op if not registered (g_slot_index < 0)
 */
void deregister_user();
/**
 * @brief Generate POSIX message queue name from user ID
 * @param user_id User identifier string
 * @return Queue name like "/synctext_user_alice"
 * 
 * Sanitizes user_id by replacing '/' with '_' (queue names must start with /).
 * Truncates to QUEUE_NAME_MAX if necessary.
 */
std::string make_queue_name(const std::string& user_id);
extern int g_mq_maxmsg;
void open_message_queue(const std::string& qname);
RegisterResult register_user_try(const std::string& user_id, bool force_reclaim);
pid_t find_active_pid_for_user(const std::string& user_id, bool& outFresh);

// ============================================================================
// wire.cpp: Binary encoding of an Update
// ============================================================================
bool serialize_update(const Update& u, char* out, size_t cap, size_t& out_len);
bool deserialize_update(const char* in, size_t len, Update& u);

// ============================================================================
// inbox.cpp: Receive ring buffer and the main-loop wake-up fd
// ============================================================================
extern std::atomic<uint64_t> g_rx_total;
extern std::atomic<uint64_t> g_rx_dropped;
extern int  g_debug_level;
extern std::vector<Update> g_rx_ring;
extern std::atomic<size_t> g_rx_head;
extern std::atomic<size_t> g_rx_tail;
void enqueue_received_update(Update&& u);
extern int g_wake_fd;
void init_wake_fd();

// ============================================================================
// net.cpp: LAN mode: a full mesh of TCP links between participants
// ============================================================================
extern int g_connect_timeout_ms;
extern bool g_port_explicit;
extern int g_port_base;
extern int g_port_span;
/// Bound how long a handshake read may block. A peer that accepts the connection
/// and then says nothing -- a half-open host, a blackholing middlebox, or our own
/// listener after a self-connect -- must not park the joiner forever.
void set_recv_timeout(int fd, int ms);
extern std::string g_network_error;
bool send_frame(int fd, uint8_t kind, const void* payload, size_t len);
bool recv_frame(int fd, uint8_t& kind, std::vector<char>& payload);
bool network_send_update(int fd, const Update& u);
bool parse_network_hello(const std::vector<char>& payload, std::string& out_user_id, int& out_port,
                                bool& out_link, std::string& out_doc_id);
/// False while our only link is to a --serve server (which relays for us).
bool net_is_mesh();
/// The participant we joined through; it answers our request for the history.
std::string net_contact_peer();
/// Bind our listening socket.
///
/// With no port on the command line a host takes the first free port in
/// [g_port_base, g_port_base + g_port_span) and prints it, so nobody has to pick a
/// number that might already be taken. A joiner only needs *some* port that its
/// peers can link to, so it takes an ephemeral one.
bool network_open_listener();
bool start_network_session(const std::string& host);
void stop_network_session();

// ============================================================================
// listener.cpp: Thread that receives updates from our message queue
// ============================================================================
void listener_loop();

// ============================================================================
// broadcast.cpp: Sending local updates to every peer
// ============================================================================
extern std::vector<Update> g_tx_pending;
// Send up to 'g_max_broadcast_per_cycle' operations (prefix of ops) to ALL recipients.
// Returns the number of ops sent (0..min(ops.size(), g_max_broadcast_per_cycle)).
size_t broadcast_updates_to_all(std::vector<Update>& ops);
// Send every op to one participant (by user id): its queue locally, its connection
// when we host, the host link when we join. Falls back to everyone if it is unknown.
void send_updates_to_user(const std::string& user_id, std::vector<Update>& ops);
// Local mode: is any other participant registered, alive and heartbeating?
bool local_peer_alive();

// ============================================================================
// sync.cpp: The sync engine between the editor and the CRDT (see rga.h)
// ============================================================================
extern std::atomic<bool> g_sync_now;     ///< Set by the network thread after a reconnect.
std::string lines_to_text(const std::vector<std::string>& lines);
std::vector<std::string> text_to_lines(const std::string& text);
std::vector<std::string> crdt_encode_ops(const std::vector<crdt::Op>& ops, size_t max_bytes);
bool crdt_decode_ops(const std::string& payload, std::vector<crdt::Op>& out);
std::string crdt_encode_vv(const crdt::VersionVector& vv);
bool crdt_decode_vv(const std::string& payload, crdt::VersionVector& out);
std::string crdt_genesis_origin(const std::string& text);
std::string crdt_new_client_id();
/// Create the replica: import the local file if we are first, otherwise ask peers.
void sync_start();
/// True once the replica holds the shared document and local edits may be sent.
bool sync_ready();
/// The file or buffer now holds new_lines: turn the change into ops and send them.
bool sync_local_edit(const std::vector<std::string>& new_lines);
/// Handle a CrdtOps or CrdtSync envelope from a peer.
void sync_handle(const Update& u);
/// Housekeeping: join timeout, periodic anti-entropy, reconnect resync.
void sync_tick();
/// True when the replica has text the file / buffer does not show yet.
bool sync_dirty();
/// The replica's text as lines; it becomes the new baseline. If caret pointers are
/// given, the caret is moved so it stays next to the same character.
std::vector<std::string> sync_take_lines(size_t* caret_line, size_t* caret_col);
std::string sync_status();

// ============================================================================
// fileio.cpp: Reading and writing the document file
// ============================================================================
std::vector<std::string> read_file_lines(const std::string& path);
// Read lines with basic stabilization: ensure file's mtime and size are
// unchanged before and after the read, or give up after max_wait_ms.
std::vector<std::string> read_file_lines_stable(const std::string& path, int max_wait_ms);
void write_initial_doc(const std::string& path);

// ============================================================================
// monitor.cpp helpers
// ============================================================================
std::string type_to_string(Update::Type t);
std::string time_str_from(std::time_t t);

// ============================================================================
// serve.cpp: Multi-document server (--serve)
// ============================================================================
/// Entry point for --serve. Runs until interrupted; there is no editor UI.
int serve_main();

// ============================================================================
// monitor.cpp: Monitor view: the read-only document display
// ============================================================================
/// The monitor view: what the terminal shows when the editor has a window of its own
/// (and the whole UI when there is no editor at all). Same visual language as the
/// editor -- status bars top and bottom, a dim gutter -- so the two read as one app.
void render_document(const std::vector<std::string>& lines,
                            const std::vector<Update>& updates,
                            struct timespec last_change_time,
                            bool show_spinner);

// ============================================================================
// tui.cpp: Built-in terminal editor and its separate window
// ============================================================================
extern bool g_tui;
extern int g_tui_in_fd;
extern bool g_edit_window;
extern std::string g_edit_window_marker;
extern std::string g_key_pending;
extern uint64_t g_tui_self_write_ns;
/// Put stdout in non-blocking mode for the duration of the session.
///
/// A frame is a few KB, and if whatever is on the other end stops reading (a paused
/// terminal, a stalled ssh link, a harness that never drains its pty) a blocking
/// write() parks the main loop indefinitely -- which in a collaborative editor also
/// stops remote edits from being merged. Since every frame is a full repaint, giving
/// up on one we cannot write costs nothing: the next one paints the current state.
void tui_set_output_nonblocking(bool on);
/// Write one frame, tolerating partial writes and abandoning it if the terminal
/// will not accept more. Returns false if the frame was dropped.
bool tui_write_frame(const std::string& frame);
void term_size_fd(int fd, int& rows, int& cols);
// Box-drawing and marker glyphs, with an ASCII fallback for terminals or fonts
// that make a mess of them (SYNCTEXT_ASCII=1).
TuiGlyphs tui_glyphs();
void tui_clamp_cursor(const std::vector<std::string>& buf);
void tui_get_cursor(size_t& line, size_t& col);
void tui_set_cursor(size_t line, size_t col);
/// Record a peer caret. Called from the rx drain; carets never touch the document.
void tui_note_peer_cursor(const Update& u);
/// Tell everyone where our caret is, throttled and only when it actually moved.
void tui_broadcast_cursor(bool force);
/// Re-announce our caret every few seconds so peers' copies never expire while we idle.
void tui_cursor_heartbeat();
/// Pad a status bar to the full terminal width: left-aligned and right-aligned
/// halves, with the bar colour running edge to edge.
std::string pad_bar(const std::string& left, const std::string& right, int cols,
                           const char* bar_col, const char* off_col);
void tui_render(const std::vector<std::string>& buf);
void edit_tty_release();
/// Open a window for the editor and attach to it. Returns false if we could not,
/// in which case the caller keeps the editor in the current terminal.
bool open_edit_window();
/// One editing turn: consume keystrokes, publish whatever changed, keep the file current.
/// Returns true if the screen needs repainting.
bool tui_step(std::vector<std::string>& buf, struct timespec& last_mtime);

// ============================================================================
// startup.cpp: Getting the first copy of the document
// ============================================================================
/// Draw with whichever front-end is active.
void render_view(const std::vector<std::string>& lines,
                        const std::vector<Update>& updates,
                        struct timespec last_change_time,
                        bool show_spinner);
bool ensure_local_doc();

// ============================================================================
// watcher.cpp: File watching (inotify), helper processes, terminal mode, env parsing
// ============================================================================
InotifyState init_inotify_for_file(const std::string& path);
InotifyFlags drain_inotify_events(int fd);
void settle_inotify_quiet(int fd, int quiet_ms, int overall_ms);
bool spawn_bg_shell(const std::string& cmdline);
void open_local_editor_if_possible();
void set_terminal_rawmode();
void restore_terminal_mode();
void handle_keypresses_nonblock();
uint64_t parse_env_seconds(const char* name, uint64_t def_sec);
int parse_env_millis(const char* name, int def_ms);
bool parse_env_bool(const char* name, bool defVal=true);

// ============================================================================
// Small helpers defined inline so every file can use them
// ============================================================================
inline bool rx_ring_pop(Update& out) {
    size_t tail = g_rx_tail.load(std::memory_order_relaxed);
    size_t head = g_rx_head.load(std::memory_order_acquire);
    if (tail == head) return false; // empty
    out = std::move(g_rx_ring[tail % RX_RING_CAP]);
    g_rx_tail.store(tail + 1, std::memory_order_release);
    return true;
}

inline void wake_main_loop() {
    if (g_wake_fd < 0) return;
    uint64_t one = 1;
    ssize_t r = write(g_wake_fd, &one, sizeof(one));
    (void)r;
}

inline void drain_wake_fd() {
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

// Helper to compare timespec with nanosecond precision
inline bool timespec_equal(const struct timespec& a, const struct timespec& b) {
    return a.tv_sec == b.tv_sec && a.tv_nsec == b.tv_nsec;
}

} // namespace synctext

#endif
