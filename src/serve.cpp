/**
 * @file serve.cpp
 * @brief Multi-document server (--serve).
 */
#include "shared.h"

namespace synctext {


// ============================================================================
// SECTION 4b: MULTI-DOCUMENT SERVER (--serve)
//
// A --host process *is* an editor, so it can only share the one document it has
// open. --serve is the other shape: a headless process that holds several
// documents and relays between their participants without editing any of them.
// It keeps a replica of each document on disk purely so a late joiner has
// something to be seeded with.
// ============================================================================

static std::vector<std::unique_ptr<ServedDoc>> g_served;
static std::mutex g_serve_threads_mu;
static std::vector<std::pair<std::shared_ptr<NetworkClient>, std::thread>> g_serve_threads;
static ServedDoc* served_find(const std::string& id) {
    for (auto& d : g_served) if (d->id == id) return d.get();
    return nullptr;
}
static std::string served_list() {
    std::string out;
    for (size_t i = 0; i < g_served.size(); ++i) {
        if (i) out += ", ";
        out += g_served[i]->id.empty() ? "(default)" : g_served[i]->id;
    }
    return out;
}

/// Roster for one document: this server first, then its participants.
/// Caller holds d->mu.
static std::string served_roster_blob(ServedDoc* d) {
    std::string out = g_user_id + "\t\t" + std::to_string(g_listen_port) + "\n";
    for (const auto& c : d->clients) {
        out += c->user_id + "\t" + c->ip + "\t" + std::to_string(c->port) + "\n";
    }
    return out;
}
static void served_broadcast_roster(ServedDoc* d) {
    std::lock_guard<std::mutex> lock(d->mu);
    std::string blob = served_roster_blob(d);
    for (const auto& c : d->clients) send_frame(c->fd, ROSTER, blob.data(), blob.size());
}

/// Save the server's replica so the on-disk copy (what late joiners are first shown)
/// matches the shared document. Caller holds d->mu.
static void served_write(ServedDoc* d) {
    FILE* f = std::fopen(d->path.c_str(), "wb");
    if (!f) return;
    for (const auto& line : text_to_lines(d->replica->text())) {
        std::fwrite(line.c_str(), 1, line.size(), f);
        std::fwrite("\n", 1, 1, f);
    }
    std::fclose(f);
}
/// The server is a full participant in the CRDT, just without an editor: it applies
/// every op to its own replica and answers version-vector requests from that replica,
/// so it can seed and repair clients even when no other client is connected.
/// Returns true if the message should be relayed to the document's other clients.
static bool served_handle(ServedDoc* d, const std::shared_ptr<NetworkClient>& from, const Update& u) {
    if (u.type == Update::Type::CrdtOps) {
        std::vector<crdt::Op> ops;
        if (!crdt_decode_ops(u.new_content, ops)) return false;
        std::string before = d->replica->text();
        d->replica->receive_all(ops);
        if (d->replica->text() != before) served_write(d);
        return true;
    }
    if (u.type == Update::Type::CrdtSync) {
        crdt::VersionVector theirs;
        if (!crdt_decode_vv(u.new_content, theirs)) return false;
        bool wants_all = u.col_start == 1;
        auto missing = d->replica->missing_for(theirs);
        if (!missing.empty() || wants_all) {
            auto chunks = crdt_encode_ops(missing, 60000);
            if (chunks.empty()) chunks.emplace_back();
            for (size_t i = 0; i < chunks.size(); ++i) {
                Update r{};
                r.type = Update::Type::CrdtOps;
                r.user_id = g_user_id;
                r.old_content = u.old_content;            // addressed to the requester
                r.new_content = std::move(chunks[i]);
                r.col_start = (wants_all && i + 1 == chunks.size()) ? 1 : 0;
                r.timestamp_ns = now_ns();
                network_send_update(from->fd, r);
            }
        }
        // The server has everything any client sent it, so a periodic request is
        // answered here; a joiner's full request must not fan out to everyone.
        return false;
    }
    return true;                                         // carets and anything else: relay
}
static void served_client_thread(std::shared_ptr<NetworkClient> client) {
    ServedDoc* d = client->doc;
    for (;;) {
        uint8_t kind = 0; std::vector<char> payload;
        if (!recv_frame(client->fd, kind, payload)) break;
        if (kind != UPDATE) continue;
        Update u{};
        if (!deserialize_update(payload.data(), payload.size(), u)) continue;
        std::lock_guard<std::mutex> lock(d->mu);
        if (!served_handle(d, client, u)) continue;
        for (const auto& other : d->clients) {
            if (other != client) network_send_update(other->fd, u);
        }
    }
    int fd = client->fd;
    {
        std::lock_guard<std::mutex> lock(d->mu);
        client->fd = -1;
        d->clients.erase(std::remove(d->clients.begin(), d->clients.end(), client), d->clients.end());
    }
    if (fd >= 0) close(fd);
    if (!g_stop) {
        served_broadcast_roster(d);
        std::cout << "  - " << client->user_id << " left "
                  << (d->id.empty() ? "(default)" : d->id) << "\n" << std::flush;
    }
    client->finished.store(true, std::memory_order_release);
}
static void serve_reap_threads(bool join_all) {
    std::vector<std::thread> to_join;
    {
        std::lock_guard<std::mutex> lock(g_serve_threads_mu);
        for (auto it = g_serve_threads.begin(); it != g_serve_threads.end(); ) {
            if (join_all || it->first->finished.load(std::memory_order_acquire)) {
                to_join.push_back(std::move(it->second));
                it = g_serve_threads.erase(it);
            } else ++it;
        }
    }
    for (auto& t : to_join) if (t.joinable()) t.join();
}
static void serve_accept_loop() {
    while (!g_stop) {
        sockaddr_in peer{}; socklen_t plen = sizeof(peer);
        int fd = accept(g_listen_fd, (sockaddr*)&peer, &plen);
        if (fd < 0) { if (errno == EINTR) continue; if (g_stop) break; usleep(50000); continue; }

        uint8_t kind = 0; std::vector<char> code;
        std::string uid; int cport = 0; bool rejoin = false; std::string want;
        set_recv_timeout(fd, g_connect_timeout_ms);
        if (!recv_frame(fd, kind, code) || kind != HELLO ||
            !parse_network_hello(code, uid, cport, rejoin, want)) {
            const char* msg = "invalid access code";
            send_frame(fd, ERROR, msg, std::strlen(msg)); close(fd); continue;
        }
        // With several documents on one port the joiner has to say which one it wants;
        // with exactly one there is no ambiguity, so an unnamed joiner gets it.
        ServedDoc* d = nullptr;
        if (want.empty() && g_served.size() == 1) d = g_served[0].get();
        else d = served_find(want);
        if (!d) {
            std::string msg = want.empty()
                ? ("this server holds several documents (" + served_list() + "); name one with --doc")
                : ("this server does not hold document '" + want + "' (it has: " + served_list() + ")");
            send_frame(fd, ERROR, msg.data(), msg.size()); close(fd); continue;
        }
        {
            std::lock_guard<std::mutex> lock(d->mu);
            std::string base = uid;
            int n = 2;
            bool taken = (uid == g_user_id);
            for (const auto& c : d->clients) if (c->user_id == uid) taken = true;
            while (taken && n < 100) {
                uid = base + "-" + std::to_string(n++);
                taken = (uid == g_user_id);
                for (const auto& c : d->clients) if (c->user_id == uid) taken = true;
            }
        }
        std::string docinfo = d->id + "\n" + uid;
        if (!send_frame(fd, DOCINFO, docinfo.data(), docinfo.size())) { close(fd); continue; }

        std::vector<char> snap;
        {
            std::lock_guard<std::mutex> lock(d->mu);
            std::ifstream in(d->path, std::ios::binary);
            snap.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
        if (snap.size() > NETWORK_MAX_FRAME || !send_frame(fd, SNAPSHOT, snap.data(), snap.size())) { close(fd); continue; }
        set_recv_timeout(fd, 0);

        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        auto client = std::make_shared<NetworkClient>();
        client->fd = fd; client->user_id = uid; client->doc = d; client->ip = ip; client->port = cport;
        { std::lock_guard<std::mutex> lock(d->mu); d->clients.push_back(client); }
        served_broadcast_roster(d);
        std::cout << "  + " << uid << " joined " << (d->id.empty() ? "(default)" : d->id)
                  << " from " << ip << "\n" << std::flush;
        {
            std::lock_guard<std::mutex> lock(g_serve_threads_mu);
            g_serve_threads.emplace_back(client, std::thread(served_client_thread, client));
        }
        serve_reap_threads(false);
    }
}

/// Entry point for --serve. Runs until interrupted; there is no editor UI.
int serve_main() {
    mkdir("user_docs", 0755);
    for (const auto& id : g_doc_ids) {
        auto d = std::make_unique<ServedDoc>();
        d->id = id;
        g_doc_id = id;                      // doc_dir()/resolve_doc_path() read the global
        if (!id.empty()) mkdir(doc_dir().c_str(), 0755);
        d->path = resolve_doc_path(g_user_id);
        struct stat st{};
        if (stat(d->path.c_str(), &st) != 0) write_initial_doc(d->path);
        // The server starts the document's history from its file (genesis ops).
        d->replica = std::make_unique<crdt::Replica>(crdt_new_client_id());
        std::string text = lines_to_text(read_file_lines(d->path));
        if (!text.empty()) d->replica->import_genesis(text, crdt_genesis_origin(text));
        g_served.push_back(std::move(d));
    }
    g_doc_id.clear();

    g_network_mode = true;
    g_network_host = true;                  // we own a listening socket
    if (!network_open_listener()) {
        std::cerr << (g_network_error.empty() ? "Could not open a listening socket" : g_network_error) << "\n";
        return 2;
    }
    std::cout << "Serving " << g_served.size() << " document(s) on port " << g_network_port;
    if (!g_port_explicit) std::cout << " (chosen automatically)";
    std::cout << "\n";
    for (const auto& d : g_served) {
        std::cout << "  - " << (d->id.empty() ? "(default)" : d->id) << "  ->  " << d->path << "\n";
    }
    std::cout << "  Others join with:  ./build/editor --doc <id> --join <this machine's IP> "
              << g_access_code;
    if (g_port_explicit) std::cout << " " << g_network_port;
    std::cout << "\n  Ctrl-C to stop.\n" << std::flush;

    install_signal_handlers();
    std::thread accept_thread(serve_accept_loop);
    while (!g_stop) usleep(200000);

    if (g_listen_fd >= 0) shutdown(g_listen_fd, SHUT_RDWR);
    for (auto& d : g_served) {
        std::lock_guard<std::mutex> lock(d->mu);
        for (const auto& c : d->clients) if (c->fd >= 0) shutdown(c->fd, SHUT_RDWR);
    }
    if (accept_thread.joinable()) accept_thread.join();
    serve_reap_threads(true);
    if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
    std::cout << "\nServer stopped.\n";
    return 0;
}

} // namespace synctext
