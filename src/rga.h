// rga.h - a sequence CRDT (RGA) for collaborative text, as designed in the interview notes.
//
// Layers (top to bottom):
//
//   Replica   one user's copy. Turns editor actions at (row, col) into ops, applies
//             remote ops, buffers ops that arrive too early, drops duplicates, and
//             can send a peer whatever it is missing (version vectors).
//   View      lines (vector<string>) + ids (node pointer per character, same shape)
//             + nls (the newline node ending each row). This is what an editor draws.
//   Sequence  the truth: a doubly linked list of character nodes + a hash map
//             ID -> node, with a B-tree of "boxes" on top. Each box stores counts of
//             visible characters / newlines / tail so a node's (row, col) is O(log n).
//
// Rules (the whole algorithm):
//   * ID = (Lamport counter, user). Unique because users are unique.
//   * Insert = "put char after node <anchor ID>". Walk forward from the anchor and
//     skip neighbours whose ID is bigger than the new one; insert there.
//   * Delete = tombstone (flag). Nodes are never unlinked, so anchors stay valid.
//
// Not implemented (deliberately, to keep it readable): tombstone garbage collection,
// run-length nodes (one node per character here). Wire encoding lives in sync.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace crdt {

// ---------------------------------------------------------------------------
// IDs
// ---------------------------------------------------------------------------
struct Id {
    uint64_t ctr = 0;     // Lamport counter; 0 is reserved for HEAD
    std::string user;

    bool operator==(const Id& o) const { return ctr == o.ctr && user == o.user; }
    bool operator!=(const Id& o) const { return !(*this == o); }
    // Compare the counter first, then the user name. Every replica uses this order.
    bool operator<(const Id& o) const { return ctr != o.ctr ? ctr < o.ctr : user < o.user; }
    bool operator>(const Id& o) const { return o < *this; }
};

struct IdHash {
    size_t operator()(const Id& i) const {
        return std::hash<uint64_t>()(i.ctr) * 1000003u ^ std::hash<std::string>()(i.user);
    }
};

const Id kHeadId{0, ""};

// ---------------------------------------------------------------------------
// Sequence: linked list + hash map + counted B-tree
// ---------------------------------------------------------------------------
struct Box;

struct Node {
    Id id;
    char ch = 0;
    bool deleted = false;     // tombstone flag
    Node* prev = nullptr;
    Node* next = nullptr;
    Box* leaf = nullptr;      // which leaf this node belongs to (nullptr for HEAD)
};

// A box is either a leaf (a consecutive piece of the linked list) or an internal
// node of the tree. Counts cover everything below it and only include visible nodes.
struct Box {
    Box* parent = nullptr;
    bool is_leaf = true;
    std::vector<Box*> kids;   // internal boxes only, in document order

    Node* first = nullptr;    // leaf only: first / last list node in this leaf
    Node* last = nullptr;
    size_t n_nodes = 0;       // leaf only: nodes in this leaf, tombstones included

    size_t ch = 0;            // visible symbols ('\n' counts as a symbol)
    size_t nl = 0;            // visible newlines
    size_t tail = 0;          // visible symbols after the last newline (= ch if nl == 0)
};

// Where a node sits among the visible symbols *before* it.
struct Pos {
    size_t pos = 0;  // visible symbols before the node
    size_t row = 0;  // visible newlines before the node
    size_t col = 0;  // visible symbols since the last newline before the node
};

class Sequence {
public:
    explicit Sequence(size_t leaf_max = 64, size_t fanout = 16);
    Sequence(const Sequence&) = delete;
    Sequence& operator=(const Sequence&) = delete;

    Node* head() const { return head_; }
    Node* find(const Id& id) const;                        // nullptr if unknown

    // RGA integrate. The anchor must exist; inserting a known ID returns the old node.
    Node* insert(const Id& after, const Id& id, char ch);

    void mark_deleted(Node* n);                            // flag + refresh one path
    void mark_deleted_batch(const std::vector<Node*>& ns); // flag all, refresh each box once

    Pos position(const Node* n) const;                     // O(leaf size + log n)

    std::string text() const;                              // visible text, by walking the list
    size_t visible_size() const { return root_->ch; }
    size_t node_count() const { return nodes_.size(); }    // including tombstones
    size_t height() const;

    // Test helper: recompute everything from scratch and compare. Empty string = OK.
    std::string check_invariants(bool check_positions = true) const;

private:
    Box* new_box(bool leaf);
    Box* leftmost_leaf() const;
    void recompute(Box* b) const;
    void refresh_up(Box* b) const;
    void split_leaf(Box* leaf);
    void split_box(Box* box);
    void insert_sibling(Box* left, Box* right);

    size_t leaf_max_;
    size_t fanout_;
    std::vector<std::unique_ptr<Node>> nodes_;
    std::vector<std::unique_ptr<Box>> boxes_;
    std::unordered_map<Id, Node*, IdHash> index_;
    Node* head_ = nullptr;
    Box* root_ = nullptr;
};

// ---------------------------------------------------------------------------
// Ops and Replica
// ---------------------------------------------------------------------------
struct Op {
    enum class Kind : uint8_t { Insert, Delete };
    Kind kind = Kind::Insert;
    std::string origin;   // who made the op
    uint64_t seq = 0;     // origin's op number: 1, 2, 3, ... (for dedupe / resend)
    Id id;                // Insert: the new node's ID.  Delete: the node to delete.
    Id after;             // Insert only: the anchor
    char ch = 0;          // Insert only
};

using VersionVector = std::map<std::string, uint64_t>;   // user -> highest seq applied

class Replica {
public:
    explicit Replica(std::string user, size_t leaf_max = 64, size_t fanout = 16);

    // --- local editing, in editor coordinates ---
    // Insert ch before (row, col). '\n' splits the row.
    Op insert_char(size_t row, size_t col, char ch);
    // Delete `count` visible symbols starting at (row, col), in reading order
    // (newlines included, so this can join rows). Returns one op per symbol.
    std::vector<Op> erase(size_t row, size_t col, size_t count);

    // --- local editing, by node ---
    // The editor integration edits relative to the nodes it last showed the user, not
    // to (row, col): remote ops may have moved things since, but nodes never move.
    Op insert_after(const Node* anchor, char ch);
    // Deletes the given nodes; ones already deleted are skipped (no op produced).
    std::vector<Op> erase_nodes(const std::vector<const Node*>& nodes);
    // The visible nodes in reading order, newline nodes included. Pointers stay valid
    // for the replica's lifetime (nodes are never freed).
    std::vector<const Node*> visible_nodes() const;

    // Load initial text as ops from a deterministic origin: every replica that imports
    // the same text gets identical IDs, so two copies started from the same file merge
    // without duplicating it. Only valid on an empty replica.
    void import_genesis(const std::string& text, const std::string& origin);

    // --- network ---
    // Safe with duplicates, any order, and ops that arrive before what they depend on.
    void receive(const Op& op);
    // Same, for a batch. Large batches update the view once at the end.
    void receive_all(const std::vector<Op>& ops);
    // Every op this replica has that a peer with version vector `theirs` lacks.
    std::vector<Op> missing_for(const VersionVector& theirs) const;   // in causal order
    const VersionVector& version_vector() const { return vv_; }
    size_t pending() const { return pending_.size(); }

    // --- view ---
    const std::vector<std::string>& lines() const { return lines_; }
    std::string text() const;            // lines joined with '\n'
    const std::string& user() const { return user_; }
    const Sequence& sequence() const { return seq_; }

    std::string check_invariants(bool check_positions = true) const;

private:
    Node* prev_node(size_t row, size_t col) const;
    bool ready(const Op& op) const;
    void apply_remote(const Op& op);
    void record(const Op& op);
    void drain_pending();
    void view_insert(size_t row, size_t col, Node* n);
    void view_erase_one(size_t row, size_t col, Node* n);
    void rebuild_view();          // lines/ids/nls from the list, O(n)

    bool defer_view_ = false;     // while true, ops update only the list; see receive_all

    std::string user_;
    uint64_t lamport_ = 0;   // highest counter seen anywhere
    uint64_t my_seq_ = 0;    // my own op numbers

    Sequence seq_;
    std::vector<std::string> lines_{""};
    std::vector<std::vector<Node*>> ids_{{}};
    std::vector<Node*> nls_;  // nls_[r] = the '\n' node that ends row r

    VersionVector vv_;
    std::vector<Op> history_;                     // every applied op, in apply order
    std::vector<Op> pending_;                     // arrived too early
};

}  // namespace crdt
