#pragma once
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/common/mime_parameters.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace mail_system {

// 解析邮件 body 文件的 MIME 结构，填充 mime_root 树
// body 内容形如 "Header: value\r\n\r\nbody..."
// raw 收 string_view：调用方可以直接传 mmap 出来的映射区，
// 不必先把整封邮件读进 std::string。所有传 std::string 的旧调用点隐式转换，无需改动。
// MimePart 里存的是偏移量和拷贝出来的小字符串，不持有 raw 的指针，
// 因此解析完成后映射可以安全释放。
// depth：multipart 嵌套层级。恶意构造的深嵌套邮件（每层一个 boundary）
// 会把递归打到栈溢出 —— fuzz 冒烟的第一个必中目标。真实邮件嵌套不超过
// ~10 层，64 层封顶后该 part 按不透明正文处理，长度信息仍由调用方修正。
inline void parse_mime_tree(std::string_view raw, MimePart& root, size_t pos = 0,
                            size_t depth = 0) {
    root = MimePart{};
    root.metadata_version = kMimeMetadataVersion;
    // 找 header/body 分隔
    size_t sep = raw.find("\r\n\r\n", pos);
    std::string hdrs{(sep != std::string::npos) ? raw.substr(pos, sep - pos) : raw.substr(pos)};
    size_t body_start = (sep != std::string::npos) ? sep + 4 : raw.size();

    auto content_type = mime_header_value(hdrs, "content-type");
    // 保留既有畸形通知邮件兼容：头在正文后时只认行首的最后一个 Content-Type，
    // 不把 DKIM h= 中的名字当成 header，也不修改只读 mmap。
    if (content_type.empty()) {
        const auto whole_lower = mime_params_detail::lower(std::string(raw));
        const auto last = whole_lower.rfind("\ncontent-type:");
        if (last != std::string::npos)
            content_type = mime_header_value(std::string(raw.substr(last + 1)), "content-type");
    }
    const auto disposition = mime_header_value(hdrs, "content-disposition");
    const auto media = mime_params_detail::lower(mime_params_detail::trim(
        content_type.substr(0, content_type.find(';'))));
    const auto slash = media.find('/');
    if (slash != std::string::npos) {
        root.type = media.substr(0, slash);
        root.subtype = media.substr(slash + 1);
    } else {
        root.type = "text";
        root.subtype = "plain";
    }
    root.charset = mime_params_detail::lower(mime_parameter(content_type, "charset"));
    root.boundary = mime_parameter(content_type, "boundary");
    root.name = mime_parameter(disposition, "filename");
    if (root.name.empty()) root.name = mime_parameter(content_type, "name");
    root.disposition = mime_params_detail::lower(mime_params_detail::trim(
        disposition.substr(0, disposition.find(';'))));
    root.content_id = mime_header_value(hdrs, "content-id");
    root.encoding = mime_params_detail::lower(mime_header_value(hdrs, "content-transfer-encoding"));
    if (root.encoding.empty()) root.encoding = "7bit";

    // 记录 offset 和 length（length 覆盖整个 part：header + body）
    // 注意：非 multipart 的根 part 也必须用整封邮件长度，否则 IMAP BODY[1] 只能取到 header
    root.offset = pos;
    root.length = raw.size() - pos;
    // body 部分大小（content after \r\n\r\n）
    if (sep != std::string::npos) {
        root.body_size = raw.size() - body_start;
        root.lines = 0;
        for (size_t i = body_start; i < raw.size(); ++i)
            if (raw[i] == '\n') root.lines++;
    }

    // multipart: 递归解析子 part（深度封顶防栈溢出）
    if (root.is_multipart() && !root.boundary.empty() && depth < 64) {
        std::string bdr = "--" + root.boundary;
        size_t search_from = body_start;

        // 跳到第一个 boundary 后的内容
        size_t first = raw.find(bdr + "\r\n", search_from);
        if (first == std::string::npos) first = raw.find(bdr + "\n", search_from);
        if (first != std::string::npos) {
            size_t nl = raw.find("\r\n", first);
            if (nl == std::string::npos) nl = raw.find('\n', first);
            if (nl != std::string::npos)
                search_from = nl + (raw[nl] == '\r' ? 2 : 1);
        }

        while (search_from < raw.size()) {
            size_t next = raw.find(bdr, search_from);
            if (next == std::string::npos) break;
            if (next > 0 && raw[next-1] != '\n') { search_from = next + bdr.size(); continue; }

            bool is_close = (next + bdr.size() + 2 <= raw.size() &&
                             raw.substr(next + bdr.size(), 2) == "--");

            MimePart sub;
            // 子 part 只能看到自己的区间；否则同名 boundary 的畸形兄弟 part
            // 会被每层重复当作后代，深度虽封顶仍产生指数级解析工作。
            // 保留从文件起点的前缀，树中 offset 仍是完整 MIME 的绝对偏移。
            parse_mime_tree(raw.substr(0, next), sub, search_from, depth + 1);
            // 修正 sub 的 length 到 bdr 边界
            sub.length = next - search_from;
            while (sub.length > 0 && (raw[search_from + sub.length - 1] == '\r' ||
                                       raw[search_from + sub.length - 1] == '\n'))
                sub.length--;
            // 写回真实 body_size。
            // sub_sep 必须落在本 part 范围内（< next）：part 在 boundary 前没有
            // 空行时，find 会一路找到后续内容的分隔符，此时 next-(sub_sep+4)
            // 无符号下溢成天文数字，下面按它索引 raw 就是野读（fuzz+ASan 实锤，
            // 生产上落在 mmap 映射末尾即 SIGBUS）。
            size_t sub_sep = raw.find("\r\n\r\n", search_from);
            if (sub_sep != std::string::npos && sub_sep < next) {
                sub.body_size = next - (sub_sep + 4);
                while (sub.body_size > 0 && (raw[sub_sep + 4 + sub.body_size - 1] == '\r' ||
                                              raw[sub_sep + 4 + sub.body_size - 1] == '\n'))
                    sub.body_size--;
            }
            root.subs.push_back(std::move(sub));

            if (is_close) break;
            size_t nxt_nl = raw.find("\r\n", next);
            if (nxt_nl == std::string::npos) nxt_nl = raw.find('\n', next);
            if (nxt_nl == std::string::npos) break;
            search_from = nxt_nl + (raw[nxt_nl] == '\r' ? 2 : 1);
        }
    }
}

// sidecar JSON 里的字符串字段转义。boundary/charset 基本是 token，
// 但附件 filename（"n"）可以带引号和反斜杠，不转义会写出读不回来的 JSON。
inline std::string mime_json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // 控制字符直接丢弃，避免写出非法 JSON
                    break;
                }
                out += c;
        }
    }
    return out;
}

// 把 MIME 树写成 sidecar JSON —— load_mime_tree 的逆操作。
//
// 先写临时文件再 rename：rename 在 POSIX 上是原子的，因此并发的读者
// （多个 IMAP 会话可能同时 FETCH 同一封邮件）绝不会看到写了一半的 sidecar。
// 临时文件名带 pid，因为 smtpsServer 和 imapsServer 是两个独立进程。
inline bool save_mime_tree(const std::string& body_path, const MimePart& root) {
    if (body_path.empty() || root.type.empty()) {
        return false;
    }

    static std::atomic<std::uint64_t> seq{0};
    const std::string final_path = body_path + ".mime";
    const std::string tmp_path = final_path + ".tmp." +
                                 std::to_string(static_cast<long>(::getpid())) + "." +
                                 std::to_string(seq.fetch_add(1));

    try {
        {
            std::ofstream sf(tmp_path, std::ios::binary | std::ios::trunc);
            if (!sf.is_open()) {
                return false;
            }

            std::function<void(const MimePart&, std::ostream&)> write_part;
            write_part = [&](const MimePart& p, std::ostream& os) {
                os << "{\"v\":" << p.metadata_version << ",\"t\":\"" << mime_json_escape(p.type)
                   << "\",\"s\":\"" << mime_json_escape(p.subtype) << "\"";
                if (!p.charset.empty())  os << ",\"c\":\"" << mime_json_escape(p.charset) << "\"";
                if (!p.encoding.empty()) os << ",\"e\":\"" << mime_json_escape(p.encoding) << "\"";
                if (!p.boundary.empty()) os << ",\"b\":\"" << mime_json_escape(p.boundary) << "\"";
                if (!p.name.empty())     os << ",\"n\":\"" << mime_json_escape(p.name) << "\"";
                if (!p.disposition.empty()) os << ",\"d\":\"" << mime_json_escape(p.disposition) << "\"";
                if (!p.content_id.empty()) os << ",\"i\":\"" << mime_json_escape(p.content_id) << "\"";
                os << ",\"o\":" << p.offset << ",\"l\":" << p.length
                   << ",\"z\":" << p.body_size << ",\"ln\":" << p.lines;
                if (!p.subs.empty()) {
                    os << ",\"p\":[";
                    for (size_t i = 0; i < p.subs.size(); ++i) {
                        if (i) os << ",";
                        write_part(p.subs[i], os);
                    }
                    os << "]";
                }
                os << "}";
            };
            write_part(root, sf);

            sf.flush();
            if (!sf.good()) {
                sf.close();
                std::remove(tmp_path.c_str());
                return false;
            }
        }

        if (std::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
            std::remove(tmp_path.c_str());
            return false;
        }
        return true;
    } catch (...) {
        std::remove(tmp_path.c_str());
        return false;
    }
}

inline bool load_mime_tree(const std::string& body_path, MimePart& root);

// 取得 MIME 树：优先读 sidecar，没有就现场解析**并回写 sidecar**。
//
// 回写是关键：否则每次 FETCH 都要把整封邮件重新解析一遍。旧邮件、以及
// 超过 inbound_mime_parse_limit_bytes 而被 SMTP 侧跳过的大邮件，都走这条路。
// 回写失败无所谓，只是下次再解析一次，不影响本次结果。
inline bool ensure_mime_tree(const std::string& body_path,
                             const std::string& raw,
                             MimePart& root) {
    if (load_mime_tree(body_path, root) && root.metadata_version >= kMimeMetadataVersion) {
        return true;
    }
    if (raw.empty()) { root = MimePart{};return false; }
    root = MimePart{};
    parse_mime_tree(raw, root);
    if (root.type.empty()) {
        return false;
    }
    save_mime_tree(body_path, root);
    return true;
}

// 从 sidecar JSON 加载预解析的 MIME 树
// ⚠ 必须先清空 root：调用方会复用同一个 MimePart（如 IMAP 批量 FETCH 逐封拉
// BODYSTRUCTURE 时 ctx->struct_tree 跨邮件复用），不清空会导致第二封起残留/累积
// 上一封的 subs —— 批量 BODYSTRUCTURE 串台（如 A 邮件的结构出现在 B 邮件）。
// ensure_mime_tree 已有 root=MimePart{}，这里补上，2026-08-30 修复。
inline bool load_mime_tree(const std::string& body_path, MimePart& root) {
    root = MimePart{};
    std::string path = body_path + ".mime";
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (json.empty() || json[0] != '{') return false;

    std::function<void(const std::string&, size_t&, MimePart&)> parse;
    parse = [&](const std::string& s, size_t& pos, MimePart& p) {
        auto read_str = [&]() -> std::string {
            size_t start = s.find('"', pos); if (start == std::string::npos) return "";
            // 找到未被转义的收尾引号（save_mime_tree 会转义 " 和 \）
            size_t end = start + 1;
            while (end < s.size() && s[end] != '"') {
                if (s[end] == '\\' && end + 1 < s.size()) ++end;
                ++end;
            }
            if (end >= s.size()) return "";
            const std::string escaped = s.substr(start + 1, end - start - 1);
            pos = end + 1;

            std::string out;
            out.reserve(escaped.size());
            for (size_t i = 0; i < escaped.size(); ++i) {
                if (escaped[i] != '\\' || i + 1 >= escaped.size()) { out += escaped[i]; continue; }
                switch (escaped[++i]) {
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    default:   out += escaped[i]; break;
                }
            }
            return out;
        };
        auto read_uint = [&]() -> uint64_t {
            while (pos < s.size() && !std::isdigit(s[pos])) pos++;
            uint64_t v = 0;
            while (pos < s.size() && std::isdigit(s[pos])) { v = v * 10 + (s[pos] - '0'); pos++; }
            return v;
        };
        pos = s.find('{', pos); if (pos == std::string::npos) return;
        pos++; // skip {
        while (pos < s.size() && s[pos] != '}') {
            std::string key = read_str(); pos++; // skip :
            if (key == "v") p.metadata_version = static_cast<uint32_t>(read_uint());
            else if (key == "t") p.type = read_str();
            else if (key == "s") p.subtype = read_str();
            else if (key == "c") p.charset = read_str();
            else if (key == "e") p.encoding = read_str();
            else if (key == "b") p.boundary = read_str();
            else if (key == "n") p.name = read_str();
            else if (key == "d") p.disposition = read_str();
            else if (key == "i") p.content_id = read_str();
            else if (key == "o") p.offset = read_uint();
            else if (key == "l") p.length = read_uint();
            else if (key == "z") p.body_size = read_uint();
            else if (key == "ln") p.lines = read_uint();
            else if (key == "p") {
                pos = s.find('[', pos); if (pos == std::string::npos) return;
                pos++; // skip [
                while (pos < s.size() && s[pos] != ']') {
                    MimePart sub;
                    parse(s, pos, sub);
                    if (!sub.type.empty()) p.subs.push_back(std::move(sub));
                    if (pos < s.size() && s[pos] == ',') pos++;
                }
                if (pos < s.size()) pos++; // skip ]
            }
            if (pos < s.size() && s[pos] == ',') pos++;
        }
        if (pos < s.size()) pos++; // skip }
    };
    try {
        size_t pos = 0;
        parse(json, pos, root);
        return !root.type.empty();
    } catch (...) { return false; }
}

// 提取 MIME part 的正文：跳过该 part 自己的 MIME header，返回原始内容。
// 注意：BODY[<section>] 按 RFC 3501 返回的是原始编码内容（base64/quoted-printable
// 保持原样），由客户端按 BODYSTRUCTURE 的 body-fld-enc 自行解码；webmail 的
// mime_view.h 在此之上做 transfer-decode。
// raw 是完整 body 文件内容（含顶层 header + multipart），part 来自预解析
// MimePart 树（offset/length 为该 part 在 raw 中的字节区间，含其自己的 header）。
// （2026-10-18 自 imap_utils.cpp 上移：偏移/旧 sidecar 兼容语义与 mime_parser
// 同源，IMAP/webmail 两个读者不该各持一份。）
inline std::string extract_part_content(const std::string& raw, const MimePart& part) {
    size_t start = part.offset;
    size_t end = part.offset + part.length;
    if (start >= raw.size()) return {};
    if (end > raw.size()) end = raw.size();
    if (end <= start) return {};

    // 在 part 范围内定位 header/body 分隔
    size_t sep = raw.find("\r\n\r\n", start);
    if (sep == std::string::npos || sep >= end) {
        sep = raw.find("\n\n", start);
        if (sep == std::string::npos || sep >= end) return {};
    }
    size_t body_start = sep + (raw[sep] == '\r' ? 4 : 2);
    // 兼容旧 sidecar：非 multipart 根 part 的 length 可能只覆盖 header
    // （parse_mime_tree 的旧 bug）。此时 body 应从 header 之后延伸到消息末尾。
    if (body_start >= end && start == 0) end = raw.size();
    if (body_start >= end) return {};

    std::string body = raw.substr(body_start, end - body_start);
    // 去掉尾部空白行
    while (!body.empty() && (body.back() == '\r' || body.back() == '\n'))
        body.pop_back();
    return body;
}

} // namespace mail_system
