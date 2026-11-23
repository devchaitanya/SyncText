/**
 * @file tui.cpp
 * @brief Built-in terminal editor and its separate window.
 */
#include "shared.h"

namespace synctext {

// State used only by this file.
#define TUI_GUTTER_ON "\033[38;5;222m"
#define TUI_CURLINE   "\033[48;5;236m"
#define TUI_TILDE     "\033[38;5;238m"


// ============================================================================
// SECTION 5b: BUILT-IN TERMINAL EDITOR (TUI)
//
// An external GUI editor only tells us anything when the user saves, so it can
// never show per-character updates and has nowhere to draw a peer's caret. This
// editor keeps the document in the terminal: every keystroke mutates the shared
// buffer, is diffed immediately and sent as CRDT ops (sync.cpp). Caret positions
// travel as Update::Type::Cursor records, which never touch the document.
// ============================================================================

bool g_tui = false;                   ///< True when the built-in editor is active.
/// The editor draws to, and reads keys from, these descriptors. They default to our
/// own terminal, but when the editor gets its own window they point at that window's
/// tty instead, leaving stdout free for the monitor view.
int g_tui_in_fd = STDIN_FILENO;
static int g_tui_out_fd = STDOUT_FILENO;
bool g_edit_window = false;           ///< True when the editor has a window of its own.
static pid_t g_edit_window_pid = -1;         ///< Helper process holding that window open.
std::string g_edit_window_marker;     ///< File the helper reports its tty path in.
static struct termios g_edit_term_orig{};    ///< Saved settings for the editor tty.
static bool g_edit_term_saved = false;
static size_t g_cur_line = 0;                ///< Local caret line (0-based index into the buffer).
static size_t g_cur_col = 0;                 ///< Local caret column (0-based byte offset in the line).
static int g_view_top = 0;                   ///< First buffer line shown (vertical scroll).
static int g_view_left = 0;                  ///< First column shown (horizontal scroll).
std::string g_key_pending;            ///< Bytes of a partially-read escape sequence.
static std::string g_tui_status;             ///< Transient message shown in the footer.
static uint64_t g_tui_status_ns = 0;
uint64_t g_tui_self_write_ns = 0;     ///< When we last wrote the doc ourselves (suppresses self-echo).
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
void tui_set_output_nonblocking(bool on) {
    int flags = fcntl(g_tui_out_fd, F_GETFL, 0);
    if (flags < 0) return;
    if (on) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
    (void)fcntl(g_tui_out_fd, F_SETFL, flags);
}

/// Write one frame, tolerating partial writes and abandoning it if the terminal
/// will not accept more. Returns false if the frame was dropped.
bool tui_write_frame(const std::string& frame) {
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
void term_size_fd(int fd, int& rows, int& cols) {
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
TuiGlyphs tui_glyphs() {
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
void tui_get_cursor(size_t& line, size_t& col) { line = g_cur_line; col = g_cur_col; }
void tui_set_cursor(size_t line, size_t col) { g_cur_line = line; g_cur_col = col; }
void tui_clamp_cursor(const std::vector<std::string>& buf) {
    if (buf.empty()) { g_cur_line = 0; g_cur_col = 0; return; }
    if (g_cur_line >= buf.size()) g_cur_line = buf.size() - 1;
    if (g_cur_col > buf[g_cur_line].size()) g_cur_col = buf[g_cur_line].size();
}

/// Record a peer caret. Called from the rx drain; carets never touch the document.
void tui_note_peer_cursor(const Update& u) {
    if (u.user_id == g_user_id) return;
    PeerCursor pc; pc.line = u.line_no; pc.col = u.col_start; pc.seen_ns = now_ns();
    g_peer_cursors[u.user_id] = pc;
    g_force_refresh = true;
}

/// Tell everyone where our caret is, throttled and only when it actually moved.
void tui_broadcast_cursor(bool force) {
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
void tui_cursor_heartbeat() {
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
std::string pad_bar(const std::string& left, const std::string& right, int cols,
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
void tui_render(const std::vector<std::string>& buf) {
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
    if (g_network_mode) left += std::string(C(TUI_BAR_DIM)) + (net_is_mesh() ? "  mesh" : "  via server") + C(TUI_BAR);
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
void edit_tty_release() {
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
bool open_edit_window() {
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
bool tui_step(std::vector<std::string>& buf, struct timespec& last_mtime) {
    bool moved = false;
    std::vector<std::string> before = buf;
    bool edited = tui_handle_keys(buf, moved);
    if (edited && !sync_ready()) {
        // Still receiving the shared document; an edit now would be overwritten by it.
        buf = std::move(before);
        tui_clamp_cursor(buf);
        tui_note("joining session - try again in a moment");
        return true;
    }
    if (edited) {
        if (sync_local_edit(buf)) g_last_local_change_ns.store(now_ns(), std::memory_order_relaxed);
        tui_write_buffer(buf, last_mtime);
    }
    if (edited || moved) tui_broadcast_cursor(false);
    return edited || moved;
}

} // namespace synctext
