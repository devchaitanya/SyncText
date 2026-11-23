/**
 * @file startup.cpp
 * @brief Getting the first copy of the document.
 */
#include "shared.h"

namespace synctext {


/// Draw with whichever front-end is active.
void render_view(const std::vector<std::string>& lines,
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
        nowt = now_ns();  // after reading hb
            bool fresh = hb >= nowt || (nowt - hb) <= g_stale_ns;
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
bool ensure_local_doc() {
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
            nowt = now_ns();  // after reading hb
            bool fresh = hb >= nowt || (nowt - hb) <= g_stale_ns;
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

} // namespace synctext
