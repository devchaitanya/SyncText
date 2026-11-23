// bench_rga.cpp - micro-benchmarks for the CRDT (src/rga.*) and the wire encoding (src/sync.cpp).
// Build/run: make perf   (from tests/crdt)
#include "../../src/shared.h"
#include <netinet/tcp.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>

using namespace crdt;
using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
static long rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string k; long v = 0;
    while (f >> k) if (k == "VmRSS:") { f >> v; break; }
    return v;
}
static std::string doc_text(size_t n) {          // 40-char lines
    std::string t;
    for (size_t i = 0; i < n; ++i) t.push_back(i % 40 == 39 ? '\n' : static_cast<char>('a' + i % 26));
    return t;
}

// Resident memory of one replica holding n chars, then after deleting half of them.
static int measure_memory(size_t n) {
    long before = rss_kb();
    Replica a("aaaaaaaaaaaa");
    a.import_genesis(doc_text(n), "#base00000000");
    long after = rss_kb();
    auto nodes = a.visible_nodes();
    a.erase_nodes(std::vector<const Node*>(nodes.begin(), nodes.begin() + static_cast<long>(n / 2)));
    long after_del = rss_kb();
    std::printf("  memory, %7zu chars      : %.1f MB (%.0f bytes/char); deleting half adds %.1f MB\n", n,
                (after - before) / 1024.0, (after - before) * 1024.0 / static_cast<double>(n),
                (after_del - after) / 1024.0);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--mem") return measure_memory(std::stoul(argv[2]));
    std::mt19937 rng(7);
    std::printf("== CRDT core (single replica, in memory) ==\n");

    // 1. typing at the end
    {
        const size_t N = 200000;
        Replica a("aaaaaaaaaaaa");
        auto t0 = Clock::now();
        for (size_t i = 0; i < N; ++i) {
            size_t row = a.lines().size() - 1;
            a.insert_char(row, a.lines()[row].size(), i % 40 == 39 ? '\n' : 'x');
        }
        double ms = ms_since(t0);
        std::printf("  local typing at end      : %7.0f ns/keystroke  (%.1fM keystrokes/s, %zu chars)\n",
                    ms * 1e6 / N, N / ms / 1e3, N);
    }
    // 2. typing at random places in a 100k-char document
    {
        Replica a("aaaaaaaaaaaa");
        a.import_genesis(doc_text(100000), "#base00000000");
        const size_t N = 50000;
        auto t0 = Clock::now();
        for (size_t i = 0; i < N; ++i) {
            size_t row = rng() % a.lines().size();
            size_t col = rng() % (a.lines()[row].size() + 1);
            a.insert_char(row, col, 'y');
        }
        double ms = ms_since(t0);
        std::printf("  local typing, random spot: %7.0f ns/keystroke  (100k-char doc)\n", ms * 1e6 / N);
    }
    // 3. applying remote ops
    {
        Replica a("aaaaaaaaaaaa");
        a.import_genesis(doc_text(100000), "#base00000000");
        for (size_t i = 0; i < 50000; ++i) {
            size_t row = rng() % a.lines().size();
            size_t col = rng() % (a.lines()[row].size() + 1);
            if (i % 4 == 3 && col < a.lines()[row].size()) a.erase(row, col, 1);
            else a.insert_char(row, col, 'z');
        }
        auto ops = a.missing_for({});
        Replica b("bbbbbbbbbbbb"), c("cccccccccccc");
        auto t0 = Clock::now();
        b.receive_all(ops);                                   // one batch (view rebuilt once)
        double batch = ms_since(t0);
        t0 = Clock::now();
        for (const Op& op : ops) c.receive(op);               // one op at a time (view patched each time)
        double single = ms_since(t0);
        std::printf("  remote ops, batched      : %7.0f ns/op  (%zu ops = full join of a 150k-op history: %.0f ms)\n",
                    batch * 1e6 / ops.size(), ops.size(), batch);
        std::printf("  remote ops, one at a time: %7.0f ns/op\n", single * 1e6 / ops.size());
    }
    // 4. node -> (row, col)
    {
        std::printf("  node -> (row,col) lookup :");
        for (size_t n : {10000u, 100000u, 1000000u}) {
            Replica a("aaaaaaaaaaaa");
            a.import_genesis(doc_text(n), "#base00000000");
            auto nodes = a.visible_nodes();
            std::vector<const Node*> probe;
            for (int i = 0; i < 1000; ++i) probe.push_back(nodes[rng() % nodes.size()]);
            size_t sink = 0;
            auto t0 = Clock::now();
            for (const Node* p : probe) sink += a.sequence().position(p).row;
            double tree = ms_since(t0) * 1e6 / probe.size();
            t0 = Clock::now();
            for (int i = 0; i < 20; ++i) {
                size_t row = 0;
                for (const Node* x = a.sequence().head()->next; x != probe[i]; x = x->next) row += (x->ch == '\n');
                sink += row;
            }
            double walk = ms_since(t0) * 1e6 / 20;
            std::printf("  n=%zu: tree %.0f ns vs walk %.0f us%s", n, tree, walk / 1000, n == 1000000u ? "\n" : " |");
            if (sink == 42) std::printf(" ");
        }
    }
    // 5. memory
    {
        // Memory is measured in a separate run (--mem N): freed memory stays in the
        // allocator, so measuring after the sections above would undercount.
    }

    std::printf("== Wire encoding (sync.cpp) ==\n");
    {
        // a typed run: 1000 consecutive keystrokes
        Replica a("aaaaaaaaaaaa");
        std::vector<Op> run;
        for (int i = 0; i < 1000; ++i) run.push_back(a.insert_char(0, static_cast<size_t>(i), 'q'));
        size_t run_bytes = 0;
        for (auto& c : synctext::crdt_encode_ops(run, 1800)) run_bytes += c.size();
        // one isolated keystroke
        size_t one = synctext::crdt_encode_ops({run[500]}, 1800)[0].size();
        // scattered single-char edits
        Replica b("bbbbbbbbbbbb");
        b.import_genesis(doc_text(20000), "#base00000000");
        std::vector<Op> scattered;
        for (int i = 0; i < 1000; ++i) {
            size_t row = rng() % b.lines().size();
            scattered.push_back(b.insert_char(row, rng() % (b.lines()[row].size() + 1), 'w'));
        }
        size_t sc_bytes = 0;
        for (auto& c : synctext::crdt_encode_ops(scattered, 1800)) sc_bytes += c.size();
        // a whole-document join reply (genesis run of 100k chars)
        Replica g("gggggggggggg");
        g.import_genesis(doc_text(100000), "#base00000000");
        auto hist = g.missing_for({});
        auto t0 = Clock::now();
        auto chunks = synctext::crdt_encode_ops(hist, 60000);
        double enc = ms_since(t0);
        size_t join_bytes = 0;
        for (auto& c : chunks) join_bytes += c.size();
        std::vector<Op> back;
        t0 = Clock::now();
        for (auto& c : chunks) synctext::crdt_decode_ops(c, back);
        double dec = ms_since(t0);
        std::printf("  one keystroke            : %zu bytes payload\n", one);
        std::printf("  typed run of 1000 chars  : %.1f bytes/char (run-length records)\n", run_bytes / 1000.0);
        std::printf("  1000 scattered keystrokes: %.1f bytes/char\n", sc_bytes / 1000.0);
        std::printf("  join reply, 100k-char doc: %.0f KB (%.2f bytes/char); encode %.1f ms, decode %.1f ms\n",
                    join_bytes / 1024.0, static_cast<double>(join_bytes) / 100000, enc, dec);
    }

    std::printf("== Transport primitives (two processes, ping-pong, %d round trips) ==\n", 20000);
    {
        const int N = 20000;
        // POSIX message queues, blocking receive
        mq_unlink("/stbench_ping"); mq_unlink("/stbench_pong");
        struct mq_attr attr{}; attr.mq_maxmsg = 10; attr.mq_msgsize = 128;
        mqd_t ping = mq_open("/stbench_ping", O_CREAT | O_RDWR, 0600, &attr);
        mqd_t pong = mq_open("/stbench_pong", O_CREAT | O_RDWR, 0600, &attr);
        char buf[128] = "x";
        pid_t pid = fork();
        if (pid == 0) {
            for (int i = 0; i < N; ++i) { mq_receive(ping, buf, sizeof(buf), nullptr); mq_send(pong, buf, 56, 0); }
            _exit(0);
        }
        auto t0 = Clock::now();
        for (int i = 0; i < N; ++i) { mq_send(ping, buf, 56, 0); mq_receive(pong, buf, sizeof(buf), nullptr); }
        double rtt_us = ms_since(t0) * 1000 / N;
        waitpid(pid, nullptr, 0);
        mq_close(ping); mq_close(pong); mq_unlink("/stbench_ping"); mq_unlink("/stbench_pong");
        std::printf("  POSIX mq, 56-byte message: %.1f us round trip (~%.1f us one way)\n", rtt_us, rtt_us / 2);

        // TCP over loopback, the LAN transport
        int lfd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(lfd, (sockaddr*)&addr, sizeof(addr)); listen(lfd, 1);
        socklen_t len = sizeof(addr); getsockname(lfd, (sockaddr*)&addr, &len);
        pid = fork();
        if (pid == 0) {
            int c = socket(AF_INET, SOCK_STREAM, 0);
            connect(c, (sockaddr*)&addr, sizeof(addr));
            int one = 1; setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            for (int i = 0; i < N; ++i) { recv(c, buf, 61, MSG_WAITALL); send(c, buf, 61, 0); }
            _exit(0);
        }
        int s = accept(lfd, nullptr, nullptr);
        int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        t0 = Clock::now();
        for (int i = 0; i < N; ++i) { send(s, buf, 61, 0); recv(s, buf, 61, MSG_WAITALL); }
        rtt_us = ms_since(t0) * 1000 / N;
        waitpid(pid, nullptr, 0);
        close(s); close(lfd);
        std::printf("  TCP loopback, 61-byte frame: %.1f us round trip (~%.1f us one way)\n", rtt_us, rtt_us / 2);
    }
    return 0;
}
