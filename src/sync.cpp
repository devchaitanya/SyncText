/**
 * @file sync.cpp
 * @brief The sync engine: connects the editor (a file, or the TUI buffer) to the CRDT.
 *
 * Every process owns one crdt::Replica, touched only by the main thread.
 *
 *   local edit   file/buffer changed -> diff against the BASELINE (what the user last
 *                saw) -> each changed range becomes "delete these nodes" + "insert
 *                after that node" -> ops are broadcast.
 *   remote ops   decoded from a CrdtOps envelope -> replica.receive_all() -> the main
 *                loop writes the replica's text to the file -> that becomes the baseline.
 *
 * The baseline keeps the node of every visible character the user is looking at.
 * Local diffs are mapped through those nodes, not through positions, so a local
 * edit lands in the right place even if remote ops were merged in between (nodes
 * never move or disappear; deleted ones are only flagged).
 *
 * Joining and repair use one message, CrdtSync = "here is my version vector".
 * A peer that has ops the sender lacks answers with a CrdtOps reply addressed to it.
 *   - a new participant sends it with the "want everything" flag and waits;
 *   - everyone sends it every few seconds (anti-entropy), which repairs ops lost to a
 *     full queue, a dropped message or a host failover.
 * The first participant of a session has nobody to ask, so it imports its file as
 * "genesis" ops whose IDs come from a hash of the text: two participants that start
 * from the same file at the same moment create identical IDs and do not duplicate it.
 */
#include "shared.h"

#include <random>

namespace synctext {

std::atomic<bool> g_sync_now{false};

// ---------------------------------------------------------------------------
// Text <-> lines (the file is a list of lines, each written with a trailing '\n')
// ---------------------------------------------------------------------------
std::string lines_to_text(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out.push_back('\n');
        out += lines[i];
    }
    return out;
}
std::vector<std::string> text_to_lines(const std::string& text) {
    std::vector<std::string> out;
    if (text.empty()) return out;
    size_t start = 0;
    for (;;) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) { out.push_back(text.substr(start)); break; }
        out.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Wire encoding
//
// A CrdtOps payload is a list of records. Typing produces chains where every op is
// the previous op's successor (same origin, next seq, next counter, anchored on the
// previous char), so such a chain is stored once with its characters as a string:
//   'I' origin seq0 ctr0 after(ctr,user) n chars[n]      insert run
//   'i' origin seq id(ctr,user) after(ctr,user) ch       single insert
//   'D' origin seq0 n id[n]                               delete run (consecutive seqs)
// Integers are little-endian; strings are a length byte plus the bytes.
// ---------------------------------------------------------------------------
namespace {

void put_u8(std::string& s, uint8_t v) { s.push_back(static_cast<char>(v)); }
void put_u32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>(v >> (8 * i))); }
void put_u64(std::string& s, uint64_t v) { for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>(v >> (8 * i))); }
void put_str(std::string& s, const std::string& v) { put_u8(s, static_cast<uint8_t>(v.size())); s += v; }
void put_id(std::string& s, const crdt::Id& id) { put_u64(s, id.ctr); put_str(s, id.user); }

struct Reader {
    const std::string& s;
    size_t off = 0;
    bool ok = true;
    bool need(size_t n) { if (off + n > s.size()) ok = false; return ok; }
    uint8_t u8() { if (!need(1)) return 0; return static_cast<uint8_t>(s[off++]); }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(s[off++])) << (8 * i); return v; }
    uint64_t u64() { if (!need(8)) return 0; uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(s[off++])) << (8 * i); return v; }
    std::string str() { size_t n = u8(); if (!need(n)) return {}; std::string v = s.substr(off, n); off += n; return v; }
    crdt::Id id() { crdt::Id i; i.ctr = u64(); i.user = str(); return i; }
};

bool extends_insert_run(const crdt::Op& prev, const crdt::Op& op) {
    return op.kind == crdt::Op::Kind::Insert && prev.kind == crdt::Op::Kind::Insert &&
           op.origin == prev.origin && op.seq == prev.seq + 1 &&
           op.id.user == op.origin && op.id.ctr == prev.id.ctr + 1 && op.after == prev.id;
}
bool extends_delete_run(const crdt::Op& prev, const crdt::Op& op) {
    return op.kind == crdt::Op::Kind::Delete && prev.kind == crdt::Op::Kind::Delete &&
           op.origin == prev.origin && op.seq == prev.seq + 1;
}

}  // namespace

std::vector<std::string> crdt_encode_ops(const std::vector<crdt::Op>& ops, size_t max_bytes) {
    std::vector<std::string> chunks;
    std::string cur;
    size_t count_at = 0;        // offset of the open run's count field
    uint32_t count = 0;         // ops in the open run (0 = no open run)
    const crdt::Op* last = nullptr;
    auto close_run = [&]() {
        if (count == 0) return;
        for (int i = 0; i < 4; ++i) cur[count_at + i] = static_cast<char>(count >> (8 * i));
        count = 0;
        last = nullptr;
    };
    auto flush_chunk = [&]() {
        close_run();
        if (!cur.empty()) chunks.push_back(std::move(cur));
        cur.clear();
    };
    for (const crdt::Op& op : ops) {
        // extend the open run when possible and there is room for one more element
        if (count > 0 && last) {
            size_t extra = (op.kind == crdt::Op::Kind::Insert) ? 1 : 9 + op.id.user.size();
            bool fits = cur.size() + extra <= max_bytes;
            if (fits && extends_insert_run(*last, op)) { cur.push_back(op.ch); ++count; last = &op; continue; }
            if (fits && extends_delete_run(*last, op)) { put_id(cur, op.id); ++count; last = &op; continue; }
        }
        close_run();
        size_t worst = 1 + 1 + op.origin.size() + 8 + 9 + op.id.user.size() + 9 + op.after.user.size() + 4 + 1;
        if (!cur.empty() && cur.size() + worst > max_bytes) flush_chunk();
        if (op.kind == crdt::Op::Kind::Insert && op.id.user == op.origin) {
            put_u8(cur, 'I'); put_str(cur, op.origin); put_u64(cur, op.seq); put_u64(cur, op.id.ctr);
            put_id(cur, op.after);
            count_at = cur.size(); put_u32(cur, 0);
            cur.push_back(op.ch);
            count = 1; last = &op;
        } else if (op.kind == crdt::Op::Kind::Insert) {
            put_u8(cur, 'i'); put_str(cur, op.origin); put_u64(cur, op.seq); put_id(cur, op.id);
            put_id(cur, op.after); cur.push_back(op.ch);
        } else {
            put_u8(cur, 'D'); put_str(cur, op.origin); put_u64(cur, op.seq);
            count_at = cur.size(); put_u32(cur, 0);
            put_id(cur, op.id);
            count = 1; last = &op;
        }
    }
    flush_chunk();
    return chunks;
}

bool crdt_decode_ops(const std::string& payload, std::vector<crdt::Op>& out) {
    Reader r{payload};
    while (r.ok && r.off < payload.size()) {
        char tag = static_cast<char>(r.u8());
        if (tag == 'I') {
            std::string origin = r.str();
            uint64_t seq0 = r.u64(), ctr0 = r.u64();
            crdt::Id after = r.id();
            uint32_t n = r.u32();
            if (!r.need(n)) break;
            for (uint32_t k = 0; k < n; ++k) {
                crdt::Op op;
                op.kind = crdt::Op::Kind::Insert;
                op.origin = origin;
                op.seq = seq0 + k;
                op.id = crdt::Id{ctr0 + k, origin};
                op.after = k == 0 ? after : crdt::Id{ctr0 + k - 1, origin};
                op.ch = payload[r.off + k];
                out.push_back(std::move(op));
            }
            r.off += n;
        } else if (tag == 'i') {
            crdt::Op op;
            op.kind = crdt::Op::Kind::Insert;
            op.origin = r.str();
            op.seq = r.u64();
            op.id = r.id();
            op.after = r.id();
            op.ch = static_cast<char>(r.u8());
            out.push_back(std::move(op));
        } else if (tag == 'D') {
            std::string origin = r.str();
            uint64_t seq0 = r.u64();
            uint32_t n = r.u32();
            for (uint32_t k = 0; k < n && r.ok; ++k) {
                crdt::Op op;
                op.kind = crdt::Op::Kind::Delete;
                op.origin = origin;
                op.seq = seq0 + k;
                op.id = r.id();
                out.push_back(std::move(op));
            }
        } else {
            return false;
        }
    }
    return r.ok;
}

std::string crdt_encode_vv(const crdt::VersionVector& vv) {
    std::string s;
    put_u32(s, static_cast<uint32_t>(vv.size()));
    for (const auto& [user, seq] : vv) { put_str(s, user); put_u64(s, seq); }
    return s;
}
bool crdt_decode_vv(const std::string& payload, crdt::VersionVector& out) {
    Reader r{payload};
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && r.ok; ++i) {
        std::string user = r.str();
        out[user] = r.u64();
    }
    return r.ok;
}

/// Origin for genesis ops: '#' plus 48 bits of an FNV-1a hash of the text. Short
/// enough to stay inside std::string's small-buffer storage, so IDs stay cheap.
std::string crdt_genesis_origin(const std::string& text) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%012llx", static_cast<unsigned long long>(h & 0xffffffffffffull));
    return buf;
}

/// A random per-session client id. Not the user name: a user who restarts gets a
/// new id, so peers never mistake the new session's ops for old duplicates.
std::string crdt_new_client_id() {
    std::random_device rd;
    uint64_t v = (static_cast<uint64_t>(rd()) << 32) ^ rd() ^ (static_cast<uint64_t>(getpid()) << 16) ^ now_ns();
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%012llx", static_cast<unsigned long long>(v & 0xffffffffffffull));
    return buf;
}

// ---------------------------------------------------------------------------
// Diff: old text -> new text as a list of character hunks
// ---------------------------------------------------------------------------
namespace {

struct Hunk { size_t pos; size_t del; std::string ins; };

// Lines with their '\n' attached (the last one has none).
std::vector<std::string> pieces_of(const std::string& t) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < t.size()) {
        size_t nl = t.find('\n', start);
        size_t end = (nl == std::string::npos) ? t.size() : nl + 1;
        out.push_back(t.substr(start, end - start));
        start = end;
    }
    return out;
}

void char_hunk(const std::string& a, const std::string& b, size_t base, std::vector<Hunk>& out) {
    size_t p = 0, n = std::min(a.size(), b.size());
    while (p < n && a[p] == b[p]) ++p;
    size_t s = 0;
    while (s < n - p && a[a.size() - 1 - s] == b[b.size() - 1 - s]) ++s;
    if (a.size() - p - s == 0 && b.size() - p - s == 0) return;
    out.push_back({base + p, a.size() - p - s, b.substr(p, b.size() - p - s)});
}

// Line-level LCS to find which lines changed, then character-level trimming inside
// each changed block so a one-character edit stays a one-character op.
std::vector<Hunk> diff_text(const std::string& oldt, const std::string& newt) {
    std::vector<Hunk> hunks;
    auto A = pieces_of(oldt), B = pieces_of(newt);
    std::vector<size_t> offA(A.size() + 1, 0);
    for (size_t i = 0; i < A.size(); ++i) offA[i + 1] = offA[i] + A[i].size();

    size_t pre = 0;
    while (pre < A.size() && pre < B.size() && A[pre] == B[pre]) ++pre;
    size_t suf = 0;
    while (suf < A.size() - pre && suf < B.size() - pre &&
           A[A.size() - 1 - suf] == B[B.size() - 1 - suf]) ++suf;
    size_t n = A.size() - pre - suf, m = B.size() - pre - suf;

    auto block = [&](size_t a0, size_t a1, size_t b0, size_t b1) {   // [a0,a1) -> [b0,b1)
        if (a1 - a0 == b1 - b0) {
            for (size_t k = 0; k < a1 - a0; ++k) char_hunk(A[a0 + k], B[b0 + k], offA[a0 + k], hunks);
        } else {
            std::string a, b;
            for (size_t k = a0; k < a1; ++k) a += A[k];
            for (size_t k = b0; k < b1; ++k) b += B[k];
            char_hunk(a, b, offA[a0], hunks);
        }
    };

    if (n == 0 && m == 0) return hunks;
    if (n == 0 || m == 0 || n * m > 4000000) {     // nothing to align, or too big to align
        block(pre, pre + n, pre, pre + m);
        return hunks;
    }
    // L[i][j] = LCS length of A[pre+i..pre+n) and B[pre+j..pre+m)
    std::vector<uint16_t> L((n + 1) * (m + 1), 0);
    auto at = [&](size_t i, size_t j) -> uint16_t& { return L[i * (m + 1) + j]; };
    for (size_t i = n; i-- > 0;)
        for (size_t j = m; j-- > 0;)
            at(i, j) = (A[pre + i] == B[pre + j]) ? static_cast<uint16_t>(at(i + 1, j + 1) + 1)
                                                  : std::max(at(i + 1, j), at(i, j + 1));
    size_t i = 0, j = 0, ci = 0, cj = 0;   // (ci, cj) = start of the current changed block
    while (i < n && j < m) {
        if (A[pre + i] == B[pre + j]) {
            if (ci < i || cj < j) block(pre + ci, pre + i, pre + cj, pre + j);
            ++i; ++j; ci = i; cj = j;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            ++i;
        } else {
            ++j;
        }
    }
    block(pre + ci, pre + n, pre + cj, pre + m);
    return hunks;
}

}  // namespace

// ---------------------------------------------------------------------------
// Engine state (main thread only)
// ---------------------------------------------------------------------------
namespace {

std::unique_ptr<crdt::Replica> g_rep;
std::string g_client;                          // our CRDT identity for this session
bool g_synced = false;                         // replica holds the shared history
bool g_reply_done = false;                     // a peer finished answering our join request
uint64_t g_sync_req_ns = 0;
int g_sync_attempts = 0;
uint64_t g_last_ae_ns = 0;
int g_sync_timeout_ms = 500;         // per attempt; a request can be lost if the peer's queue is not open yet
int g_ae_interval_ms = 2000;

std::string g_base_text;                       // what the file / buffer holds
std::vector<const crdt::Node*> g_base_nodes;   // the node of each char of g_base_text
bool g_base_valid = false;
bool g_maybe_dirty = false;                    // replica changed since the baseline was set

uint64_t g_ops_sent = 0, g_ops_received = 0;

size_t chunk_limit() {
    // A queue message is MQ_MSGSIZE bytes including the Update header, our user id
    // and the target id; TCP frames can be far larger.
    return g_network_mode ? 60000 : 1800;
}

void flush_tx() {
    while (!g_tx_pending.empty()) {
        size_t sent = broadcast_updates_to_all(g_tx_pending);
        if (sent == 0) break;
        g_tx_pending.erase(g_tx_pending.begin(), g_tx_pending.begin() + static_cast<long>(sent));
    }
}

Update envelope(Update::Type type, std::string payload, const std::string& target, bool flag) {
    Update u{};
    u.type = type;
    u.user_id = g_user_id;
    u.old_content = target;                    // CrdtOps: addressee ("" = all). CrdtSync: requester.
    u.new_content = std::move(payload);
    u.col_start = flag ? 1 : 0;                // CrdtOps: final reply chunk. CrdtSync: wants everything.
    u.timestamp_ns = now_ns();
    return u;
}

void send_ops(const std::vector<crdt::Op>& ops) {
    if (ops.empty()) return;
    for (auto& chunk : crdt_encode_ops(ops, chunk_limit()))
        g_tx_pending.push_back(envelope(Update::Type::CrdtOps, std::move(chunk), "", false));
    g_ops_sent += ops.size();
    flush_tx();
}

void send_sync_request(bool want_all) {
    Update req = envelope(Update::Type::CrdtSync, crdt_encode_vv(g_rep->version_vector()), g_client, want_all);
    // Joining over LAN: one full history is enough, so ask only the peer we joined
    // through (every peer would answer, since each one has the whole history).
    if (want_all && g_network_mode && !net_contact_peer().empty()) {
        std::vector<Update> one{std::move(req)};
        send_updates_to_user(net_contact_peer(), one);
        return;
    }
    g_tx_pending.push_back(std::move(req));
    flush_tx();
}

void set_base_from_replica() {
    g_base_text = g_rep->text();
    g_base_nodes = g_rep->visible_nodes();
    g_base_valid = true;
    g_maybe_dirty = false;
}

void become_synced_by_genesis(const std::string& reason) {
    std::string text = lines_to_text(read_file_lines(g_doc_path));
    if (!text.empty()) g_rep->import_genesis(text, crdt_genesis_origin(text));
    g_synced = true;
    set_base_from_replica();
    if (g_debug_level >= 1) std::cerr << "[sync] started from local file (" << reason << ")\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// Public engine API
// ---------------------------------------------------------------------------
void sync_start() {
    g_sync_timeout_ms = parse_env_millis("SYNCTEXT_SYNC_TIMEOUT_MS", g_sync_timeout_ms);
    g_ae_interval_ms = parse_env_millis("SYNCTEXT_ANTI_ENTROPY_MS", g_ae_interval_ms);
    if (g_ae_interval_ms < 100) g_ae_interval_ms = 100;
    g_client = crdt_new_client_id();
    g_rep = std::make_unique<crdt::Replica>(g_client);
    g_last_ae_ns = now_ns();

    bool ask_peers = g_network_mode ? !g_network_host : local_peer_alive();
    if (!ask_peers) { become_synced_by_genesis(g_network_mode ? "hosting" : "no live peers"); return; }
    g_sync_req_ns = now_ns();
    g_sync_attempts = 1;
    send_sync_request(true);
}

bool sync_ready() { return g_rep && g_synced; }

bool sync_local_edit(const std::vector<std::string>& new_lines) {
    if (!sync_ready() || !g_base_valid) return false;
    std::string new_text = lines_to_text(new_lines);
    if (new_text == g_base_text) return false;

    std::vector<Hunk> hunks = diff_text(g_base_text, new_text);
    std::vector<crdt::Op> ops;
    std::vector<std::vector<const crdt::Node*>> inserted(hunks.size());
    const crdt::Node* head = g_rep->sequence().head();
    for (size_t h = 0; h < hunks.size(); ++h) {
        const Hunk& hk = hunks[h];
        std::vector<const crdt::Node*> gone(g_base_nodes.begin() + static_cast<long>(hk.pos),
                                            g_base_nodes.begin() + static_cast<long>(hk.pos + hk.del));
        for (auto& op : g_rep->erase_nodes(gone)) ops.push_back(std::move(op));
        const crdt::Node* anchor = hk.pos > 0 ? g_base_nodes[hk.pos - 1] : head;
        for (char ch : hk.ins) {
            ops.push_back(g_rep->insert_after(anchor, ch));
            anchor = g_rep->sequence().find(ops.back().id);
            inserted[h].push_back(anchor);
        }
    }
    // the new baseline: old nodes with each hunk's range replaced by its new nodes
    std::vector<const crdt::Node*> nodes;
    nodes.reserve(g_base_nodes.size() + new_text.size());
    size_t cursor = 0;
    for (size_t h = 0; h < hunks.size(); ++h) {
        nodes.insert(nodes.end(), g_base_nodes.begin() + static_cast<long>(cursor),
                     g_base_nodes.begin() + static_cast<long>(hunks[h].pos));
        nodes.insert(nodes.end(), inserted[h].begin(), inserted[h].end());
        cursor = hunks[h].pos + hunks[h].del;
    }
    nodes.insert(nodes.end(), g_base_nodes.begin() + static_cast<long>(cursor), g_base_nodes.end());
    g_base_nodes = std::move(nodes);
    g_base_text = std::move(new_text);

    bool consistent = g_base_nodes.size() == g_base_text.size();
    for (size_t i = 0; consistent && i < g_base_nodes.size(); ++i)
        consistent = g_base_nodes[i]->ch == g_base_text[i];
    if (!consistent) {                     // never expected; recover by trusting the replica
        std::cerr << "[sync] baseline mismatch after local edit; resyncing view\n";
        g_base_valid = false;
    }
    g_maybe_dirty = true;                  // remote ops may be waiting to be written
    if (g_debug_level >= 2)
        std::cerr << "[sync] local edit: " << hunks.size() << " hunk(s), " << ops.size() << " op(s)\n";
    send_ops(ops);
    return true;
}

void sync_handle(const Update& u) {
    if (!g_rep) return;
    if (u.type == Update::Type::CrdtOps) {
        std::vector<crdt::Op> ops;
        if (!crdt_decode_ops(u.new_content, ops)) { g_rx_dropped.fetch_add(1); return; }
        g_rep->receive_all(ops);
        g_ops_received += ops.size();
        g_maybe_dirty = true;
        if (u.col_start == 1 && u.old_content == g_client) g_reply_done = true;
        return;
    }
    if (u.type != Update::Type::CrdtSync) return;
    const std::string& requester = u.old_content;
    if (requester == g_client) return;
    bool wants_all = u.col_start == 1;
    if (!g_synced) {
        // Two participants that start together both see each other and both wait for
        // an answer that only a synced peer gives. Break the tie deterministically:
        // the smaller user id founds the session from its file and answers.
        if (!(wants_all && !g_network_mode && g_user_id < u.user_id)) return;
        become_synced_by_genesis("founding the session");
    }
    crdt::VersionVector theirs;
    if (!crdt_decode_vv(u.new_content, theirs)) return;
    std::vector<crdt::Op> missing = g_rep->missing_for(theirs);
    if (missing.empty() && !wants_all) return;

    std::vector<std::string> chunks = crdt_encode_ops(missing, chunk_limit());
    if (chunks.empty()) chunks.emplace_back();   // an empty final reply still says "done"
    std::vector<Update> reply;
    for (size_t i = 0; i < chunks.size(); ++i)
        reply.push_back(envelope(Update::Type::CrdtOps, std::move(chunks[i]), requester,
                                 wants_all && i + 1 == chunks.size()));
    send_updates_to_user(u.user_id, reply);
    if (g_debug_level >= 1)
        std::cerr << "[sync] sent " << missing.size() << " ops to " << u.user_id << "\n";
}

void sync_tick() {
    if (!g_rep) return;
    uint64_t now = now_ns();
    if (!g_synced) {
        if (g_reply_done && g_rep->pending() == 0) {
            g_synced = true;
            g_base_valid = false;          // forces the main loop to write the shared text
            g_maybe_dirty = true;
            if (g_debug_level >= 1) std::cerr << "[sync] joined with " << g_ops_received << " ops\n";
        } else if ((now - g_sync_req_ns) / 1000000ull >= static_cast<uint64_t>(g_sync_timeout_ms)) {
            bool someone = g_network_mode || local_peer_alive();
            if (someone && g_sync_attempts < 8) {
                ++g_sync_attempts;
                g_sync_req_ns = now;
                send_sync_request(true);
            } else {
                become_synced_by_genesis("no reply from peers");
            }
        }
        return;
    }
    bool due = (now - g_last_ae_ns) / 1000000ull >= static_cast<uint64_t>(g_ae_interval_ms);
    if (due || g_sync_now.exchange(false)) {
        g_last_ae_ns = now;
        send_sync_request(false);
    }
}

bool sync_dirty() {
    if (!sync_ready()) return false;
    if (!g_base_valid) return true;
    if (!g_maybe_dirty) return false;
    if (g_rep->text() == g_base_text) { g_maybe_dirty = false; return false; }
    return true;
}

std::vector<std::string> sync_take_lines(size_t* caret_line, size_t* caret_col) {
    const crdt::Node* head = g_rep->sequence().head();
    const crdt::Node* anchor = nullptr;
    if (caret_line && caret_col && g_base_valid) {
        // caret (line, col) in the old baseline -> the node just before it
        size_t pos = 0, line = 0;
        while (line < *caret_line && pos < g_base_text.size()) {
            size_t nl = g_base_text.find('\n', pos);
            if (nl == std::string::npos) { pos = g_base_text.size(); break; }
            pos = nl + 1;
            ++line;
        }
        size_t line_end = g_base_text.find('\n', pos);
        if (line_end == std::string::npos) line_end = g_base_text.size();
        pos = std::min(pos + *caret_col, line_end);
        anchor = pos > 0 ? g_base_nodes[pos - 1] : head;
    }
    set_base_from_replica();
    if (anchor) {
        size_t pos = 0;
        if (anchor != head) {
            crdt::Pos p = g_rep->sequence().position(anchor);
            pos = p.pos + (anchor->deleted ? 0 : 1);
        }
        size_t line = 0, col = 0;
        for (size_t i = 0; i < pos && i < g_base_text.size(); ++i) {
            if (g_base_text[i] == '\n') { ++line; col = 0; } else ++col;
        }
        *caret_line = line;
        *caret_col = col;
    }
    return text_to_lines(g_base_text);
}

std::string sync_status() {
    if (!g_rep) return "";
    std::string s = g_synced ? "crdt" : "crdt syncing";
    s += " sent=" + std::to_string(g_ops_sent) + " recv=" + std::to_string(g_ops_received);
    if (g_rep->pending()) s += " waiting=" + std::to_string(g_rep->pending());
    return s;
}

} // namespace synctext
