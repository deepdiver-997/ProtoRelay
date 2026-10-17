#include "web_server/codec/h2_codec.h"

namespace web_server {
namespace codec {

// 单请求头块上限（HEADERS+CONTINUATION 累积字节）：静态请求远小于此；
// 无上限时一条连接可以用不发 END_HEADERS 的 CONTINUATION 流把内存打爆
inline constexpr size_t kMaxHeaderBlockBytes = 64 * 1024;

bool H2Codec::stream_start_valid(int32_t id) {
    if ((id & 1) == 0) return false;                     // 偶数流 ID 不可能是客户端开的流
    if (id <= last_client_stream_) return false;         // 非单调递增重复开流
    last_client_stream_ = id;
    return true;
}

// 剥掉 HEADERS 里不在 HPACK 中的前缀：PADDED(1B 长度) 与 PRIORITY(5B) + 尾部填充。
// 返回 false = 前缀越界/非法 → PROTOCOL_ERROR。
// 布局顺序按 RFC 7541 §6.2：pad-length 字节在最前，其后才是 5 字节优先级字段
// ——旧实现先跳 5B 再读 padlen，PADDED+PRIORITY 同设时整体错位（2026-10-18 修复）。
static bool strip_headers_prefix(uint8_t flags, const std::string& payload, std::string& out) {
    size_t off = 0;
    uint8_t padlen = 0;
    if (flags & h2::flag::PADDED) {
        if (payload.empty()) return false;
        padlen = (uint8_t)payload[off++];
    }
    if (flags & h2::flag::PRIORITY) {
        if (payload.size() < off + 5) return false;
        off += 5;
    }
    if (payload.size() < off + padlen) return false;
    out = payload.substr(off, payload.size() - off - padlen);
    return true;
}

H2Codec::Result H2Codec::feed(int32_t stream_id, h2::FrameType type, uint8_t flags,
                              const std::string& payload) {
    Result r;

    if (type == h2::FrameType::HEADERS) {
        if (!stream_start_valid(stream_id)) { r.protocol_ok = false; r.error = h2::ErrorCode::PROTOCOL_ERROR; return r; }
        Head& h = heads_[stream_id];
        h.id = stream_id;
        std::string hp;
        if (!strip_headers_prefix(flags, payload, hp)) { r.protocol_ok = false; r.error = h2::ErrorCode::PROTOCOL_ERROR; return r; }
        if (h.block.size() + hp.size() > kMaxHeaderBlockBytes) {
            r.protocol_ok = false; r.error = h2::ErrorCode::ENHANCE_YOUR_CALM; return r;
        }
        h.block += hp;
        if (flags & h2::flag::END_HEADERS) h.headers_done = true;
        if (flags & h2::flag::END_STREAM) h.end_stream = true;
    } else if (type == h2::FrameType::CONTINUATION) {
        auto it = heads_.find(stream_id);
        if (it == heads_.end()) { r.protocol_ok = false; r.error = h2::ErrorCode::PROTOCOL_ERROR; return r; }   // 无 HEADERS 打头的续帧
        if (it->second.block.size() + payload.size() > kMaxHeaderBlockBytes) {
            r.protocol_ok = false; r.error = h2::ErrorCode::ENHANCE_YOUR_CALM; return r;
        }
        it->second.block += payload;                       // 纯 HPACK 续块（无前缀）
        if (flags & h2::flag::END_HEADERS) it->second.headers_done = true;
    } else if (type == h2::FrameType::DATA) {
        // 静态 GET/HEAD 无请求体；只需留意 END_STREAM 使请求闭合
        auto it = heads_.find(stream_id);
        if (it != heads_.end() && (flags & h2::flag::END_STREAM)) it->second.end_stream = true;
    } else {
        return r;                                          // 流控/管理帧：session 自处理，忽略
    }

    // 请求完整（END_HEADERS + END_STREAM）且未产出过 → 解码 + 归一化
    auto it = heads_.find(stream_id);
    if (it != heads_.end() && it->second.headers_done && it->second.end_stream && !it->second.served) {
        if (!decoder_.decode(it->second.block, it->second.headers)) {
            r.protocol_ok = false; r.error = h2::ErrorCode::COMPRESSION_ERROR;   // HPACK 解码失败
            return r;
        }
        HttpRequest q;
        for (auto& hh : it->second.headers) {
            if      (hh.first == ":method") q.method = hh.second;
            else if (hh.first == ":path")   q.path   = hh.second;
        }
        if (q.method.empty()) q.method = "GET";
        if (q.path.empty())   q.path = "/";
        r.request = std::move(q);
        // 请求已产出 → 装配态即回收。此前条目留到 RST_STREAM/GOAWAY 才清：
        // 每个请求留一份 header 块 + 解码结果，浏览器一条 H2 连接几百请求 =
        // 无界增长（长连接 webmail 下的真泄漏，2026-10-18 修复）。
        heads_.erase(it);
    }
    return r;
}

void H2Codec::abort_stream(int32_t stream_id) {
    heads_.erase(stream_id);
}

} // namespace codec
} // namespace web_server