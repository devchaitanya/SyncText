/**
 * @file main.cpp
 * @brief Program entry point and the main event loop.
 *
 * SyncText keeps one text file in sync between several people as they edit it.
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
 * 3.  **CRDT for Conflict-Free Merging (rga.h, sync.cpp):**
 *     Every character is a node with a permanent ID (Lamport counter, client id).
 *     Edits are sent as "insert after node X" / "delete node X", deletes keep a
 *     tombstone, and concurrent inserts at one spot are ordered by comparing IDs.
 *     Every copy that has received the same operations shows the same text, in
 *     whatever order they arrived. Lost messages are repaired by exchanging
 *     version vectors every few seconds (anti-entropy).
 *
 * 4.  **The file stays the user's:**
 *     Users edit a plain file in any editor (or the built-in one). The sync engine
 *     diffs the file against what it last showed, maps the change onto the nodes
 *     the user was looking at, and writes merged remote changes back to the file.
 *
 * 5.  **Efficient Change Detection:**
 *     File changes are monitored using `inotify` for near-instantaneous
 *     detection. A "settling" period is used to debounce events from editors
 *     that perform atomic saves (e.g., `rename`), which can otherwise appear
 *     as a mass-delete followed by a mass-insert. A fallback to periodic
 *     modification time polling is available if `inotify` is not supported.
 *
 * ### Where things are
 *
 *   main.cpp       run_main(): argument parsing, startup, and the main event loop.
 *   shared.h       Everything the files share (types, settings, functions).
 *   config.cpp     Settings and process-wide state used by several files.
 *   paths.cpp      User ids, document ids, file and registry names.
 *   common.cpp     now_ns() and signal handling.
 *   registry.cpp   Shared-memory registry (slots, heartbeats) and message queues.
 *   wire.cpp       Update <-> bytes.
 *   listener.cpp   Thread reading our message queue into the inbox.
 *   inbox.cpp      Lock-free ring buffer of received updates + the wake-up fd.
 *   broadcast.cpp  Sending local updates to every peer (queues or TCP).
 *   net.cpp        LAN mode: --host / --join, roster, host failover.
 *   serve.cpp      Headless multi-document server (--serve).
 *   fileio.cpp     Reading/writing the document file.
 *   rga.h/.cpp     The sequence CRDT: nodes, IDs, tombstones, counted B-tree, replica.
 *   sync.cpp       Sync engine: file/buffer changes <-> CRDT ops, joining, anti-entropy.
 *   startup.cpp    Getting the first copy of the document (from a peer or fresh).
 *   watcher.cpp    inotify, spawning helpers, raw terminal mode, env parsing.
 *   monitor.cpp    The read-only document view.
 *   tui.cpp        The built-in terminal editor (and its separate window).
 */
#include "shared.h"

namespace synctext {

// State used only by this file.
static bool g_interactive_login = false;     ///< If true, prompts user on how to handle an existing session for the same user ID.
// --- Timing & Tuning Configuration ---
static int g_debounce_ms = 0;                ///< Milliseconds to wait after a file change before processing, for basic debouncing.
static int g_settle_ms = 150;                ///< Milliseconds of quiet time required after a burst of `inotify` events before processing.
static int g_settle_overall_ms = 1500;       ///< Maximum time to wait for settling before proceeding regardless.
static uint64_t g_idle_refresh_sec = 5;      ///< Seconds between UI refreshes when idle and `g_spinner` is off.
static int g_merge_write_grace_ms = 300;     ///< After a local save, wait this long before writing merged remote changes (avoids racing the user's editor).
static int g_file_write_delay_ms = 40;       ///< Hold merged text this long, and until the file has been quiet this long, before writing it.
static int g_mass_diff_recheck_ms = 150;     ///< Milliseconds to wait before re-reading a file if a suspicious mass change is detected.
static int g_suspicious_min_lines = 10;      ///< Minimum number of lines that must change to trigger a "suspicious change" check.
static int g_suspicious_ratio = 90;          ///< Percentage of lines that must change to trigger a "suspicious change" check.
static int g_broadcast_batch_n = 1;          ///< Number of local updates to batch before broadcasting.
static bool g_user_id_explicit = false;      ///< True when SYNCTEXT_USER named the identity.
static bool g_serve_mode = false;            ///< True for a headless multi-document server.
// Listener thread counters
static std::thread g_listener_thread;
static bool g_debug_msg = false; // legacy flag (SYNCTEXT_DEBUG_MSG=1) mapped to level>=2


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
 *    c. Drain received messages from the ring buffer into the sync engine
 *    d. Write merged remote changes to the file
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
                // The slot publishes g_queue_name and g_doc_path, so they must already
                // name the candidate's queue and file when it is registered; otherwise
                // peers would send to the first session's queue instead of ours.
                std::string old_doc = g_doc_path, old_queue = g_queue_name;
                g_doc_path = resolve_doc_path(candidate);
                g_queue_name = make_queue_name(candidate);
                if (register_user_try(candidate, false) == RegisterResult::Ok) {
                    g_user_id = candidate;
                    renamed = true;
                    break;
                }
                g_doc_path = old_doc;
                g_queue_name = old_queue;
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
        std::cout << "[status] protocol=3 (crdt) mq_maxmsg=" << g_mq_maxmsg
                  << " broadcast_batch=" << g_broadcast_batch_n
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
    // Grace window before writing merged content (ms) to avoid racing local editor writes
    g_merge_write_grace_ms = parse_env_millis("SYNCTEXT_MERGE_WRITE_GRACE_MS", g_merge_write_grace_ms);
    g_file_write_delay_ms = parse_env_millis("SYNCTEXT_FILE_WRITE_DELAY_MS", g_file_write_delay_ms);
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
    // Open the local editor (best-effort, non-blocking). Skipped when we are the editor.
    if (!g_tui) open_local_editor_if_possible();

    // Start the CRDT replica (after every setting above is parsed): import our file if
    // nobody else is here, otherwise ask a live peer for the shared history; our file
    // is replaced by it once it arrives.
    sync_start();
    // Initial render
    if (g_tui) { tui_clamp_cursor(prev_lines); tui_broadcast_cursor(true); }
    render_view(prev_lines, {}, last_mtime, g_spinner);

    // Main loop: event-driven with 1s timeout; fallback to stat check. Also poll stdin for hotkeys.
    uint64_t last_rx_total = g_rx_total.load(std::memory_order_relaxed);
    while (!g_stop) {
        heartbeat();

        bool changed = false;
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
        // While joining, or while merged text waits for the write grace window to pass,
        // come back quickly instead of sleeping for the idle interval.
        if ((!sync_ready() || sync_dirty()) && timeout_ms > 50) timeout_ms = 50;

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

        // Drain received messages: carets go to the TUI, CRDT traffic to the sync engine.
        {
            Update tmp;
            while (rx_ring_pop(tmp)) {
                if (tmp.type == Update::Type::Cursor) { tui_note_peer_cursor(tmp); continue; }
                if (tmp.type == Update::Type::CrdtOps || tmp.type == Update::Type::CrdtSync) sync_handle(tmp);
                g_force_refresh = true;
            }
        }
        sync_tick();

        if (!sync_ready()) {
            // Still joining: whatever is in the file now is replaced by the shared copy.
            if (stat(g_doc_path.c_str(), &st) == 0) last_mtime = st.st_mtim;
        } else if (stat(g_doc_path.c_str(), &st) == 0) {
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
                // Turn the change into CRDT ops (mapped through the nodes the file showed)
                // and send them.
                if (sync_local_edit(new_lines)) {
                    g_last_local_change_ns.store(now_ns(), std::memory_order_relaxed);
                    render_view(new_lines, {}, last_mtime, g_spinner);
                    g_force_refresh = false;      // we just rendered
                }
                prev_lines = new_lines;
            }
        }

        // Write merged remote changes. The file is shared with the user's editor, so
        // hold back for a short grace window after a local save instead of racing it;
        // the replica keeps the merged state meanwhile and local edits still map
        // correctly (they are anchored to nodes, not positions). The built-in editor
        // owns its buffer, so it takes remote changes immediately.
        static uint64_t dirty_since_ns = 0;
        if (!sync_dirty()) dirty_since_ns = 0;
        else {
            uint64_t nowt = now_ns();
            if (dirty_since_ns == 0) dirty_since_ns = nowt;
            uint64_t last_local = g_last_local_change_ns.load(std::memory_order_relaxed);
            bool recent_local = (nowt >= last_local) &&
                                ((nowt - last_local) / 1000000ull < (uint64_t)g_merge_write_grace_ms);
            // Never write over a save we have not read yet: if the file changed since we
            // last read or wrote it, the user just saved. Skip this round; the next loop
            // turn reads their edit (turning it into ops), and then writes the merge.
            struct stat on_disk{};
            bool have_stat = stat(g_doc_path.c_str(), &on_disk) == 0;
            bool unread_save = !g_tui && have_stat && !timespec_equal(on_disk.st_mtim, last_mtime);
            // The file is shared with the user's editor and nothing locks it, so two
            // writes can interleave. Remote ops arrive within a millisecond, often while a
            // save is still being written, so hold merged text briefly: a save that
            // coincides with a peer's edit finishes first and is read (unread_save) before
            // we write. The built-in editor owns its buffer and writes immediately.
            uint64_t file_mtime_ns = have_stat ? (uint64_t)on_disk.st_mtim.tv_sec * 1000000000ull +
                                                     (uint64_t)on_disk.st_mtim.tv_nsec : 0;
            uint64_t delay_ns = (uint64_t)g_file_write_delay_ms * 1000000ull;
            bool settled = g_tui || ((nowt - dirty_since_ns) >= delay_ns &&
                                     (nowt < file_mtime_ns || nowt - file_mtime_ns >= delay_ns));
            if ((g_tui || !recent_local) && !unread_save && settled) {
                size_t caret_line = 0, caret_col = 0;
                if (g_tui) tui_get_cursor(caret_line, caret_col);
                auto merged = sync_take_lines(g_tui ? &caret_line : nullptr, g_tui ? &caret_col : nullptr);
                FILE* f = std::fopen(g_doc_path.c_str(), "wb");
                if (f) {
                    for (const auto& line : merged) {
                        std::fwrite(line.c_str(), 1, line.size(), f);
                        std::fwrite("\n", 1, 1, f);
                    }
                    std::fclose(f);
                    struct stat st_upd;
                    if (stat(g_doc_path.c_str(), &st_upd) == 0) last_mtime = st_upd.st_mtim;
                }
                if (g_tui) {
                    g_tui_self_write_ns = now_ns();      // our own write is not an edit
                    tui_set_cursor(caret_line, caret_col);
                }
                prev_lines = std::move(merged);
                g_force_refresh = true;
                dirty_since_ns = 0;
            }
        }
        // Background-drain any pending local broadcasts
        if (!g_tx_pending.empty()) {
            size_t sent = broadcast_updates_to_all(g_tx_pending);
            if (sent > 0) g_tx_pending.erase(g_tx_pending.begin(), g_tx_pending.begin() + (long)sent);
        }
        if (g_force_refresh || g_spinner) {
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
    // Queues are unlinked by name. If a new session of the same user has registered
    // while we were shutting down, the name now belongs to its queue: leave it alone.
    bool name_taken = false;
    if (g_registry && !g_queue_name.empty()) {
        for (size_t i = 0; i < MAX_USERS; ++i) {
            const auto& slot = g_registry->users[i];
            if (slot.active.load(std::memory_order_relaxed) == 2 && slot.pid != getpid() &&
                std::strncmp(slot.queue_name, g_queue_name.c_str(), QUEUE_NAME_MAX) == 0) name_taken = true;
        }
    }
    if (!g_queue_name.empty() && !name_taken) mq_unlink(g_queue_name.c_str());
    if (g_registry) munmap(g_registry, g_registry_size);
    if (g_shm_fd >= 0) close(g_shm_fd);
    return 0;
}

} // namespace synctext

int main(int argc, char** argv) {
    return synctext::run_main(argc, argv);
}
