/**
 * @file watcher.cpp
 * @brief File watching (inotify), helper processes, terminal mode, env parsing.
 */
#include "shared.h"

namespace synctext {

// State used only by this file.
static struct termios g_term_orig{};         ///< Stores original terminal settings to restore on exit.
static bool g_tty_ok = false;                ///< True if the terminal was successfully put into raw mode.


// ============================================================================
// SECTION 6: FILE MONITORING - INOTIFY EVENT HANDLING
// ============================================================================

InotifyState init_inotify_for_file(const std::string& path) {
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
InotifyFlags drain_inotify_events(int fd) {
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
void settle_inotify_quiet(int fd, int quiet_ms, int overall_ms) {
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
bool spawn_bg_shell(const std::string& cmdline) {
    return spawn_bg_argv({"/bin/sh", "-c", cmdline});
}
void open_local_editor_if_possible() {
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
void set_terminal_rawmode() {
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
void restore_terminal_mode() {
    if (g_tty_ok) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_term_orig);
        g_tty_ok = false;
    }
}
void handle_keypresses_nonblock() {
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
uint64_t parse_env_seconds(const char* name, uint64_t def_sec) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def_sec;
    char* end = nullptr;
    unsigned long long s = std::strtoull(v, &end, 10);
    if (end == v || *end != '\0') return def_sec;
    return s;
}
int parse_env_millis(const char* name, int def_ms) {
    const char* v = std::getenv(name);
    if (!v || !*v) return def_ms;
    char* end = nullptr;
    long long ms = std::strtoll(v, &end, 10);
    if (end == v || *end != '\0') return def_ms;
    if (ms < 0) ms = 0;
    if (ms > 60000) ms = 60000; // clamp to 60s
    return (int)ms;
}
bool parse_env_bool(const char* name, bool defVal) {
    const char* v = std::getenv(name);
    if (!v) return defVal;
    if (*v == '\0') return false;
    if (v[0]=='0' || v[0]=='n' || v[0]=='N' || v[0]=='f' || v[0]=='F') return false;
    return true;
}

} // namespace synctext
