/**
 * @file registry.cpp
 * @brief Shared-memory registry and per-user message queues (peer discovery).
 */
#include "shared.h"

namespace synctext {

// ============================================================================
// SECTION 1: SHARED MEMORY REGISTRY - USER DISCOVERY & COORDINATION
// ============================================================================
/**
 * The registry provides lock-free user discovery via POSIX shared memory.
 * Each user atomically reserves a slot (CAS), publishes their message queue
 * name, and sends periodic heartbeats. This enables dynamic peer discovery
 * without centralized coordination.
 */

// State used only by this file.
static int g_slot_index = -1;                ///< The index of our slot in the shared `g_registry`.
static int g_is_creator = 0;                 ///< Flag indicating if this instance created the shared memory segment.


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
void init_registry() {
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
void heartbeat() {
    if (!g_registry || g_slot_index < 0) return;
    auto& slot = g_registry->users[g_slot_index];
    slot.last_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
    if (slot.active.load(std::memory_order_relaxed) == 2) return;
    // Our slot was freed while we are alive (a peer misjudged us as stale). Take it
    // back if it is still ours, otherwise register again; without this we would stay
    // invisible to every peer, and nobody would send us anything again.
    uint32_t expected = 0;
    if (slot.pid == getpid() && slot.active.compare_exchange_strong(expected, 2)) return;
    g_slot_index = -1;
    (void)register_user_try(g_user_id, false);
}

/**
 * @brief Deregister current user from registry
 * 
 * Atomically sets slot state to 0 (empty), releasing for other users.
 * Should be called during graceful shutdown.
 * 
 * @note No-op if not registered (g_slot_index < 0)
 */
void deregister_user() {
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
std::string make_queue_name(const std::string& user_id) {
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
int g_mq_maxmsg = 256; // default max outstanding messages per queue (larger to reduce EAGAIN)
void open_message_queue(const std::string& qname) {
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
// SECTION 4: USER REGISTRATION - SLOT RESERVATION & DISCOVERY
// ============================================================================

RegisterResult register_user_try(const std::string& user_id, bool force_reclaim) {
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
pid_t find_active_pid_for_user(const std::string& user_id, bool& outFresh) {
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

} // namespace synctext
