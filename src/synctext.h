/**
 * @file synctext.h
 * @brief Declarations for the SyncText Collaborative Editor.
 *
 * This header file contains all the core data structures, constants, and the main
 * entry point for the SyncText application. It serves as the primary interface
 * and type definition source for the entire project. By centralizing these
 * declarations, it simplifies dependency management and provides a clear overview
 * of the system's components.
 */

#ifndef SYNCTEXT_H
#define SYNCTEXT_H

#include <atomic>
#include <cstdint>
#include <csignal>
#include <string>
#include <vector>
#include <ctime>
#include <sys/types.h>
#include <mqueue.h>

namespace synctext {

// ============================================================================
// :: Core Constants
// ============================================================================

constexpr size_t MAX_USERS = 5;                     ///< Maximum concurrent users in the shared registry.
constexpr size_t USER_ID_MAX = 32;                  ///< Maximum length of a user's identifier string.
constexpr size_t QUEUE_NAME_MAX = 64;               ///< Maximum length for a POSIX message queue name.
constexpr const char* REGISTRY_SHM_NAME = "/synctext_registry"; ///< Name of the POSIX shared memory object.
constexpr long MQ_MSGSIZE = 2048;                   ///< Maximum size of a single message in the message queue.
constexpr size_t RX_RING_CAP = 4096;                ///< Capacity of the inbound message ring buffer.

// ============================================================================
// :: Data Structures
// ============================================================================

/**
 * @struct UserEntry
 * @brief Represents a single user's registration in the shared memory registry.
 *
 * Each UserEntry slot tracks a participant in a collaborative session. The `active`
 * field functions as a spinlock (0: empty, 1: reserving, 2: active) to ensure
 * atomic registration.
 */
struct UserEntry {
    std::atomic<uint32_t> active;           ///< Slot state: 0=empty, 1=reserving, 2=active.
    pid_t pid;                              ///< Process ID of the user's editor instance.
    char user_id[USER_ID_MAX];              ///< Null-terminated user identifier.
    char queue_name[QUEUE_NAME_MAX];        ///< Null-terminated name of the user's message queue.
    std::atomic<uint64_t> last_heartbeat_ns;///< Timestamp of the last heartbeat, for detecting timeouts.
};

/**
 * @struct Registry
 * @brief The shared memory structure for peer discovery.
 *
 * This structure is mapped into the memory of all participating editor instances,
 * allowing them to discover and communicate with each other without a central server.
 * The `version` field is incremented on each change to signal updates to other peers.
 */
struct Registry {
    std::atomic<uint32_t> version;          ///< Incremented on any change to the user list.
    uint32_t max_users;                     ///< The total number of available UserEntry slots.
    UserEntry users[MAX_USERS];             ///< Array of user registration slots.
};

/**
 * @struct Update
 * @brief A high-level representation of a change to the document.
 *
 * This struct is used internally to represent a document modification, whether
 * it originated locally (from file watching) or remotely (from a peer). It contains
 * all necessary metadata to apply the change and resolve conflicts.
 */
struct Update {
    /**
     * @enum Type
     * @brief The specific type of modification.
     */
    enum class Type { 
        Insert,       ///< General text insertion.
        Delete,       ///< General text deletion.
        Replace,      ///< General text replacement.
        LineInsert,   ///< Insertion of one or more whole lines.
        LineDelete,   ///< Deletion of one or more whole lines.
        BlockInsert,  ///< Insertion of a multi-line block of text.
        BlockDelete   ///< Deletion of a multi-line block of text.
    } type;
    
    size_t line_no;                 ///< The 1-based line number where the change begins.
    size_t col_start, col_end;      ///< The 1-based column range of the change (for Insert, Delete, Replace).
    std::string old_content;        ///< The content that was removed or replaced.
    std::string new_content;        ///< The content that was inserted or that replaced old content.
    uint64_t timestamp_ns;          ///< High-precision timestamp for Last-Writer-Wins conflict resolution.
    std::string user_id;            ///< The identifier of the user who made the change.
};

/**
 * @struct WireHdr
 * @brief The fixed-size header for serializing an `Update` for network transmission.
 *
 * This packed struct ensures a consistent binary layout for messages sent over
 * the message queue, allowing for efficient and cross-platform communication.
 * Variable-length data (user_id, old_content, new_content) follows this header.
 */
struct WireHdr {
    uint8_t ver;            ///< Protocol version, for future compatibility.
    uint8_t type;           ///< `Update::Type` cast to a byte.
    uint32_t line_no;       ///< Line number of the update.
    uint32_t col_start;     ///< Starting column of the update.
    uint32_t col_end;       ///< Ending column of the update.
    uint64_t ts_ns;         ///< High-precision timestamp.
    uint16_t user_len;      ///< Length of the user_id string.
    uint32_t old_len;       ///< Length of the old_content string.
    uint32_t new_len;       ///< Length of the new_content string.
} __attribute__((packed));

/**
 * @struct InotifyState
 * @brief Holds the file descriptors for `inotify` file watching.
 */
struct InotifyState {
    int fd = -1;            ///< The inotify instance file descriptor.
    int wd = -1;            ///< The watch descriptor for the file being monitored.
};

/**
 * @struct InotifyFlags
 * @brief A helper struct to parse and hold the boolean flags from an `inotify` event.
 */
struct InotifyFlags {
    bool any = false;         ///< True if any event occurred.
    bool close_write = false; ///< True if a file opened for writing was closed.
    bool modify = false;      ///< True if the file content was modified.
    bool moved_to = false;    ///< True if a file was moved into the watched directory.
    bool move_self = false;   ///< True if the watched file itself was moved.
};

/**
 * @enum RegisterResult
 * @brief The result of an attempt to register a new user in the shared registry.
 */
enum class RegisterResult { 
    Ok,               ///< Registration was successful.
    DuplicateActive,  ///< A user with the same ID is already active.
    NoSlots           ///< The registry is full.
};

// ============================================================================
// :: Main Entry Point
// ============================================================================

/**
 * @brief The main entry point for the SyncText application.
 * @param argc The number of command-line arguments.
 * @param argv An array of command-line arguments.
 * @return An exit code (0 for success, non-zero for failure).
 */
int run_main(int argc, char** argv);

} // namespace synctext

#endif // SYNCTEXT_H
