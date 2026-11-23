/**
 * @file fileio.cpp
 * @brief Reading and writing the document file.
 */
#include "shared.h"

namespace synctext {

std::vector<std::string> read_file_lines(const std::string& path) {
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

// Read lines with basic stabilization: ensure file's mtime and size are
// unchanged before and after the read, or give up after max_wait_ms.
std::vector<std::string> read_file_lines_stable(const std::string& path, int max_wait_ms) {
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
void write_initial_doc(const std::string& path) {
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

} // namespace synctext
