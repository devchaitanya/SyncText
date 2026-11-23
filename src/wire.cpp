/**
 * @file wire.cpp
 * @brief Binary encoding of an Update.
 */
#include "shared.h"

namespace synctext {


// ============================================================================
// SECTION 3: MESSAGE SERIALIZATION - BINARY PROTOCOL FOR UPDATES
// ============================================================================

bool serialize_update(const Update& u, char* out, size_t cap, size_t& out_len) {
    const std::string& uid = u.user_id;
    const std::string& oldc = u.old_content;
    const std::string& newc = u.new_content;
    size_t need = sizeof(WireHdr) + uid.size() + oldc.size() + newc.size();
    if (need > cap) return false;
    // Use version 2 to allow extended operation types
    WireHdr h{2,(uint8_t)u.type,(uint32_t)u.line_no,(uint32_t)u.col_start,(uint32_t)u.col_end,u.timestamp_ns,
              (uint16_t)uid.size(),(uint32_t)oldc.size(),(uint32_t)newc.size()};
    std::memcpy(out,&h,sizeof(h));
    size_t off = sizeof(h);
    std::memcpy(out+off, uid.data(), uid.size()); off += uid.size();
    std::memcpy(out+off, oldc.data(), oldc.size()); off += oldc.size();
    std::memcpy(out+off, newc.data(), newc.size()); off += newc.size();
    out_len = off;
    return true;
}
bool deserialize_update(const char* in, size_t len, Update& u) {
    if (len < sizeof(WireHdr)) return false;
    WireHdr h{}; std::memcpy(&h, in, sizeof(h));
    if (h.ver != 1 && h.ver != 2) return false;
    size_t off = sizeof(h);
    if (off + h.user_len + h.old_len + h.new_len > len) return false;
    std::string uid(in+off, h.user_len); off += h.user_len;
    std::string oldc(in+off, h.old_len); off += h.old_len;
    std::string newc(in+off, h.new_len); off += h.new_len;
    Update out{};
    out.type = (Update::Type)h.type;
    out.line_no = h.line_no;
    out.col_start = h.col_start;
    out.col_end = h.col_end;
    out.timestamp_ns = h.ts_ns;
    out.user_id = std::move(uid);
    out.old_content = std::move(oldc);
    out.new_content = std::move(newc);
    u = std::move(out);
    return true;
}

} // namespace synctext
