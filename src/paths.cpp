/**
 * @file paths.cpp
 * @brief Identities, document ids and where files live.
 */
#include "shared.h"

namespace synctext {

// State used only by this file.
static constexpr size_t DOC_ID_MAX = 64;     ///< Longest accepted document id.


/// Who this session is, worked out rather than typed.
///
/// The document is what the user names; their own identity is an implementation
/// detail, so it comes from the OS account. The same account opening one document
/// twice on a machine would collide, so register_user_try() appends a numeric suffix
/// until the id is free. SYNCTEXT_USER overrides it, which scripts and tests use.
std::string default_user_id() {
    const char* env = std::getenv("SYNCTEXT_USER");
    if (env && *env) return env;
    const char* name = std::getenv("USER");
    if (!name || !*name) name = std::getenv("LOGNAME");
    if (!name || !*name) {
        struct passwd* pw = getpwuid(getuid());
        if (pw && pw->pw_name && *pw->pw_name) name = pw->pw_name;
    }
    std::string out;
    for (char c : std::string(name ? name : "user")) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out.push_back(ok ? c : '_');
    }
    if (out.empty()) out = "user";
    if (out.size() > USER_ID_MAX - 5) out.resize(USER_ID_MAX - 5);  // room for a "-NN" suffix
    return out;
}

/// Validate a document id. Rejects rather than rewrites: silently mapping '/' to '_'
/// would let "a/b" and "a_b" land in the same document without anyone noticing, and
/// "." or ".." would point the scope at a directory that is already in use.
/// Returns true and fills `out` on success; on failure `why` explains the problem.
bool validate_doc_id(const std::string& in, std::string& out, std::string& why) {
    if (in.empty()) { why = "document id is empty"; return false; }
    // Otherwise "--doc --host" would quietly consume the mode flag as an id.
    if (in[0] == '-') { why = "document id cannot start with '-' (missing the id after --doc?)"; return false; }
    if (in.size() > DOC_ID_MAX) {
        why = "document id is longer than " + std::to_string(DOC_ID_MAX) + " characters";
        return false;
    }
    for (char c : in) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) {
            why = "document id may only contain letters, digits, '-', '_' and '.'";
            return false;
        }
    }
    // "." and ".." resolve to a directory that is already somebody's scope, which
    // would give two sessions the same files under different registries.
    if (in.find_first_not_of('.') == std::string::npos) {
        why = "document id cannot be '" + in + "' (it would point outside its own directory)";
        return false;
    }
    out = in;
    return true;
}

/// Short, collision-free stand-in for the id inside POSIX queue names.
///
/// A queue name is capped at QUEUE_NAME_MAX, and it has to hold "/synctext_",
/// the scope, a separator and the user id. A long id would otherwise truncate the
/// user id away and hand every participant in the document the same queue.
std::string doc_scope_tag() {
    const size_t prefix = sizeof("/synctext_") - 1;
    const size_t budget = QUEUE_NAME_MAX - 1 - prefix - 1 - (USER_ID_MAX - 1);
    if (g_doc_id.size() <= budget) return g_doc_id;
    uint32_t h = 2166136261u;                       // FNV-1a
    for (char c : g_doc_id) { h ^= (unsigned char)c; h *= 16777619u; }
    char hex[9];
    std::snprintf(hex, sizeof(hex), "%08x", h);
    size_t keep = (budget > 9) ? budget - 9 : 0;    // room for '_' + 8 hex digits
    return g_doc_id.substr(0, keep) + "_" + hex;
}
/// Directory holding this scope's documents.
std::string doc_dir() {
    return g_doc_id.empty() ? std::string("user_docs")
                            : std::string("user_docs/") + g_doc_id;
}
/// Where a given participant's copy of this document lives.
std::string doc_path_for(const std::string& user_id) {
    return doc_dir() + "/" + user_id + "_doc.txt";
}
/// Same, resolved to an absolute path. Sessions are routinely started from different
/// working directories, and peers must be able to find each other's documents.
std::string resolve_doc_path(const std::string& user_id) {
    std::string rel = doc_path_for(user_id);
    char cwd[PATH_MAX];
    if (!rel.empty() && rel[0] != '/' && getcwd(cwd, sizeof(cwd))) {
        std::string abs = std::string(cwd) + "/" + rel;
        if (abs.size() < DOC_PATH_MAX) return abs;   // otherwise keep the relative form
    }
    return rel;
}
/// Peers are discovered through the registry, so a per-document registry is what
/// actually separates one document's participants from another's.
std::string registry_shm_name() {
    return g_doc_id.empty() ? std::string(REGISTRY_SHM_NAME)
                            : std::string("/synctext_reg_") + g_doc_id;
}

} // namespace synctext
