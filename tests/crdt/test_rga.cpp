// test_rga.cpp - tests for the sequence CRDT.
//
//   ./test_rga            run all tests
//   ./test_rga --bench    also run a small position() benchmark
#include "../../src/rga.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace crdt;

static int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                        \
    } while (0)

#define CHECK_EQ(a, b)                                                           \
    do {                                                                         \
        ++g_checks;                                                              \
        auto va = (a);                                                           \
        auto vb = (b);                                                           \
        if (!(va == vb)) {                                                       \
            ++g_failures;                                                        \
            std::printf("  FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                        \
    } while (0)

#define CHECK_OK(replica) CHECK_EQ((replica).check_invariants(), std::string())

// Type a string at (row, col), returning the ops.
static std::vector<Op> type(Replica& r, size_t row, size_t col, const std::string& s) {
    std::vector<Op> ops;
    for (char ch : s) {
        ops.push_back(r.insert_char(row, col, ch));
        if (ch == '\n') { ++row; col = 0; }
        else ++col;
    }
    return ops;
}

static void append(std::vector<Op>& to, const std::vector<Op>& from) {
    to.insert(to.end(), from.begin(), from.end());
}

// Every permutation of `ops` delivered to a fresh copy of the base document must
// give the same text. Returns that text.
static std::string all_orders_agree(const std::vector<Op>& base, std::vector<Op> ops) {
    std::sort(ops.begin(), ops.end(), [](const Op& a, const Op& b) {
        return a.origin != b.origin ? a.origin < b.origin : a.seq < b.seq;
    });
    std::string first;
    bool have_first = false;
    do {
        Replica d("observer");
        d.receive_all(base);
        d.receive_all(ops);
        CHECK_OK(d);
        CHECK_EQ(d.pending(), size_t{0});
        if (!have_first) { first = d.text(); have_first = true; }
        else CHECK_EQ(d.text(), first);
    } while (std::next_permutation(ops.begin(), ops.end(), [](const Op& a, const Op& b) {
        return a.origin != b.origin ? a.origin < b.origin : a.seq < b.seq;
    }));
    return first;
}

// ---------------------------------------------------------------------------

static void test_local_typing() {
    Replica a("A");
    type(a, 0, 0, "hello");
    type(a, 0, 5, "\nworld");
    CHECK_EQ(a.text(), std::string("hello\nworld"));
    CHECK_EQ(a.lines().size(), size_t{2});

    a.insert_char(0, 0, '>');          // ">hello"
    a.insert_char(1, 5, '!');          // "world!"
    CHECK_EQ(a.text(), std::string(">hello\nworld!"));

    a.erase(0, 0, 1);                  // drop '>'
    a.erase(0, 3, 6);                  // "lo\nwor" -> "helld!"
    CHECK_EQ(a.text(), std::string("helld!"));
    CHECK_EQ(a.lines().size(), size_t{1});
    CHECK_OK(a);
}

// Round 1 of the worked example: same spot, same counter.
static void test_same_spot_same_counter() {
    Replica a("A"), b("B");
    auto base = type(a, 0, 0, "cat");
    b.receive_all(base);

    Op s = a.insert_char(0, 3, 's');   // (4,A) after t
    Op x = b.insert_char(0, 3, '!');   // (4,B) after t
    CHECK_EQ(s.id.ctr, x.id.ctr);      // counters collide...
    CHECK(s.id != x.id);               // ...IDs do not

    a.receive(x);
    b.receive(s);
    CHECK_EQ(a.text(), b.text());
    CHECK_EQ(a.text(), std::string("cat!s"));   // bigger ID (4,B) goes first
    CHECK_EQ(all_orders_agree(base, {s, x}), std::string("cat!s"));
    CHECK_OK(a);
    CHECK_OK(b);
}

// The three-op example: delete b, Y after a, Z after b. Unlinking failed this one.
static void test_delete_vs_concurrent_inserts() {
    Replica a("A"), b("B"), c("C");
    auto base = type(a, 0, 0, "abc");
    b.receive_all(base);
    c.receive_all(base);

    auto del = a.erase(0, 1, 1);           // delete b
    Op y = b.insert_char(0, 1, 'Y');       // Y after a
    Op z = c.insert_char(0, 2, 'Z');       // Z after b

    std::vector<Op> ops = del;
    ops.push_back(y);
    ops.push_back(z);
    CHECK_EQ(all_orders_agree(base, ops), std::string("aYZc"));
}

// A run typed by one user is not interleaved with a concurrent insert.
static void test_runs_do_not_interleave() {
    Replica a("A"), b("B");
    auto base = type(a, 0, 0, "ab");
    b.receive_all(base);

    auto yw = type(b, 0, 1, "YW");   // Y after a, W after Y
    Op x = a.insert_char(0, 1, 'X'); // X after a

    std::vector<Op> ops = yw;
    ops.push_back(x);
    CHECK_EQ(all_orders_agree(base, ops), std::string("aYWXb"));
}

// Edits made after an insert above them still hit the right line.
static void test_insert_above_does_not_shift_edits() {
    Replica a("A"), b("B");
    auto base = type(a, 0, 0, "apple\nbanana\ncherry");
    b.receive_all(base);

    auto ea = a.erase(1, 0, 6);
    append(ea, type(a, 1, 0, "BANANA"));     // replace line 1
    auto eb = type(b, 0, 0, "fruit:\n");     // new top line

    a.receive_all(eb);
    b.receive_all(ea);
    CHECK_EQ(a.text(), std::string("fruit:\napple\nBANANA\ncherry"));
    CHECK_EQ(b.text(), a.text());
    CHECK_OK(a);
    CHECK_OK(b);
}

// Same character deleted by two users at once.
static void test_double_delete() {
    Replica a("A"), b("B");
    auto base = type(a, 0, 0, "xyz");
    b.receive_all(base);
    auto da = a.erase(0, 1, 1);
    auto db = b.erase(0, 1, 1);
    a.receive_all(db);
    b.receive_all(da);
    CHECK_EQ(a.text(), std::string("xz"));
    CHECK_EQ(b.text(), std::string("xz"));
    CHECK_OK(a);
    CHECK_OK(b);
}

// Joining two lines while someone types at the start of the second one.
static void test_join_lines_vs_insert() {
    Replica a("A"), b("B");
    auto base = type(a, 0, 0, "ab\ncd");
    b.receive_all(base);
    auto join = a.erase(0, 2, 1);          // delete the newline
    Op x = b.insert_char(1, 0, 'x');       // x after the newline
    a.receive(x);
    b.receive_all(join);
    CHECK_EQ(a.text(), std::string("abxcd"));
    CHECK_EQ(b.text(), a.text());
    CHECK_OK(a);
    CHECK_OK(b);
}

// Duplicates are ignored; ops that arrive before their anchor wait.
static void test_duplicates_and_out_of_order() {
    Replica a("A"), b("B");
    auto ops = type(a, 0, 0, "hello");
    std::reverse(ops.begin(), ops.end());   // worst possible order
    for (const Op& op : ops) {
        b.receive(op);
        b.receive(op);                       // and every op twice
    }
    CHECK_EQ(b.text(), std::string("hello"));
    CHECK_EQ(b.pending(), size_t{0});
    CHECK_OK(b);
}

// Lost messages are recovered by exchanging version vectors.
static void test_anti_entropy_recovers_losses() {
    Replica a("A"), b("B");
    auto ops = type(a, 0, 0, "lost and found");
    for (size_t i = 0; i < ops.size(); ++i)
        if (i % 3 != 0) b.receive(ops[i]);   // a third of the messages never arrive
    CHECK(b.text() != a.text());

    b.receive_all(a.missing_for(b.version_vector()));
    CHECK_EQ(b.text(), a.text());
    CHECK_EQ(b.pending(), size_t{0});
    CHECK_OK(b);
}

// A late op slots into place: same result as on-time delivery.
static void test_late_arrival() {
    Replica a("A"), b("B"), c("C");
    auto base = type(a, 0, 0, "abc");
    b.receive_all(base);
    c.receive_all(base);
    auto del = a.erase(0, 1, 1);
    Op y = b.insert_char(0, 1, 'Y');
    Op z = c.insert_char(0, 2, 'Z');

    a.receive(z);                 // A sees only Z for a while
    CHECK_EQ(a.text(), std::string("aZc"));
    a.receive(y);                 // Y shows up late
    CHECK_EQ(a.text(), std::string("aYZc"));
    (void)del;
    CHECK_OK(a);
}

// A big document with tiny leaves: many splits, a deep tree, batched range delete.
static void test_tree_splits_and_range_delete() {
    Replica a("A", /*leaf_max=*/4, /*fanout=*/3);
    std::string expect;
    for (int i = 0; i < 300; ++i) {
        char ch = (i % 17 == 16) ? '\n' : static_cast<char>('a' + i % 26);
        size_t row = a.lines().size() - 1;
        a.insert_char(row, a.lines()[row].size(), ch);
        expect.push_back(ch);
    }
    CHECK_EQ(a.text(), expect);
    CHECK(a.sequence().height() >= 4);
    CHECK_OK(a);

    a.erase(2, 3, 120);                       // spans many leaves and rows
    CHECK_OK(a);
    Replica b("B", 4, 3);
    b.receive_all(a.missing_for({}));
    CHECK_EQ(b.text(), a.text());
    CHECK_OK(b);
}

// Editing by node: the anchors stay right even after remote ops moved the text.
static void test_edit_by_node() {
    Replica a("A"), b("B");
    auto base = type(a, 0, 0, "abc");
    b.receive_all(base);
    auto seen = b.visible_nodes();                 // what B's user is looking at: a b c

    a.receive(b.insert_char(0, 0, '>'));           // (concurrently-ish) B... and A adds text:
    auto ax = type(a, 0, 1, "XX");                 // A: ">XXabc"
    b.receive_all(ax);

    // B's user edits relative to the old snapshot: insert '!' after 'b', delete 'c'.
    std::vector<Op> ops;
    ops.push_back(b.insert_after(seen[1], '!'));
    append(ops, b.erase_nodes({seen[2]}));
    append(ops, b.erase_nodes({seen[2]}));         // deleting twice produces nothing
    CHECK_EQ(ops.size(), size_t{2});
    a.receive_all(ops);
    CHECK_EQ(b.text(), std::string(">XXab!"));
    CHECK_EQ(a.text(), b.text());
    CHECK_OK(a);
    CHECK_OK(b);
}

// Two replicas importing the same file get the same IDs, so syncing does not duplicate.
static void test_genesis_is_deterministic() {
    Replica a("A"), b("B");
    a.import_genesis("line one\nline two", "#base");
    b.import_genesis("line one\nline two", "#base");
    auto ea = type(a, 0, 0, "A:");
    auto eb = type(b, 1, 0, "B:");
    b.receive_all(a.missing_for(b.version_vector()));
    a.receive_all(eb);
    CHECK_EQ(a.text(), std::string("A:line one\nB:line two"));
    CHECK_EQ(b.text(), a.text());
    (void)ea;
    CHECK_OK(a);
    CHECK_OK(b);
}

// A large batch (view rebuilt once) gives the same result as op-by-op delivery.
static void test_big_batch_and_big_erase() {
    Replica a("A", 8, 4);
    std::string text;
    for (int i = 0; i < 2000; ++i) text.push_back(i % 30 == 29 ? '\n' : static_cast<char>('a' + i % 26));
    a.import_genesis(text, "#g");
    auto all = a.visible_nodes();
    std::vector<const Node*> middle(all.begin() + 100, all.begin() + 1500);
    auto del = a.erase_nodes(middle);              // > 64 nodes: batched path
    CHECK_EQ(del.size(), size_t{1400});
    CHECK_OK(a);

    Replica b("B", 8, 4), c("C", 8, 4);
    auto everything = a.missing_for({});
    b.receive_all(everything);                     // batched
    for (const Op& op : everything) c.receive(op); // one by one
    CHECK_EQ(b.text(), a.text());
    CHECK_EQ(c.text(), a.text());
    CHECK_OK(b);
    CHECK_OK(c);
}

// missing_for() returns ops in an order a fresh replica can apply without buffering.
static void test_history_is_causal() {
    Replica a("A"), b("B"), c("C");
    auto base = type(a, 0, 0, "ab");
    b.receive_all(base);
    auto yb = type(b, 0, 1, "Y");      // B anchors on A's node
    a.receive_all(yb);
    auto za = type(a, 0, 2, "Z");      // A anchors on B's node
    for (const Op& op : a.missing_for({})) {
        c.receive(op);
        CHECK_EQ(c.pending(), size_t{0});
    }
    CHECK_EQ(c.text(), a.text());
    (void)za;
}

// ---------------------------------------------------------------------------
// Fuzz: N replicas, random edits, a network that drops, duplicates and reorders.
// ---------------------------------------------------------------------------
static void fuzz(unsigned seed, int replicas, int steps, size_t leaf_max, size_t fanout) {
    std::mt19937 rng(seed);
    auto chance = [&](int pct) { return static_cast<int>(rng() % 100) < pct; };

    std::vector<std::unique_ptr<Replica>> rs;
    for (int i = 0; i < replicas; ++i)
        rs.push_back(std::make_unique<Replica>(std::string(1, static_cast<char>('A' + i)),
                                               leaf_max, fanout));
    std::vector<std::vector<Op>> inbox(replicas);

    auto broadcast = [&](int from, const std::vector<Op>& ops) {
        for (int to = 0; to < replicas; ++to) {
            if (to == from) continue;
            for (const Op& op : ops) {
                if (chance(10)) continue;                  // dropped
                inbox[to].push_back(op);
                if (chance(5)) inbox[to].push_back(op);    // duplicated
            }
        }
    };

    for (int step = 0; step < steps; ++step) {
        int who = static_cast<int>(rng() % replicas);
        Replica& r = *rs[who];
        if (chance(55)) {                                  // local edit
            size_t row = rng() % r.lines().size();
            size_t col = rng() % (r.lines()[row].size() + 1);
            if (chance(70) || r.text().empty()) {
                const char* alphabet = "abcdef\n";
                broadcast(who, {r.insert_char(row, col, alphabet[rng() % 7])});
            } else {
                broadcast(who, r.erase(row, col, 1 + rng() % 6));
            }
        } else if (!inbox[who].empty()) {                  // deliver a random message
            size_t i = rng() % inbox[who].size();
            Op op = inbox[who][i];
            inbox[who].erase(inbox[who].begin() + static_cast<long>(i));
            r.receive(op);
        }
        if (step % 97 == 0) {
            for (auto& x : rs) CHECK_EQ(x->check_invariants(false), std::string());
        }
    }

    // deliver what is still in flight, then repair losses with anti-entropy
    for (int to = 0; to < replicas; ++to) rs[to]->receive_all(inbox[to]);
    for (int round = 0; round < 2; ++round)
        for (auto& x : rs)
            for (auto& y : rs)
                if (x != y) x->receive_all(y->missing_for(x->version_vector()));

    for (auto& x : rs) {
        CHECK_EQ(x->text(), rs[0]->text());
        CHECK_EQ(x->pending(), size_t{0});
        CHECK_EQ(x->version_vector(), rs[0]->version_vector());
        CHECK_OK(*x);
    }
}

// ---------------------------------------------------------------------------

static void bench() {
    for (size_t n : {10000u, 100000u, 1000000u}) {
        Replica a("A");
        for (size_t i = 0; i < n; ++i) {
            size_t row = a.lines().size() - 1;
            a.insert_char(row, a.lines()[row].size(), (i % 40 == 39) ? '\n' : 'x');
        }
        const Sequence& s = a.sequence();
        // probe nodes spread over the document
        std::vector<const Node*> probes;
        size_t i = 0;
        for (const Node* x = s.head()->next; x; x = x->next, ++i)
            if (i % (n / 100) == 0) probes.push_back(x);

        auto t0 = std::chrono::steady_clock::now();
        size_t sink = 0;
        for (int rep = 0; rep < 100; ++rep)
            for (const Node* p : probes) sink += s.position(p).row;
        auto t1 = std::chrono::steady_clock::now();
        for (const Node* p : probes) {
            size_t row = 0;
            for (const Node* x = s.head()->next; x != p; x = x->next) row += (x->ch == '\n');
            sink += row;
        }
        auto t2 = std::chrono::steady_clock::now();

        double tree_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / (100.0 * probes.size());
        double walk_us = std::chrono::duration<double, std::micro>(t2 - t1).count() / probes.size();
        std::printf("  n=%-8zu height=%zu  position(): %7.3f us   linear walk: %9.3f us   (sink %zu)\n",
                    n, s.height(), tree_us, walk_us, sink % 10);
    }
}

int main(int argc, char** argv) {
    struct T { const char* name; std::function<void()> fn; };
    std::vector<T> tests = {
        {"local typing and erase", test_local_typing},
        {"same spot, same counter", test_same_spot_same_counter},
        {"delete vs concurrent inserts (all orders)", test_delete_vs_concurrent_inserts},
        {"runs do not interleave (all orders)", test_runs_do_not_interleave},
        {"insert above does not shift edits", test_insert_above_does_not_shift_edits},
        {"double delete", test_double_delete},
        {"join lines vs insert", test_join_lines_vs_insert},
        {"duplicates and out-of-order delivery", test_duplicates_and_out_of_order},
        {"anti-entropy recovers lost ops", test_anti_entropy_recovers_losses},
        {"late arrival", test_late_arrival},
        {"tree splits and batched range delete", test_tree_splits_and_range_delete},
        {"edit by node after remote changes", test_edit_by_node},
        {"genesis import is deterministic", test_genesis_is_deterministic},
        {"big batch and big erase", test_big_batch_and_big_erase},
        {"history replays in causal order", test_history_is_causal},
    };
    for (unsigned seed = 1; seed <= 30; ++seed) {
        tests.push_back({nullptr, [seed] { fuzz(seed, 4, 3000, 4, 3); }});
    }
    tests.push_back({"fuzz: 5 replicas, default tree sizes", [] { fuzz(99, 5, 6000, 64, 16); }});

    int fuzz_runs = 0, fuzz_failures = 0;
    for (const T& t : tests) {
        int before = g_failures;
        t.fn();
        if (t.name) {
            std::printf("%s  %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
        } else {
            ++fuzz_runs;
            fuzz_failures += g_failures - before;
        }
    }
    std::printf("%s  fuzz: %d seeds x 4 replicas x 3000 steps, tiny tree (lossy, reordering network)\n",
                fuzz_failures == 0 ? "ok  " : "FAIL", fuzz_runs);
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);

    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
        std::printf("\nbenchmark: cost to find a node's row/col\n");
        bench();
    }
    return g_failures == 0 ? 0 : 1;
}
