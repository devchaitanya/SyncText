/**
 * @file net.cpp
 * @brief LAN mode: a full mesh of TCP links between participants.
 *
 * Every participant listens, and every pair of participants holds one direct TCP
 * link, so an edit goes straight from its author to each peer with no relay and no
 * host to lose. The CRDT is what makes this simple: ops may arrive in any order,
 * over any path, more than once, and replicas still converge (sync.cpp, rga.cpp).
 *
 *   joining   dial any participant (the one started with --host, or anyone else),
 *             get the document id, an identity and that participant's peer list,
 *             then link to everyone on the list.
 *   linking   of two participants, the one with the smaller user id dials, so a pair
 *             never ends up with two links. A link handshake has no snapshot.
 *   gossip    whenever its set of links changes, a participant sends the list to all
 *             of them (ROSTER). That is how everyone learns about a newcomer.
 *   leaving   a link that breaks is dropped. There is no election; everyone else
 *             keeps editing. A link to a live peer that broke is dialled again.
 *   repair    the version-vector exchange (sync.cpp) runs over every link, so a peer
 *             that is reachable only through someone else still converges: that
 *             someone has its ops and hands them on.
 *
 * Talking to a --serve server is the one star-shaped case: the server relays, so
 * a client links only to it while it is up, and links to the other clients
 * directly (from the server's roster) if it goes away.
 */
#include "shared.h"

namespace synctext {

// State used only by this file.
static std::thread g_accept_thread;          ///< Accepts links from peers (everyone runs one).
static std::thread g_mesh_thread;            ///< Dials peers we know about but have no link to.
static std::mutex g_network_send_mutex;
static std::mutex g_client_threads_mutex;
/// Per-link reader threads. Kept joinable (not detached) so shutdown can wait for
/// them: a detached thread can outlive main and touch these globals mid-destruction.
static std::vector<std::pair<std::shared_ptr<NetworkClient>, std::thread>> g_client_threads;

/// False while our only link is to a --serve server, which relays for us.
static std::atomic<bool> g_mesh{true};
/// The participant we first joined through: it answers our request for the history.
static std::string g_contact_user;

/// Peers we have heard about (from ROSTER gossip) and may need to dial.
struct KnownPeer { std::string ip; int port = 0; int fails = 0; uint64_t next_try_ns = 0; };
static std::mutex g_known_mutex;
static std::map<std::string, KnownPeer> g_known;
static constexpr int LINK_MAX_FAILS = 5;            ///< Give up on a peer after this many failed dials.
static constexpr int LINK_RETRY_BASE_MS = 300;      ///< Back-off between dials grows by this each time.
static constexpr int LINK_CONNECT_TIMEOUT_MS = 2000;

int g_connect_timeout_ms = 10000;  ///< Bound on a single connect attempt when joining.
bool g_port_explicit = false;      ///< True when the user named a port on the command line.
int g_port_base = 9000;            ///< First port tried when none was named.
int g_port_span = 16;              ///< How many consecutive ports to try / scan.

bool net_is_mesh() { return g_mesh.load(); }
std::string net_contact_peer() { return g_contact_user; }

/// connect() with a deadline that also honours shutdown requests.
///
/// A blocking connect to an address that silently drops packets (wrong IP, host
/// down, firewall DROP rather than REJECT) parks for the full TCP SYN timeout --
/// minutes -- and a SIGTERM in the middle changes nothing, because the syscall is
/// restarted. Connecting non-blocking and polling in short slices keeps a typo in
/// the host address from looking like a hang, and lets Ctrl-C work.
static bool connect_with_timeout(int fd, const sockaddr_in& addr, int timeout_ms) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    bool ok = false;
    if (connect(fd, (const sockaddr*)&addr, sizeof(addr)) == 0) {
        ok = true;
    } else if (errno == EINPROGRESS) {
        const int slice_ms = 200;
        int waited = 0;
        while (waited < timeout_ms && !g_stop) {
            struct pollfd pf{fd, POLLOUT, 0};
            int pr = poll(&pf, 1, slice_ms);
            if (pr > 0) {
                int err = 0; socklen_t len = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) { errno = EIO; break; }
                if (err == 0) { ok = true; }
                else errno = err;
                break;
            }
            if (pr < 0 && errno != EINTR) break;
            waited += slice_ms;
        }
        if (!ok && waited >= timeout_ms) errno = ETIMEDOUT;
    }
    (void)fcntl(fd, F_SETFL, flags);   // the rest of the protocol expects blocking I/O
    return ok;
}

/// Bound how long a handshake read may block. A peer that accepts the connection
/// and then says nothing -- a half-open host, a blackholing middlebox, or our own
/// listener after a self-connect -- must not park the joiner forever.
void set_recv_timeout(int fd, int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/// True when a socket ended up connected to itself. A joiner listens on the session
/// port before it dials, so pointing --join at this machine's own address connects
/// to our own listener, which never answers a handshake.
static bool socket_connected_to_self(int fd) {
    sockaddr_in local{}, peer{};
    socklen_t ll = sizeof(local), pl = sizeof(peer);
    if (getsockname(fd, (sockaddr*)&local, &ll) != 0) return false;
    if (getpeername(fd, (sockaddr*)&peer, &pl) != 0) return false;
    // Same address on both ends means the destination resolved to this machine: the
    // kernel only picks a source address equal to the destination for a local target.
    if (local.sin_addr.s_addr != peer.sin_addr.s_addr) return false;
    // Either a simultaneous self-connect (identical ports), or -- the case that
    // actually bites -- we dialled the listening socket we opened for failover.
    return local.sin_port == peer.sin_port || ntohs(peer.sin_port) == g_listen_port;
}

/// Why the LAN session could not be established. A wrong access code, an unreachable
/// host and a busy port are very different problems; the operator should be told which.
std::string g_network_error;
static bool send_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len) { ssize_t n = send(fd, p, len, MSG_NOSIGNAL); if (n <= 0) return false; p += n; len -= (size_t)n; }
    return true;
}
static bool recv_all(int fd, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    while (len) { ssize_t n = recv(fd, p, len, 0); if (n <= 0) return false; p += n; len -= (size_t)n; }
    return true;
}
bool send_frame(int fd, uint8_t kind, const void* payload, size_t len) {
    if (fd < 0 || len > NETWORK_MAX_FRAME) return false;
    uint8_t h[5] = {kind, (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    std::lock_guard<std::mutex> lock(g_network_send_mutex);
    return send_all(fd, h, sizeof(h)) && (len == 0 || send_all(fd, payload, len));
}
bool recv_frame(int fd, uint8_t& kind, std::vector<char>& payload) {
    uint8_t h[5]; if (!recv_all(fd, h, sizeof(h))) return false;
    uint32_t len = ((uint32_t)h[1] << 24) | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 8) | h[4];
    if (len > NETWORK_MAX_FRAME) return false;
    kind = h[0]; payload.resize(len); return len == 0 || recv_all(fd, payload.data(), len);
}
bool network_send_update(int fd, const Update& u) {
    if (fd < 0) return false;
    // Per-thread scratch buffer: NETWORK_MAX_FRAME is too large to place on the stack.
    static thread_local std::vector<char> data(NETWORK_MAX_FRAME);
    size_t len = 0;
    return serialize_update(u, data.data(), data.size(), len) && send_frame(fd, UPDATE, data.data(), len);
}

// ----------------------------------------------------------------------------
// Handshake
//
//   HELLO    "code \n user \n listen_port \n link \n doc"   (link = 1 for a mesh link)
//   DOCINFO  "doc \n assigned_user \n my_user \n mesh"      (a --serve server sends
//                                                            only the first two lines)
//   SNAPSHOT the document file                              (join only, not on a link)
// ----------------------------------------------------------------------------
static std::string make_network_hello(bool link) {
    // The trailing document id lets the peer refuse a joiner that asked for a
    // different document, instead of silently handing over the wrong one.
    return g_access_code + "\n" + g_user_id + "\n" + std::to_string(g_listen_port) + "\n" +
           (link ? "1" : "0") + "\n" + g_doc_id;
}
bool parse_network_hello(const std::vector<char>& payload, std::string& out_user_id, int& out_port,
                                bool& out_link, std::string& out_doc_id) {
    std::string hello(payload.begin(), payload.end());
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= hello.size()) {
        size_t eol = hello.find('\n', pos);
        if (eol == std::string::npos) { parts.push_back(hello.substr(pos)); break; }
        parts.push_back(hello.substr(pos, eol - pos));
        pos = eol + 1;
    }
    if (parts.empty() || parts[0] != g_access_code) return false;
    out_user_id = (parts.size() > 1 && !parts[1].empty()) ? parts[1] : "(unknown)";
    out_port = (parts.size() > 2) ? std::atoi(parts[2].c_str()) : 0;
    out_link = (parts.size() > 3) && parts[3] == "1";
    out_doc_id = (parts.size() > 4) ? parts[4] : std::string();
    return true;
}
static std::vector<std::string> split_lines(const std::vector<char>& payload) {
    std::vector<std::string> parts;
    std::string blob(payload.begin(), payload.end());
    size_t pos = 0;
    while (pos <= blob.size()) {
        size_t eol = blob.find('\n', pos);
        if (eol == std::string::npos) { parts.push_back(blob.substr(pos)); break; }
        parts.push_back(blob.substr(pos, eol - pos));
        pos = eol + 1;
    }
    return parts;
}
static std::vector<char> read_doc_bytes() {
    std::ifstream file(g_doc_path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}
static bool write_doc_bytes(const std::vector<char>& data) {
    std::ofstream file(g_doc_path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(data.data(), (std::streamsize)data.size());
    return file.good();
}

/// Joining: consume the DOCINFO frame and settle our document scope and identity.
///
/// A joiner that passed --doc has already been validated by the peer, so the ids
/// match. A joiner that passed nothing adopts whatever the session is editing, which
/// is what makes "join user1's doc1" work without having to know its id: the copy
/// lands in that document's directory rather than the default one.
static bool adopt_host_document(int fd, std::string& why, std::string& out_peer_user) {
    uint8_t kind = 0; std::vector<char> payload;
    if (!recv_frame(fd, kind, payload)) { why = "host sent no document information"; return false; }
    if (kind == ERROR) { why = std::string(payload.begin(), payload.end()); return false; }
    if (kind != DOCINFO) { why = "unexpected reply from host during handshake"; return false; }
    std::vector<std::string> parts = split_lines(payload);
    std::string host_doc = parts.size() > 0 ? parts[0] : std::string();
    std::string assigned = parts.size() > 1 ? parts[1] : std::string();
    out_peer_user = parts.size() > 2 ? parts[2] : std::string();
    g_mesh = parts.size() > 3 && parts[3] == "mesh";      // a --serve server relays instead
    if (!assigned.empty() && assigned != g_user_id) {
        g_user_id = assigned;          // renamed by the peer to keep identities unique
        g_base_user_id = assigned;
    }
    if (host_doc == g_doc_id) { g_doc_path = resolve_doc_path(g_user_id); return true; }
    if (!g_doc_id.empty()) {          // the peer should have rejected us; belt and braces
        why = "host serves document '" + (host_doc.empty() ? std::string("(default)") : host_doc) +
              "', not '" + g_doc_id + "'";
        return false;
    }
    std::string checked, bad;
    if (!host_doc.empty() && !validate_doc_id(host_doc, checked, bad)) {
        why = "host advertised an unusable document id: " + bad;
        return false;
    }
    g_doc_id = checked;
    if (!g_doc_id.empty()) { mkdir("user_docs", 0755); mkdir(doc_dir().c_str(), 0755); }
    g_doc_path = resolve_doc_path(g_user_id);   // re-file our copy under the session's scope
    return true;
}

// ----------------------------------------------------------------------------
// Roster: g_roster is what the UI shows -- us, then everyone we hold a link to.
// The same list, sent as a ROSTER frame, is the gossip that spreads membership.
// ----------------------------------------------------------------------------
static bool is_loopback(const std::string& ip) { return ip.rfind("127.", 0) == 0; }

static void roster_rebuild() {
    std::vector<RosterEntry> list;
    RosterEntry self; self.user_id = g_user_id; self.port = g_listen_port;
    list.push_back(self);
    {
        std::lock_guard<std::mutex> lock(g_network_clients_mutex);
        for (const auto& c : g_network_clients) {
            RosterEntry e; e.user_id = c->user_id; e.ip = c->ip; e.port = c->port;
            list.push_back(std::move(e));
        }
    }
    std::lock_guard<std::mutex> lock(g_roster_mutex);
    g_roster = std::move(list);
}
static std::string roster_serialize() {
    std::string out;
    std::lock_guard<std::mutex> lock(g_roster_mutex);
    for (const auto& e : g_roster) {
        out += e.user_id; out += '\t'; out += e.ip; out += '\t'; out += std::to_string(e.port); out += '\n';
    }
    return out;
}
static std::vector<RosterEntry> roster_parse(const std::vector<char>& payload) {
    std::vector<RosterEntry> parsed;
    for (const std::string& line : split_lines(payload)) {
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        RosterEntry e;
        e.user_id = line.substr(0, t1);
        e.ip = line.substr(t1 + 1, t2 - t1 - 1);
        e.port = std::atoi(line.c_str() + t2 + 1);
        if (!e.user_id.empty()) parsed.push_back(std::move(e));
    }
    return parsed;
}
/// Tell every linked peer who we are linked to.
static void roster_broadcast() {
    std::string blob = roster_serialize();
    std::lock_guard<std::mutex> lock(g_network_clients_mutex);
    for (const auto& client : g_network_clients) send_frame(client->fd, ROSTER, blob.data(), blob.size());
}
static bool linked_to(const std::string& user_id) {
    std::lock_guard<std::mutex> lock(g_network_clients_mutex);
    for (const auto& c : g_network_clients) if (c->user_id == user_id) return true;
    return false;
}
/// A peer told us who it is linked to: remember everyone, so the mesh thread can dial
/// those we have no link to yet.
static void roster_learn(const std::shared_ptr<NetworkClient>& from, const std::vector<char>& payload) {
    std::vector<RosterEntry> list = roster_parse(payload);
    if (!g_mesh) {
        // Linked only to a --serve server: show its roster, and keep the other
        // clients' addresses in case the server goes away.
        std::lock_guard<std::mutex> lock(g_roster_mutex);
        g_roster = list;
    }
    std::lock_guard<std::mutex> lock(g_known_mutex);
    for (RosterEntry& e : list) {
        if (e.user_id == g_user_id) continue;
        if (e.ip.empty()) continue;                  // the sender itself: already linked
        // A peer on the sender's machine may be listed by its loopback address, which
        // means nothing from here; it is reachable at the sender's address instead.
        if (is_loopback(e.ip) && !is_loopback(from->ip) && !from->ip.empty()) e.ip = from->ip;
        KnownPeer& k = g_known[e.user_id];
        // Someone holds a live link to it, so it is worth dialling again.
        if (k.ip != e.ip || k.port != e.port || k.fails >= LINK_MAX_FAILS) { k.fails = 0; k.next_try_ns = 0; }
        k.ip = e.ip;
        k.port = e.port;
    }
}

// ----------------------------------------------------------------------------
// Links
// ----------------------------------------------------------------------------
/// Join the threads of departed links. With join_all, wait for every one of them.
/// Threads are moved out under the lock and joined outside it, so a link thread
/// finishing its own cleanup can still take the roster locks.
static void reap_client_threads(bool join_all) {
    std::vector<std::thread> to_join;
    {
        std::lock_guard<std::mutex> lock(g_client_threads_mutex);
        for (auto it = g_client_threads.begin(); it != g_client_threads.end(); ) {
            if (join_all || it->first->finished.load(std::memory_order_acquire)) {
                to_join.push_back(std::move(it->second));
                it = g_client_threads.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& t : to_join) if (t.joinable()) t.join();
}
/// Wake every link's blocking recv so its thread can wind down.
static void shutdown_client_sockets() {
    std::lock_guard<std::mutex> lock(g_network_clients_mutex);
    for (const auto& client : g_network_clients) {
        if (client->fd >= 0) shutdown(client->fd, SHUT_RDWR);
    }
}

/// One thread per link: hand what arrives to the main thread, then retire the link.
/// Nothing is relayed: every peer has its own link to the author.
static void network_link_thread(std::shared_ptr<NetworkClient> client) {
    for (;;) {
        uint8_t kind = 0; std::vector<char> payload;
        if (!recv_frame(client->fd, kind, payload)) break;
        if (kind == ROSTER) { roster_learn(client, payload); g_force_refresh = true; wake_main_loop(); continue; }
        if (kind != UPDATE) break;
        Update u{};
        if (!deserialize_update(payload.data(), payload.size(), u)) continue;
        enqueue_received_update(Update(u));
        wake_main_loop();
    }
    // Retire the fd under the lock so no sender can pick it up after close().
    int fd = client->fd;
    {
        std::lock_guard<std::mutex> lock(g_network_clients_mutex);
        client->fd = -1;
        g_network_clients.erase(std::remove(g_network_clients.begin(), g_network_clients.end(), client),
                                g_network_clients.end());
    }
    if (fd >= 0) close(fd);
    if (!g_stop) {
        if (g_debug_level >= 1) std::cerr << "[net] link to " << client->user_id << " closed\n";
        // Our relay is gone: from now on, link to the other clients directly.
        if (!g_mesh && client->user_id == g_contact_user) g_mesh = true;
        roster_rebuild();
        roster_broadcast();
        g_force_refresh = true;
        wake_main_loop();
    }
    client->finished.store(true, std::memory_order_release);
}

/// Register a handshaken link and start its thread. Returns false (and leaves the fd
/// to the caller) if we already hold a link to that user.
static bool add_link(int fd, const std::string& user_id, const std::string& ip, int port) {
    auto client = std::make_shared<NetworkClient>();
    client->fd = fd; client->user_id = user_id; client->ip = ip; client->port = port;
    {
        std::lock_guard<std::mutex> lock(g_network_clients_mutex);
        for (const auto& c : g_network_clients) if (c->user_id == user_id) return false;
        g_network_clients.push_back(client);
    }
    {
        std::lock_guard<std::mutex> lock(g_known_mutex);
        KnownPeer& k = g_known[user_id];
        k.ip = ip; k.port = port; k.fails = 0; k.next_try_ns = 0;
    }
    {
        std::lock_guard<std::mutex> lock(g_client_threads_mutex);
        g_client_threads.emplace_back(client, std::thread(network_link_thread, client));
    }
    if (g_debug_level >= 1) std::cerr << "[net] linked with " << user_id << " (" << ip << ":" << port << ")\n";
    roster_rebuild();
    roster_broadcast();      // tells the new peer who else to link to, and the others about it
    g_sync_now = true;       // swap version vectors over the new link right away
    g_force_refresh = true;
    wake_main_loop();
    reap_client_threads(false);
    return true;
}

/// Accept links from peers: joiners (handed the document) and mesh links (not).
static void network_accept_loop() {
    while (!g_stop) {
        sockaddr_in peer{}; socklen_t peer_len = sizeof(peer);
        int fd = accept(g_listen_fd, (sockaddr*)&peer, &peer_len);
        if (fd < 0) { if (errno == EINTR) continue; if (g_stop) break; usleep(50000); continue; }
        uint8_t kind = 0; std::vector<char> code;
        std::string client_user_id; int client_port = 0; bool link = false; std::string want_doc;
        // Never let one silent client block every other peer from being accepted.
        set_recv_timeout(fd, g_connect_timeout_ms);
        if (!recv_frame(fd, kind, code) || kind != HELLO ||
            !parse_network_hello(code, client_user_id, client_port, link, want_doc)) {
            const char* msg = "invalid access code"; send_frame(fd, ERROR, msg, std::strlen(msg)); close(fd); continue;
        }
        // A joiner that named a document must be told plainly if this session edits a
        // different one, rather than being handed the wrong document's contents.
        if (!want_doc.empty() && want_doc != g_doc_id) {
            std::string msg = "this host is serving document '" +
                              (g_doc_id.empty() ? std::string("(default)") : g_doc_id) +
                              "', not '" + want_doc + "'";
            send_frame(fd, ERROR, msg.data(), msg.size()); close(fd); continue;
        }
        if (link) {
            // A mesh link from a participant that already joined: its identity is settled.
            if (client_user_id == g_user_id || linked_to(client_user_id)) {
                std::string msg = "already linked with " + client_user_id;
                send_frame(fd, ERROR, msg.data(), msg.size()); close(fd); continue;
            }
        } else {
            // Identities come from OS accounts and collide easily (the same person on two
            // terminals, or two machines with the same username); echo suppression compares
            // user ids, so a clash would make participants ignore each other's edits.
            std::string base = client_user_id;
            int n = 2;
            // Only live links count: a participant that crashed and comes back under its
            // old name is still in our gossip memory, and should get that name back.
            auto taken = [&]() { return client_user_id == g_user_id || linked_to(client_user_id); };
            while (taken() && n < 100) client_user_id = base + "-" + std::to_string(n++);
        }
        std::string docinfo = g_doc_id + "\n" + client_user_id + "\n" + g_user_id + "\nmesh";
        if (!send_frame(fd, DOCINFO, docinfo.data(), docinfo.size())) { close(fd); continue; }
        if (!link) {
            // The joiner shows this until its replica has the shared history.
            std::vector<char> snapshot = read_doc_bytes();
            if (snapshot.size() > NETWORK_MAX_FRAME || !send_frame(fd, SNAPSHOT, snapshot.data(), snapshot.size())) {
                close(fd); continue;
            }
        }
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        set_recv_timeout(fd, 0);   // handshake done; live traffic blocks indefinitely
        if (!add_link(fd, client_user_id, ip, client_port)) close(fd);
    }
}

/// Dial a known peer and complete a link handshake. Returns the socket, or -1.
static int dial_link(const std::string& user_id, const KnownPeer& k) {
    if (k.ip.empty() || k.port <= 0) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)k.port);
    if (inet_pton(AF_INET, k.ip.c_str(), &addr.sin_addr) != 1) { close(fd); return -1; }
    if (!connect_with_timeout(fd, addr, std::min(g_connect_timeout_ms, LINK_CONNECT_TIMEOUT_MS)) ||
        socket_connected_to_self(fd)) {
        close(fd); return -1;
    }
    set_recv_timeout(fd, LINK_CONNECT_TIMEOUT_MS);
    std::string hello = make_network_hello(true);
    uint8_t kind = 0; std::vector<char> payload;
    if (!send_frame(fd, HELLO, hello.data(), hello.size()) || !recv_frame(fd, kind, payload) || kind != DOCINFO) {
        close(fd); return -1;
    }
    std::vector<std::string> parts = split_lines(payload);
    // Same document, and the peer is who the roster said it is.
    if (parts.empty() || parts[0] != g_doc_id || parts.size() < 3 || parts[2] != user_id) { close(fd); return -1; }
    set_recv_timeout(fd, 0);
    return fd;
}

/// Keep the mesh complete: dial every known peer we have no link to. Of each pair,
/// only the smaller user id dials, so two peers never open two links to each other.
static void network_mesh_loop() {
    while (!g_stop) {
        for (int i = 0; i < 2 && !g_stop; ++i) usleep(50 * 1000);
        if (!g_mesh) continue;
        std::vector<std::pair<std::string, KnownPeer>> due;
        uint64_t now = now_ns();
        {
            std::lock_guard<std::mutex> lock(g_known_mutex);
            for (const auto& kv : g_known) {
                if (!(g_user_id < kv.first)) continue;
                if (kv.second.fails >= LINK_MAX_FAILS || now < kv.second.next_try_ns) continue;
                due.push_back(kv);
            }
        }
        for (const auto& kv : due) {
            if (g_stop) break;
            if (linked_to(kv.first)) continue;
            int fd = dial_link(kv.first, kv.second);
            if (fd >= 0 && add_link(fd, kv.first, kv.second.ip, kv.second.port)) continue;
            if (fd >= 0) { close(fd); continue; }             // linked meanwhile
            std::lock_guard<std::mutex> lock(g_known_mutex);
            auto it = g_known.find(kv.first);
            if (it == g_known.end()) continue;
            it->second.fails++;
            it->second.next_try_ns = now_ns() + (uint64_t)(LINK_RETRY_BASE_MS * it->second.fails) * 1000000ull;
            if (g_debug_level >= 1 && it->second.fails == LINK_MAX_FAILS)
                std::cerr << "[net] giving up on " << kv.first << " for now\n";
        }
    }
}

/// Bind our listening socket.
///
/// With no port on the command line a host takes the first free port in
/// [g_port_base, g_port_base + g_port_span) and prints it, so nobody has to pick a
/// number that might already be taken. A joiner only needs *some* port that its
/// peers can link to, so it takes an ephemeral one -- which also means it can never
/// collide with, or accidentally dial, a port a host wants.
bool network_open_listener() {
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) return false;
    int yes = 1; setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY);

    bool bound_ok = false;
    if (!g_network_host) {
        addr.sin_port = g_port_explicit ? htons((uint16_t)g_network_port) : 0;
        bound_ok = bind(g_listen_fd, (sockaddr*)&addr, sizeof(addr)) == 0;
        if (!bound_ok && g_port_explicit) {         // named port busy: any port will do for a joiner
            addr.sin_port = 0;
            bound_ok = bind(g_listen_fd, (sockaddr*)&addr, sizeof(addr)) == 0;
        }
    } else if (g_port_explicit) {
        addr.sin_port = htons((uint16_t)g_network_port);
        bound_ok = bind(g_listen_fd, (sockaddr*)&addr, sizeof(addr)) == 0;
        if (!bound_ok) {
            g_network_error = "Could not listen on port " + std::to_string(g_network_port) + " (" +
                              std::strerror(errno) + "). Is another SyncText host already running?";
            return false;
        }
    } else {
        for (int p = g_port_base; p < g_port_base + g_port_span && !bound_ok; ++p) {
            addr.sin_port = htons((uint16_t)p);
            if (bind(g_listen_fd, (sockaddr*)&addr, sizeof(addr)) == 0) { g_network_port = p; bound_ok = true; }
        }
        if (!bound_ok) {
            g_network_error = "No free port in " + std::to_string(g_port_base) + "-" +
                              std::to_string(g_port_base + g_port_span - 1) +
                              ". Name one explicitly, or set SYNCTEXT_PORT_BASE.";
            return false;
        }
    }
    if (!bound_ok) return false;
    if (listen(g_listen_fd, 16) < 0) return false;
    sockaddr_in bound{}; socklen_t blen = sizeof(bound);
    g_listen_port = (getsockname(g_listen_fd, (sockaddr*)&bound, &blen) == 0)
                        ? ntohs(bound.sin_port) : g_network_port;
    return true;
}
/// Try to complete a join handshake with one host:port. On success returns the
/// connected socket (>= 0) and the peer's user id; on failure returns -1 and leaves
/// the reason in g_network_error. `fatal` is set when retrying other ports is pointless.
static int try_join_port(const std::string& host, int port, bool& fatal, std::string& out_peer_user) {
    fatal = false;
    g_network_port = port;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { g_network_error = "Could not create a socket"; fatal = true; return -1; }
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        g_network_error = "'" + host + "' is not a valid IPv4 address (use the host's numeric address, not its name)";
        fatal = true; close(fd); return -1;
    }
    if (!connect_with_timeout(fd, addr, g_connect_timeout_ms)) {
        g_network_error = "Could not reach " + host + ":" + std::to_string(port) + " (" + std::strerror(errno) +
                          "). Check the host is running with --host, that you are on the same network, "
                          "and that the port is open on the host's firewall.";
        close(fd); return -1;
    }
    if (socket_connected_to_self(fd)) {
        g_network_error = "Connected to this machine's own listener on " + host + ":" +
                          std::to_string(port) +
                          ". Point --join at the HOST's address, not this machine's.";
        close(fd); return -1;
    }
    set_recv_timeout(fd, g_connect_timeout_ms);
    std::string hello = make_network_hello(false);
    if (!send_frame(fd, HELLO, hello.data(), hello.size())) {
        g_network_error = "Connected to " + host + " but the handshake could not be sent";
        close(fd); return -1;
    }
    std::string why;
    if (!adopt_host_document(fd, why, out_peer_user)) {
        g_network_error = "Host " + host + ":" + std::to_string(port) + ": " + why;
        close(fd); return -1;
    }
    uint8_t kind = 0; std::vector<char> snapshot;
    if (!recv_frame(fd, kind, snapshot)) {
        g_network_error = "Host " + host + ":" + std::to_string(port) +
                          " accepted the connection but sent no reply within " +
                          std::to_string(g_connect_timeout_ms) + "ms";
        close(fd); return -1;
    }
    if (kind == ERROR) {
        std::string reason(snapshot.begin(), snapshot.end());
        g_network_error = "Host rejected the connection: " + (reason.empty() ? std::string("no reason given") : reason);
        close(fd); return -1;
    }
    if (kind != SNAPSHOT) { g_network_error = "Unexpected reply from host during handshake"; close(fd); return -1; }
    if (!write_doc_bytes(snapshot)) {
        g_network_error = "Could not write the received document to " + g_doc_path;
        fatal = true; close(fd); return -1;
    }
    set_recv_timeout(fd, 0);   // live traffic blocks indefinitely again
    return fd;
}
bool start_network_session(const std::string& host) {
    // Everyone listens: peers link to us here.
    if (!network_open_listener()) return false;
    if (!g_network_host) {
        int fd = -1;
        std::string peer_user;
        if (g_port_explicit) {
            bool fatal = false;
            fd = try_join_port(host, g_network_port, fatal, peer_user);
        } else {
            // No port named: walk the range. Ports that answer with the wrong document or a
            // bad code are skipped, so the search lands on the session actually editing what
            // was asked for rather than merely the first SyncText process it meets.
            std::string first_error;
            for (int p = g_port_base; p < g_port_base + g_port_span; ++p) {
                bool fatal = false;
                fd = try_join_port(host, p, fatal, peer_user);
                if (fd >= 0) break;
                if (fatal) break;
                if (first_error.empty()) first_error = g_network_error;
            }
            if (fd < 0) {
                g_network_error = "No session on " + host + " ports " + std::to_string(g_port_base) + "-" +
                                  std::to_string(g_port_base + g_port_span - 1) +
                                  " accepted this request. Last reason: " + g_network_error +
                                  (first_error.empty() ? "" : " | First: " + first_error);
            }
        }
        if (fd < 0) return false;
        // A --serve server does not say who it is; name its link after its address.
        g_contact_user = peer_user.empty() ? host : peer_user;
        add_link(fd, g_contact_user, host, g_network_port);
    } else {
        roster_rebuild();
    }
    g_accept_thread = std::thread(network_accept_loop);
    g_mesh_thread = std::thread(network_mesh_loop);
    return true;
}
void stop_network_session() {
    if (!g_network_mode) return;
    g_stop = 1;
    // Unblock accept(); the dialler notices g_stop within one connect slice.
    if (g_listen_fd >= 0) shutdown(g_listen_fd, SHUT_RDWR);
    if (g_accept_thread.joinable()) g_accept_thread.join();
    if (g_mesh_thread.joinable()) g_mesh_thread.join();   // no new links past this point
    shutdown_client_sockets();
    reap_client_threads(true);
    if (g_listen_fd >= 0) { close(g_listen_fd); g_listen_fd = -1; }
}

} // namespace synctext
