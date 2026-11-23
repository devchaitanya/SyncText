/**
 * @file monitor.cpp
 * @brief Monitor view: the read-only document display.
 */
#include "shared.h"

namespace synctext {

std::string type_to_string(Update::Type t) {
    switch (t) {
        case Update::Type::Insert: return "insert";
        case Update::Type::Delete: return "delete";
        case Update::Type::Replace: return "replace";
        case Update::Type::LineInsert: return "line-insert";
        case Update::Type::LineDelete: return "line-delete";
        case Update::Type::BlockInsert: return "block-insert";
        case Update::Type::BlockDelete: return "block-delete";
        case Update::Type::Cursor: return "cursor";
        case Update::Type::CrdtOps: return "crdt-ops";
        case Update::Type::CrdtSync: return "crdt-sync";
    }
    return "unknown";
}
std::string time_str_from(std::time_t t) {
    char buf[64];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    return buf;
}

static bool stdout_supports_color() {
    if (std::getenv("SYNCTEXT_NO_COLOR")) return false;
    return isatty(STDOUT_FILENO);
}

/// Colour gate for the monitor view, which writes to stdout rather than the editor tty.
static const char* MC(const char* seq) { return stdout_supports_color() ? seq : ""; }

/// The monitor view: what the terminal shows when the editor has a window of its own
/// (and the whole UI when there is no editor at all). Same visual language as the
/// editor -- status bars top and bottom, a dim gutter -- so the two read as one app.
void render_document(const std::vector<std::string>& lines,
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
        // Mesh: everyone we hold a direct link to. Via --serve: the server's list.
        bool mesh = net_is_mesh();
        for (size_t i = 0; i < snap.size(); ++i) {
            who.push_back(snap[i].user_id + (!mesh && i == 0 ? " (server)" : ""));
        }
        if (who.empty()) who.push_back(g_user_id);
        role = mesh ? " mesh" : " via server";
    } else if (g_registry) {
        for (size_t i = 0; i < MAX_USERS; ++i) {
            auto &slot = g_registry->users[i];
            if (slot.active.load(std::memory_order_relaxed) != 2) continue;
            uint64_t hb = slot.last_heartbeat_ns.load(std::memory_order_relaxed);
            uint64_t now = now_ns();   // read after hb: a heartbeat written meanwhile is "fresh", not "in the future"
            bool fresh = hb >= now || (now - hb) <= g_stale_ns;
            bool alive = (kill(slot.pid, 0) == 0) || (errno == EPERM);
            if (fresh && alive) {
                who.push_back(slot.user_id);
            } else if (!alive) {
                // Only a slot whose process is gone is freed. A live but quiet peer (busy,
                // stopped in a debugger) is just not listed; freeing its slot would cut it
                // off from every broadcast for good.
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

} // namespace synctext
