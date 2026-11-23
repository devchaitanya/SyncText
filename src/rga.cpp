// rga.cpp - implementation of the sequence CRDT described in rga.h.
#include "rga.h"

#include <algorithm>
#include <cassert>
#include <set>
#include <sstream>
#include <stdexcept>

namespace crdt {

// ===========================================================================
// Sequence
// ===========================================================================

Sequence::Sequence(size_t leaf_max, size_t fanout)
    : leaf_max_(std::max<size_t>(leaf_max, 2)), fanout_(std::max<size_t>(fanout, 3)) {
    nodes_.push_back(std::make_unique<Node>());
    head_ = nodes_.back().get();
    head_->id = kHeadId;
    index_[kHeadId] = head_;
    root_ = new_box(/*leaf=*/true);   // one empty leaf to start with
}

Box* Sequence::new_box(bool leaf) {
    boxes_.push_back(std::make_unique<Box>());
    boxes_.back()->is_leaf = leaf;
    return boxes_.back().get();
}

Node* Sequence::find(const Id& id) const {
    auto it = index_.find(id);
    return it == index_.end() ? nullptr : it->second;
}

Box* Sequence::leftmost_leaf() const {
    Box* b = root_;
    while (!b->is_leaf) b = b->kids.front();
    return b;
}

size_t Sequence::height() const {
    size_t h = 1;
    for (Box* b = root_; !b->is_leaf; b = b->kids.front()) ++h;
    return h;
}

// Recompute one box's counts from what is directly below it.
void Sequence::recompute(Box* b) const {
    b->ch = b->nl = b->tail = 0;
    if (b->is_leaf) {
        if (b->n_nodes == 0) return;
        for (Node* x = b->first;; x = x->next) {
            if (!x->deleted) {
                ++b->ch;
                if (x->ch == '\n') { ++b->nl; b->tail = 0; }
                else ++b->tail;
            }
            if (x == b->last) break;
        }
        return;
    }
    for (Box* k : b->kids) { b->ch += k->ch; b->nl += k->nl; }
    // tail: walk kids from the right; whole kids count until one contains a newline
    for (auto it = b->kids.rbegin(); it != b->kids.rend(); ++it) {
        if ((*it)->nl == 0) b->tail += (*it)->ch;
        else { b->tail += (*it)->tail; break; }
    }
}

void Sequence::refresh_up(Box* b) const {
    for (; b; b = b->parent) recompute(b);
}

Node* Sequence::insert(const Id& after, const Id& id, char ch) {
    if (Node* existing = find(id)) return existing;
    Node* p = find(after);
    if (!p) throw std::logic_error("insert: anchor not present");

    // RGA rule: skip neighbours with a bigger ID (concurrent siblings that sort first,
    // together with everything typed after them, which has even bigger counters).
    while (p->next && p->next->id > id) p = p->next;

    nodes_.push_back(std::make_unique<Node>());
    Node* n = nodes_.back().get();
    n->id = id;
    n->ch = ch;

    // link into the list after p
    n->prev = p;
    n->next = p->next;
    if (p->next) p->next->prev = n;
    p->next = n;
    index_[id] = n;

    // the new node joins its predecessor's leaf (HEAD -> the leftmost leaf)
    Box* leaf = (p == head_) ? leftmost_leaf() : p->leaf;
    if (leaf->n_nodes == 0) leaf->first = leaf->last = n;
    else if (p == head_) leaf->first = n;
    else if (leaf->last == p) leaf->last = n;
    n->leaf = leaf;
    ++leaf->n_nodes;

    if (leaf->n_nodes > leaf_max_) split_leaf(leaf);
    else refresh_up(leaf);
    return n;
}

// Cut an oversized leaf in two; the right half becomes a new leaf.
void Sequence::split_leaf(Box* left) {
    size_t keep = left->n_nodes / 2;
    Node* m = left->first;
    for (size_t i = 0; i < keep; ++i) m = m->next;   // m = first node of the right half

    Box* right = new_box(/*leaf=*/true);
    right->first = m;
    right->last = left->last;
    right->n_nodes = left->n_nodes - keep;
    left->last = m->prev;
    left->n_nodes = keep;
    for (Node* x = m;; x = x->next) {
        x->leaf = right;
        if (x == right->last) break;
    }
    recompute(left);
    recompute(right);
    insert_sibling(left, right);   // may split parents, may grow a new root
    refresh_up(left);
    refresh_up(right);
}

// Cut an internal box with too many kids in two.
void Sequence::split_box(Box* left) {
    size_t keep = left->kids.size() / 2;
    Box* right = new_box(/*leaf=*/false);
    right->kids.assign(left->kids.begin() + keep, left->kids.end());
    left->kids.resize(keep);
    for (Box* k : right->kids) k->parent = right;
    recompute(left);
    recompute(right);
    insert_sibling(left, right);
}

// Place `right` immediately after `left` under the same parent.
void Sequence::insert_sibling(Box* left, Box* right) {
    if (!left->parent) {                     // left was the root: grow the tree upwards
        Box* r = new_box(/*leaf=*/false);
        r->kids = {left, right};
        left->parent = right->parent = r;
        root_ = r;
        recompute(r);
        return;
    }
    Box* p = left->parent;
    auto it = std::find(p->kids.begin(), p->kids.end(), left);
    p->kids.insert(it + 1, right);
    right->parent = p;
    if (p->kids.size() > fanout_) split_box(p);
}

void Sequence::mark_deleted(Node* n) {
    if (n == head_ || n->deleted) return;
    n->deleted = true;
    refresh_up(n->leaf);
}

// Flag everything first, then recompute each touched box exactly once, level by level.
// All leaves sit at the same depth, so each `level` set holds boxes of one depth.
void Sequence::mark_deleted_batch(const std::vector<Node*>& ns) {
    std::set<Box*> level;
    for (Node* n : ns) {
        if (n == head_ || n->deleted) continue;
        n->deleted = true;
        level.insert(n->leaf);
    }
    while (!level.empty()) {
        std::set<Box*> up;
        for (Box* b : level) {
            recompute(b);
            if (b->parent) up.insert(b->parent);
        }
        level.swap(up);
    }
}

Pos Sequence::position(const Node* n) const {
    Pos p;
    if (n == head_) return p;

    // 1. inside the node's own leaf
    const Box* leaf = n->leaf;
    bool col_done = false;
    for (const Node* x = leaf->first; x != n; x = x->next) {
        if (x->deleted) continue;
        ++p.pos;
        if (x->ch == '\n') { ++p.row; p.col = 0; col_done = true; }
        else ++p.col;
    }

    // 2. walk up; at each level add the counts of the kids left of where we came from
    const Box* child = leaf;
    for (const Box* box = leaf->parent; box; child = box, box = box->parent) {
        auto it = std::find(box->kids.begin(), box->kids.end(), child);
        while (it != box->kids.begin()) {          // nearest left sibling first
            --it;
            const Box* sib = *it;
            p.pos += sib->ch;
            p.row += sib->nl;
            if (!col_done) {
                if (sib->nl == 0) p.col += sib->ch;
                else { p.col += sib->tail; col_done = true; }
            }
        }
    }
    return p;
}

std::string Sequence::text() const {
    std::string out;
    out.reserve(root_->ch);
    for (const Node* x = head_->next; x; x = x->next)
        if (!x->deleted) out.push_back(x->ch);
    return out;
}

std::string Sequence::check_invariants(bool check_positions) const {
    std::ostringstream why;

    // 1. tree shape: parent links, equal leaf depth, fan-out, counts
    std::vector<const Box*> leaves;
    size_t leaf_depth = 0;
    struct Frame { const Box* b; size_t depth; };
    std::vector<Frame> stack{{root_, 1}};
    if (root_->parent) why << "root has a parent; ";
    while (!stack.empty()) {
        Frame f = stack.back();
        stack.pop_back();
        Box copy = *f.b;
        recompute(&copy);
        if (copy.ch != f.b->ch || copy.nl != f.b->nl || copy.tail != f.b->tail)
            why << "stale counts at depth " << f.depth << "; ";
        if (f.b->is_leaf) {
            if (leaf_depth == 0) leaf_depth = f.depth;
            else if (leaf_depth != f.depth) why << "leaves at different depths; ";
            if (f.b->n_nodes > leaf_max_) why << "leaf over capacity; ";
            leaves.push_back(f.b);
        } else {
            if (f.b->kids.empty() || f.b->kids.size() > fanout_) why << "bad fan-out; ";
            for (auto it = f.b->kids.rbegin(); it != f.b->kids.rend(); ++it) {
                if ((*it)->parent != f.b) why << "bad parent pointer; ";
                stack.push_back({*it, f.depth + 1});
            }
        }
    }

    // 2. the leaves, in order, cover the linked list exactly
    const Node* x = head_->next;
    for (const Box* leaf : leaves) {
        if (leaf->n_nodes == 0) {
            if (leaves.size() != 1) why << "empty leaf in a non-empty tree; ";
            continue;
        }
        if (x != leaf->first) { why << "leaf first pointer mismatch; "; break; }
        for (size_t i = 0; i < leaf->n_nodes; ++i) {
            if (!x) { why << "list shorter than leaves; "; break; }
            if (x->leaf != leaf) why << "node->leaf mismatch; ";
            if (i + 1 == leaf->n_nodes && x != leaf->last) why << "leaf last pointer mismatch; ";
            x = x->next;
        }
    }
    if (x) why << "list longer than leaves; ";

    // 3. list links and the hash map
    size_t count = 0;
    for (const Node* y = head_; y; y = y->next) {
        ++count;
        if (y->next && y->next->prev != y) why << "broken prev link; ";
        if (find(y->id) != y) why << "index mismatch; ";
    }
    if (count != nodes_.size() || index_.size() != nodes_.size()) why << "node count mismatch; ";

    // 4. position() agrees with a brute-force walk for every node
    if (check_positions) {
        Pos brute;
        for (const Node* y = head_->next; y; y = y->next) {
            Pos got = position(y);
            if (got.pos != brute.pos || got.row != brute.row || got.col != brute.col) {
                why << "position mismatch for (" << y->id.ctr << "," << y->id.user << "); ";
                break;
            }
            if (!y->deleted) {
                ++brute.pos;
                if (y->ch == '\n') { ++brute.row; brute.col = 0; }
                else ++brute.col;
            }
        }
    }
    return why.str();
}

// ===========================================================================
// Replica
// ===========================================================================

Replica::Replica(std::string user, size_t leaf_max, size_t fanout)
    : user_(std::move(user)), seq_(leaf_max, fanout) {
    if (user_.empty()) throw std::invalid_argument("user must not be empty (reserved for HEAD)");
}

// The node just before the cursor at (row, col).
Node* Replica::prev_node(size_t row, size_t col) const {
    if (col > 0) return ids_[row][col - 1];
    if (row > 0) return nls_[row - 1];
    return seq_.head();
}

Op Replica::insert_char(size_t row, size_t col, char ch) {
    if (row >= lines_.size() || col > lines_[row].size())
        throw std::out_of_range("insert_char: cursor outside the document");
    Node* prev = prev_node(row, col);

    Op op;
    op.kind = Op::Kind::Insert;
    op.origin = user_;
    op.seq = ++my_seq_;
    op.id = Id{++lamport_, user_};   // bigger than every ID seen so far
    op.after = prev->id;
    op.ch = ch;

    Node* n = seq_.insert(op.after, op.id, ch);   // lands right after prev
    view_insert(row, col, n);
    record(op);
    return op;
}

std::vector<Op> Replica::erase(size_t row, size_t col, size_t count) {
    if (row >= lines_.size() || col > lines_[row].size())
        throw std::out_of_range("erase: cursor outside the document");

    // collect the nodes straight from the view: characters, then a row's newline, ...
    std::vector<Node*> victims;
    size_t r = row, c = col;
    while (victims.size() < count) {
        if (c < lines_[r].size()) { victims.push_back(ids_[r][c]); ++c; }
        else if (r < nls_.size()) { victims.push_back(nls_[r]); ++r; c = 0; }
        else break;   // end of document
    }

    // view: keep the left part of the first row and the right part of the last row
    lines_[row] = lines_[row].substr(0, col) + lines_[r].substr(c);
    std::vector<Node*> joined(ids_[row].begin(), ids_[row].begin() + col);
    joined.insert(joined.end(), ids_[r].begin() + c, ids_[r].end());
    ids_[row] = std::move(joined);
    lines_.erase(lines_.begin() + row + 1, lines_.begin() + r + 1);
    ids_.erase(ids_.begin() + row + 1, ids_.begin() + r + 1);
    nls_.erase(nls_.begin() + row, nls_.begin() + r);

    seq_.mark_deleted_batch(victims);

    std::vector<Op> ops;
    for (Node* v : victims) {
        Op op;
        op.kind = Op::Kind::Delete;
        op.origin = user_;
        op.seq = ++my_seq_;
        op.id = v->id;
        record(op);
        ops.push_back(op);
    }
    return ops;
}

Op Replica::insert_after(const Node* anchor, char ch) {
    Op op;
    op.kind = Op::Kind::Insert;
    op.origin = user_;
    op.seq = ++my_seq_;
    op.id = Id{++lamport_, user_};   // biggest ID so far, so it lands right after the anchor
    op.after = anchor->id;
    op.ch = ch;
    apply_remote(op);                // same placement + view update as a remote insert
    record(op);
    return op;
}

std::vector<Op> Replica::erase_nodes(const std::vector<const Node*>& nodes) {
    std::vector<Node*> live;
    for (const Node* c : nodes) {
        Node* n = seq_.find(c->id);
        if (n && n != seq_.head() && !n->deleted) live.push_back(n);
    }
    if (live.size() > 64) {                       // big delete: flag all, rebuild the view once
        seq_.mark_deleted_batch(live);
        rebuild_view();
    } else {
        for (Node* n : live) {
            Pos p = seq_.position(n);
            seq_.mark_deleted(n);
            view_erase_one(p.row, p.col, n);
        }
    }
    std::vector<Op> ops;
    for (Node* n : live) {
        Op op;
        op.kind = Op::Kind::Delete;
        op.origin = user_;
        op.seq = ++my_seq_;
        op.id = n->id;
        record(op);
        ops.push_back(op);
    }
    return ops;
}

std::vector<const Node*> Replica::visible_nodes() const {
    std::vector<const Node*> out;
    out.reserve(seq_.visible_size());
    for (size_t r = 0; r < lines_.size(); ++r) {
        out.insert(out.end(), ids_[r].begin(), ids_[r].end());
        if (r < nls_.size()) out.push_back(nls_[r]);
    }
    return out;
}

void Replica::import_genesis(const std::string& text, const std::string& origin) {
    std::vector<Op> ops;
    ops.reserve(text.size());
    Id prev = kHeadId;
    for (size_t i = 0; i < text.size(); ++i) {
        Op op;
        op.kind = Op::Kind::Insert;
        op.origin = origin;
        op.seq = i + 1;
        op.id = Id{i + 1, origin};
        op.after = prev;
        op.ch = text[i];
        prev = op.id;
        ops.push_back(std::move(op));
    }
    receive_all(ops);
}

void Replica::receive_all(const std::vector<Op>& ops) {
    if (ops.size() <= 64) {
        for (const Op& op : ops) receive(op);
        return;
    }
    defer_view_ = true;                           // patching the view per op would be O(n) each
    for (const Op& op : ops) receive(op);
    defer_view_ = false;
    rebuild_view();
}

void Replica::rebuild_view() {
    lines_.assign(1, std::string());
    ids_.assign(1, std::vector<Node*>());
    nls_.clear();
    for (Node* x = seq_.head()->next; x; x = x->next) {
        if (x->deleted) continue;
        if (x->ch == '\n') {
            nls_.push_back(x);
            lines_.emplace_back();
            ids_.emplace_back();
        } else {
            lines_.back().push_back(x->ch);
            ids_.back().push_back(x);
        }
    }
}

void Replica::receive(const Op& op) {
    auto it = vv_.find(op.origin);
    uint64_t have = it == vv_.end() ? 0 : it->second;
    if (op.seq <= have) return;   // duplicate
    if (ready(op)) {              // the common case: in order, dependencies present
        apply_remote(op);
        record(op);
        if (!pending_.empty()) drain_pending();
        return;
    }
    for (const Op& p : pending_)
        if (p.origin == op.origin && p.seq == op.seq) return;   // already waiting
    pending_.push_back(op);
}

// An op can be applied when it is the origin's next op and what it points at exists.
bool Replica::ready(const Op& op) const {
    auto it = vv_.find(op.origin);
    uint64_t have = it == vv_.end() ? 0 : it->second;
    if (op.seq != have + 1) return false;
    if (op.kind == Op::Kind::Insert) return seq_.find(op.after) != nullptr;
    return seq_.find(op.id) != nullptr;
}

void Replica::drain_pending() {
    bool progress = true;
    while (progress) {
        progress = false;
        std::vector<Op> waiting;
        waiting.swap(pending_);
        for (Op& op : waiting) {
            auto it = vv_.find(op.origin);
            if (it != vv_.end() && op.seq <= it->second) continue;   // became a duplicate
            if (ready(op)) {
                apply_remote(op);
                record(op);
                progress = true;
            } else {
                pending_.push_back(std::move(op));
            }
        }
    }
}

void Replica::apply_remote(const Op& op) {
    if (op.kind == Op::Kind::Insert) {
        lamport_ = std::max(lamport_, op.id.ctr);
        Node* n = seq_.insert(op.after, op.id, op.ch);
        if (defer_view_) return;
        Pos p = seq_.position(n);                // where it landed on screen
        view_insert(p.row, p.col, n);
        return;
    }
    Node* n = seq_.find(op.id);
    if (n->deleted) return;                      // someone else deleted it already
    if (defer_view_) { seq_.mark_deleted(n); return; }
    Pos p = seq_.position(n);                    // before flagging: still visible
    seq_.mark_deleted(n);
    view_erase_one(p.row, p.col, n);
}

void Replica::record(const Op& op) {
    vv_[op.origin] = op.seq;
    history_.push_back(op);       // apply order is causal order: replaying it never stalls
}

std::vector<Op> Replica::missing_for(const VersionVector& theirs) const {
    std::vector<Op> out;
    for (const Op& op : history_) {
        auto it = theirs.find(op.origin);
        uint64_t have = it == theirs.end() ? 0 : it->second;
        if (op.seq > have) out.push_back(op);
    }
    return out;
}

void Replica::view_insert(size_t row, size_t col, Node* n) {
    if (n->ch == '\n') {                         // split the row
        lines_.insert(lines_.begin() + row + 1, lines_[row].substr(col));
        lines_[row].resize(col);
        ids_.insert(ids_.begin() + row + 1,
                    std::vector<Node*>(ids_[row].begin() + col, ids_[row].end()));
        ids_[row].resize(col);
        nls_.insert(nls_.begin() + row, n);
    } else {
        lines_[row].insert(lines_[row].begin() + col, n->ch);
        ids_[row].insert(ids_[row].begin() + col, n);
    }
}

void Replica::view_erase_one(size_t row, size_t col, Node* n) {
    if (n->ch == '\n') {                         // join row with the next one
        assert(nls_[row] == n);
        lines_[row] += lines_[row + 1];
        ids_[row].insert(ids_[row].end(), ids_[row + 1].begin(), ids_[row + 1].end());
        lines_.erase(lines_.begin() + row + 1);
        ids_.erase(ids_.begin() + row + 1);
        nls_.erase(nls_.begin() + row);
    } else {
        assert(ids_[row][col] == n);
        lines_[row].erase(col, 1);
        ids_[row].erase(ids_[row].begin() + col);
    }
}

std::string Replica::text() const {
    std::string out;
    for (size_t r = 0; r < lines_.size(); ++r) {
        if (r) out.push_back('\n');
        out += lines_[r];
    }
    return out;
}

std::string Replica::check_invariants(bool check_positions) const {
    std::string why = seq_.check_invariants(check_positions);
    if (text() != seq_.text()) why += "view text differs from the list; ";
    if (nls_.size() + 1 != lines_.size() || ids_.size() != lines_.size()) why += "view shape; ";

    // the view's nodes, read in order, must be exactly the list's visible nodes
    std::vector<const Node*> from_view;
    for (size_t r = 0; r < lines_.size(); ++r) {
        if (ids_[r].size() != lines_[r].size()) { why += "ids/lines length; "; break; }
        for (size_t c = 0; c < ids_[r].size(); ++c) {
            if (ids_[r][c]->ch != lines_[r][c] || ids_[r][c]->deleted) why += "bad ids entry; ";
            from_view.push_back(ids_[r][c]);
        }
        if (r < nls_.size()) from_view.push_back(nls_[r]);
    }
    std::vector<const Node*> from_list;
    for (const Node* x = seq_.head()->next; x; x = x->next)
        if (!x->deleted) from_list.push_back(x);
    if (from_view != from_list) why += "view node order differs from the list; ";
    return why;
}

}  // namespace crdt
