#ifndef TRADITIONAL_IMAPS_FSM_TPP
#define TRADITIONAL_IMAPS_FSM_TPP

#include <algorithm>
#include <cstring>

namespace mail_system {

// ====================================================================
// 工具方法
// ====================================================================

// RFC 3501 date-time: DD-Mon-YYYY HH:MM:SS +ZZZZ
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::imap_timestamp(time_t t) {
    struct tm result;
    memset(&result, 0, sizeof(result));
#ifdef _WIN32
    gmtime_s(&result, &t);
#else
    gmtime_r(&t, &result);
#endif
    static const char* months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d-%s-%04d %02d:%02d:%02d +0000",
             result.tm_mday, months[result.tm_mon],
             result.tm_year + 1900,
             result.tm_hour, result.tm_min, result.tm_sec);
    return std::string(buf);
}

// RFC 3501 quoted-string
// 注意：IMAP-UTF-7 编码的名称（以 & 开头）也需要加引号，
// 否则 VMime 等客户端会把 &...- 中的 '-' 当作名称的一部分而非编码结束符
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::quote_string(const std::string& s) {
    // Always quote if: empty, contains special chars, or is IMAP-UTF-7 encoded
    // IMAP-UTF-7 编码的名称（以 & 开头）需要加引号，
    // 纯 ASCII atom 不加引号
    bool needs_quote = s.empty() ||
                       s[0] == '&' ||
                       s.find_first_of("\"\\") != std::string::npos ||
                       s.find(' ') != std::string::npos;
    if (!needs_quote) {
        return s;
    }
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// ====================================================================
// 邮箱名编解码
//
// RFC 3501 §5.1.3: IMAP mailbox 名称必须用 modified UTF-7 编码。
// 编码方式：非 ASCII 连续块 → 取 UTF-16BE → modified Base64 → &...-
// (modified Base64: 用 ',' 代替 '/', 不加 '=' 填充)
// ====================================================================

// 辅助：modified Base64 编码表 & 解码表
namespace {
    const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+,";
}

// UTF-8 → modified UTF-7（用于 LIST/LSUB 响应）
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::encode_mailbox_name(const std::string& name) {
    std::string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
    if (upper == "INBOX") return "INBOX";

    std::string result;
    std::vector<uint16_t> buf;

    auto flush = [&]() {
        if (buf.empty()) return;
        std::vector<uint8_t> be;
        for (uint16_t u : buf) {
            be.push_back((u >> 8) & 0xFF);
            be.push_back(u & 0xFF);
        }
        result += '&';
        for (size_t i = 0; i < be.size(); i += 3) {
            uint8_t b[3] = {0}; int n = 0;
            for (int j = 0; j < 3 && i + j < be.size(); ++j, ++n) b[j] = be[i + j];
            uint32_t t = (b[0] << 16) | (b[1] << 8) | b[2];
            result += kBase64[(t >> 18) & 0x3F];
            result += kBase64[(t >> 12) & 0x3F];
            if (n >= 2) result += kBase64[(t >> 6) & 0x3F];
            if (n >= 3) result += kBase64[t & 0x3F];
        }
        result += '-';
        buf.clear();
    };

    size_t i = 0;
    while (i < name.size()) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x80) {
            flush();
            result += (c == '&') ? "&-" : std::string(1, static_cast<char>(c));
            i++;
        } else {
            uint32_t cp = 0; size_t extra = 0;
            if (c >= 0xF0)      { cp = c & 0x07; extra = 3; }
            else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
            else                { cp = c & 0x1F; extra = 1; }
            for (size_t j = 1; j <= extra; ++j)
                cp = (cp << 6) | (static_cast<unsigned char>(name[i + j]) & 0x3F);
            i += extra + 1;
            if (cp > 0xFFFF) {
                cp -= 0x10000;
                buf.push_back(0xD800 | ((cp >> 10) & 0x3FF));
                buf.push_back(0xDC00 | (cp & 0x3FF));
            } else {
                buf.push_back(static_cast<uint16_t>(cp));
            }
        }
    }
    flush();
    return result;
}

// 辅助：Base64 解码表（modified: 用 ',' 代替 '/'）
namespace {
    const int kBase64Decode[128] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    };
}

// modified UTF-7 → UTF-8（手动实现，不依赖 iconv）
// 处理 &base64- 序列以及 &- 转义
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::decode_mailbox_name(const std::string& imap7) {
    std::string result;

    size_t i = 0;
    while (i < imap7.size()) {
        char c = imap7[i];

        if (c == '&') {
            if (i + 1 < imap7.size() && imap7[i + 1] == '-') {
                // &- → 字面 '&'
                result += '&';
                i += 2;
            } else {
                // &...- → modified Base64 片段
                size_t end = imap7.find('-', i + 1);
                if (end == std::string::npos) {
                    result += c;
                    i++;
                    continue;
                }
                std::string b64 = imap7.substr(i + 1, end - i - 1);
                i = end + 1;

                // 解码 modified Base64 → 字节
                std::vector<uint8_t> bytes;
                uint32_t acc = 0;
                int bits = 0;
                for (char bc : b64) {
                    if (bc < 0 || bc >= 128) continue;
                    int val = kBase64Decode[static_cast<int>(bc)];
                    if (val < 0) continue;
                    acc = (acc << 6) | static_cast<uint32_t>(val);
                    bits += 6;
                    if (bits >= 8) {
                        bits -= 8;
                        bytes.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
                    }
                }

                // UTF-16BE → UTF-8
                for (size_t j = 0; j + 1 < bytes.size(); j += 2) {
                    uint16_t unit = (static_cast<uint16_t>(bytes[j]) << 8)
                                   | bytes[j + 1];

                    if (unit >= 0xD800 && unit <= 0xDBFF && j + 3 < bytes.size()) {
                        // 高 surrogate + 低 surrogate
                        uint16_t low = (static_cast<uint16_t>(bytes[j + 2]) << 8)
                                      | bytes[j + 3];
                        uint32_t cp = 0x10000
                            + ((unit - 0xD800) << 10)
                            + (low - 0xDC00);
                        result += static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
                        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (cp & 0x3F));
                        j += 2; // 跳过低 surrogate
                    } else if (unit >= 0xD800 && unit <= 0xDFFF) {
                        continue; // 孤立的 surrogate，跳过
                    } else if (unit < 0x80) {
                        result += static_cast<char>(unit);
                    } else if (unit < 0x800) {
                        result += static_cast<char>(0xC0 | ((unit >> 6) & 0x1F));
                        result += static_cast<char>(0x80 | (unit & 0x3F));
                    } else {
                        result += static_cast<char>(0xE0 | ((unit >> 12) & 0x0F));
                        result += static_cast<char>(0x80 | ((unit >> 6) & 0x3F));
                        result += static_cast<char>(0x80 | (unit & 0x3F));
                    }
                }
            }
        } else {
            result += c;
            i++;
        }
    }
    return result;
}

// 构建 flags 字符串
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::build_flags_string(
    int status, bool starred, bool deleted, bool important)
{
    std::string flags;
    // \Seen: status==0 (已读)
    if (status == 0) {
        flags += "\\Seen ";
    }
    if (starred) {
        flags += "\\Flagged ";
    }
    if (deleted) {
        flags += "\\Deleted ";
    }
    if (important) {
        flags += "\\Important ";
    }
    // 去除尾部空格
    if (!flags.empty() && flags.back() == ' ') {
        flags.pop_back();
    }
    return flags;
}

// 构建 ENVELOPE 响应
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::build_envelope_string(
    const std::string& date_str,
    const std::string& subject,
    const std::string& from,
    const std::string& sender,
    const std::string& reply_to,
    const std::string& to,
    const std::string& cc,
    const std::string& bcc,
    const std::string& in_reply_to,
    const std::string& message_id)
{
    // ENVELOPE( date, subject, from, sender, reply-to, to, cc, bcc, in-reply-to, message-id )
    // 每个地址列表是: ((name NIL addr host) ...)
    auto addr_to_list = [](const std::string& addr) -> std::string {
        if (addr.empty()) return "NIL";
        // Parse "user@domain" or "Name <user@domain>"
        std::string name = "NIL";
        std::string user;
        std::string domain;
        std::string input = addr;

        // Try to extract name part
        size_t angle_start = input.find('<');
        size_t angle_end = input.find('>');
        std::string addr_spec;
        if (angle_start != std::string::npos && angle_end != std::string::npos) {
            std::string before_angle = input.substr(0, angle_start);
            // trim
            before_angle.erase(0, before_angle.find_first_not_of(" \t\""));
            before_angle.erase(before_angle.find_last_not_of(" \t\"") + 1);
            if (!before_angle.empty()) {
                name = "\"" + before_angle + "\"";
            }
            addr_spec = input.substr(angle_start + 1, angle_end - angle_start - 1);
        } else {
            addr_spec = input;
        }

        size_t at_pos = addr_spec.find('@');
        if (at_pos != std::string::npos) {
            user = addr_spec.substr(0, at_pos);
            domain = addr_spec.substr(at_pos + 1);
        } else {
            user = addr_spec;
            domain = "";
        }

        if (user.empty()) user = "NIL";
        if (domain.empty()) domain = "NIL";

        return "((" + name + " NIL " + quote_string(user) + " " + quote_string(domain) + "))";
    };

    std::string out = "(";
    out += (date_str.empty() ? "NIL" : "\"" + date_str + "\"") + " ";
    out += (subject.empty() ? "NIL" : "\"" + subject + "\"") + " ";
    out += addr_to_list(from) + " ";
    out += addr_to_list(sender.empty() ? from : sender) + " ";
    out += (reply_to.empty() ? addr_to_list(from) : addr_to_list(reply_to)) + " ";
    out += addr_to_list(to) + " ";
    out += (cc.empty() ? "NIL" : addr_to_list(cc)) + " ";
    out += (bcc.empty() ? "NIL" : addr_to_list(bcc)) + " ";
    out += (in_reply_to.empty() ? "NIL" : "\"" + in_reply_to + "\"") + " ";
    out += (message_id.empty() ? "NIL" : "\"" + message_id + "\"");
    out += ")";
    return out;
}

// 构建 BODY[] 响应
template <typename ConnectionType>
std::string TraditionalImapsFsm<ConnectionType>::build_fetch_body_response(
    const std::string& body_content, size_t octets)
{
    if (body_content.empty()) {
        return "\"\"";
    }
    // Use literal: {size}\r\n<content>
    std::string out = "{" + std::to_string(body_content.size()) + "}\r\n";
    out += body_content;
    return out;
}

// ====================================================================
// 发送 IMAP 响应
// ====================================================================

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::send_untagged(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& data)
{
    SessionBase<ConnectionType>::do_async_write(
        std::move(session),
        "* " + data + "\r\n",
        nullptr // callback nullptr → 会自动继续读取
    );
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::send_tagged(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& tag,
    const std::string& status,
    const std::string& message)
{
    SessionBase<ConnectionType>::do_async_write(
        std::move(session),
        tag + " " + status + " " + message + "\r\n",
        nullptr
    );
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::send_continuation(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& message)
{
    SessionBase<ConnectionType>::do_async_write(
        std::move(session),
        "+ " + message + "\r\n",
        nullptr
    );
}

// ====================================================================
// 初始化：转换表 / 处理器
// ====================================================================

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::init_transition_table() {
    // INIT → NOT_AUTHENTICATED (on CONNECT)
    transition_table_[{ImapState::INIT, ImapEvent::CONNECT}] = ImapState::NOT_AUTHENTICATED;

    // NOT_AUTHENTICATED: stay on same state for most commands
    transition_table_[{ImapState::NOT_AUTHENTICATED, ImapEvent::CAPABILITY}] = ImapState::NOT_AUTHENTICATED;
    transition_table_[{ImapState::NOT_AUTHENTICATED, ImapEvent::LOGIN}] = ImapState::NOT_AUTHENTICATED; // may transition in handler
    transition_table_[{ImapState::NOT_AUTHENTICATED, ImapEvent::NOOP}] = ImapState::NOT_AUTHENTICATED;
    transition_table_[{ImapState::NOT_AUTHENTICATED, ImapEvent::LOGOUT}] = ImapState::LOGOUT;
    if constexpr (!std::is_same_v<ConnectionType, SslConnection>)
        transition_table_[{ImapState::NOT_AUTHENTICATED, ImapEvent::STARTTLS}] = ImapState::NOT_AUTHENTICATED;

    // AUTHENTICATED
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::SELECT}] = ImapState::SELECTED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::EXAMINE}] = ImapState::SELECTED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::CAPABILITY}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::LIST}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::LSUB}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::IMAP_STATUS}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::CREATE}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::DELETE}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::RENAME}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::SUBSCRIBE}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::UNSUBSCRIBE}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::APPEND}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::CHECK}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::CLOSE}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::NOOP}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::IDLE}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::AUTHENTICATED, ImapEvent::LOGOUT}] = ImapState::LOGOUT;

    // SELECTED
    transition_table_[{ImapState::SELECTED, ImapEvent::FETCH}] = ImapState::SELECTED;
    transition_table_[{ImapState::SELECTED, ImapEvent::STORE}] = ImapState::SELECTED;
    transition_table_[{ImapState::SELECTED, ImapEvent::SEARCH}] = ImapState::SELECTED;
    transition_table_[{ImapState::SELECTED, ImapEvent::COPY}] = ImapState::SELECTED;
    transition_table_[{ImapState::SELECTED, ImapEvent::MOVE}] = ImapState::SELECTED;
    transition_table_[{ImapState::SELECTED, ImapEvent::UID}] = ImapState::SELECTED;
    transition_table_[{ImapState::SELECTED, ImapEvent::EXPUNGE}] = ImapState::SELECTED;
    transition_table_[{ImapState::SELECTED, ImapEvent::CLOSE}] = ImapState::AUTHENTICATED;
    transition_table_[{ImapState::SELECTED, ImapEvent::CHECK}] = ImapState::SELECTED;
    transition_table_[{ImapState::SELECTED, ImapEvent::CAPABILITY}] = ImapState::SELECTED;
    transition_table_[{ImapState::SELECTED, ImapEvent::NOOP}] = ImapState::SELECTED;
    transition_table_[{ImapState::SELECTED, ImapEvent::APPEND}] = ImapState::SELECTED;
    transition_table_[{ImapState::SELECTED, ImapEvent::IDLE}] = ImapState::SELECTED;
    transition_table_[{ImapState::SELECTED, ImapEvent::LOGOUT}] = ImapState::LOGOUT;

    // ERROR / TIMEOUT everywhere (remain in current state)
    for (int i = 0; i <= static_cast<int>(ImapState::SELECTED); ++i) {
        auto s = static_cast<ImapState>(i);
        transition_table_[{s, ImapEvent::ERROR}] = s;
        transition_table_[{s, ImapEvent::TIMEOUT}] = s;
    }
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::init_state_handlers() {
    // INIT
    state_handlers_[ImapState::INIT][ImapEvent::CONNECT] =
        [this](auto session, auto args) { handle_init_connect(std::move(session), args); };

    // NOT_AUTHENTICATED
    state_handlers_[ImapState::NOT_AUTHENTICATED][ImapEvent::CAPABILITY] =
        [this](auto session, auto args) { handle_capability(std::move(session), args); };
    if constexpr (!std::is_same_v<ConnectionType, SslConnection>)
        state_handlers_[ImapState::NOT_AUTHENTICATED][ImapEvent::STARTTLS] =
            [this](auto session, auto args) { handle_starttls(std::move(session), args); };
    state_handlers_[ImapState::NOT_AUTHENTICATED][ImapEvent::LOGIN] =
        [this](auto session, auto args) { handle_login(std::move(session), args); };
    state_handlers_[ImapState::NOT_AUTHENTICATED][ImapEvent::NOOP] =
        [this](auto session, auto args) { handle_noop(std::move(session), args); };
    state_handlers_[ImapState::NOT_AUTHENTICATED][ImapEvent::LOGOUT] =
        [this](auto session, auto args) { handle_logout(std::move(session), args); };
    state_handlers_[ImapState::NOT_AUTHENTICATED][ImapEvent::ERROR] =
        [this](auto session, auto args) { handle_error(std::move(session), args); };
    state_handlers_[ImapState::NOT_AUTHENTICATED][ImapEvent::TIMEOUT] =
        [this](auto session, auto args) { handle_timeout(std::move(session), args); };

    // AUTHENTICATED
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::CAPABILITY] =
        [this](auto session, auto args) { handle_capability(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::SELECT] =
        [this](auto session, auto args) { handle_select(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::EXAMINE] =
        [this](auto session, auto args) { handle_examine(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::LIST] =
        [this](auto session, auto args) { handle_list(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::LSUB] =
        [this](auto session, auto args) { handle_lsub(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::IMAP_STATUS] =
        [this](auto session, auto args) { handle_status(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::CREATE] =
        [this](auto session, auto args) { handle_create(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::DELETE] =
        [this](auto session, auto args) { handle_delete(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::RENAME] =
        [this](auto session, auto args) { handle_rename(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::SUBSCRIBE] =
        [this](auto session, auto args) { handle_subscribe(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::UNSUBSCRIBE] =
        [this](auto session, auto args) { handle_unsubscribe(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::APPEND] =
        [this](auto session, auto args) { handle_append(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::CHECK] =
        [this](auto session, auto args) { handle_check(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::CLOSE] =
        [this](auto session, auto args) { handle_close(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::NOOP] =
        [this](auto session, auto args) { handle_noop(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::IDLE] =
        [this](auto session, auto args) { handle_idle(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::LOGOUT] =
        [this](auto session, auto args) { handle_logout(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::ERROR] =
        [this](auto session, auto args) { handle_error(std::move(session), args); };
    state_handlers_[ImapState::AUTHENTICATED][ImapEvent::TIMEOUT] =
        [this](auto session, auto args) { handle_timeout(std::move(session), args); };

    // SELECTED
    state_handlers_[ImapState::SELECTED][ImapEvent::FETCH] =
        [this](auto session, auto args) { handle_fetch(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::STORE] =
        [this](auto session, auto args) { handle_store(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::SEARCH] =
        [this](auto session, auto args) { handle_search(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::COPY] =
        [this](auto session, auto args) { handle_copy(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::MOVE] =
        [this](auto session, auto args) { handle_move(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::UID] =
        [this](auto session, auto args) { handle_uid(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::EXPUNGE] =
        [this](auto session, auto args) { handle_expunge(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::CLOSE] =
        [this](auto session, auto args) { handle_close(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::CHECK] =
        [this](auto session, auto args) { handle_check(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::APPEND] =
        [this](auto session, auto args) { handle_append(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::CAPABILITY] =
        [this](auto session, auto args) { handle_capability(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::NOOP] =
        [this](auto session, auto args) { handle_noop(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::IDLE] =
        [this](auto session, auto args) { handle_idle(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::LOGOUT] =
        [this](auto session, auto args) { handle_logout(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::ERROR] =
        [this](auto session, auto args) { handle_error(std::move(session), args); };
    state_handlers_[ImapState::SELECTED][ImapEvent::TIMEOUT] =
        [this](auto session, auto args) { handle_timeout(std::move(session), args); };
}

// ====================================================================
// 事件派发
// ====================================================================

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::process_event(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    ImapEvent event,
    const std::string& tag,
    const std::string& args)
{
    if constexpr (ENABLE_IMAP_DETAIL_DEBUG_LOG) {
        LOG_IMAP_DETAIL_DEBUG("Current State: {}, Event: {}, Tag: {}, Args: {}",
                          ImapsFsm<ConnectionType>::get_state_name(
                              static_cast<ImapState>(session->get_current_state())),
                          ImapsFsm<ConnectionType>::get_event_name(event),
                          tag, args);
    }

    // 保存 tag 到 context
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    if (ctx) {
        ctx->current_tag = tag;
    }

    if (static_cast<ImapState>(session->get_current_state()) == ImapState::LOGOUT ||
        static_cast<ImapState>(session->get_current_state()) == ImapState::CLOSED) {
        session->close();
        return;
    }

    auto transition_key = std::make_pair(
        static_cast<ImapState>(session->get_current_state()), event);
    auto transition_it = transition_table_.find(transition_key);

    if (transition_it != transition_table_.end()) {
        auto state_handler_it = state_handlers_.find(
            static_cast<ImapState>(session->get_current_state()));
        if (state_handler_it != state_handlers_.end()) {
            auto event_handler_it = state_handler_it->second.find(event);
            if (event_handler_it != state_handler_it->second.end()) {
                event_handler_it->second(std::move(session), args);
                return;
            }
        }
        // Handler not found — send BAD
        send_tagged(std::move(session), tag, "BAD", "Unsupported command in current state");
    } else {
        // Invalid transition
        send_tagged(std::move(session), tag, "BAD", "Invalid command sequence");
    }
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::auto_process_event(
    std::unique_ptr<SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    if (!ctx) return;

    // 如果是 IDLE 状态的特殊处理
    if (ctx->idle_mode) {
        handle_done(std::move(session), "");
        return;
    }

    // 从 session 获取待处理事件
    // 注意：session 中 event 和 args 在 parse 阶段已设置好
    // 这里需要一个适配: 从 session 获取 event type 和 args
    // ImapsSession 会在 parse 时设置这些值
    // 但现在 process_event 签名多了 tag 参数，auto_process_event 需要适配

    // 对于 IMAP，会话解析完命令后直接调用 process_event，
    // 这里留空作为兼容接口
}

// ====================================================================
// 状态处理器实现
// ====================================================================

// ---------- INIT → CONNECT → greeting ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_init_connect(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    session->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));

    // 发送 IMAP 欢迎语
    SessionBase<ConnectionType>::do_async_write(
        std::move(session),
        "* OK IMAP4rev1 Server Ready\r\n",
        [](std::unique_ptr<SessionBase<ConnectionType>> self,
           const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending IMAP greeting: {}", ec.message());
                return;
            }
            LOG_IMAP_DEBUG("Sent IMAP greeting, waiting for commands...");
            SessionBase<ConnectionType>::do_async_read(std::move(self), nullptr);
        }
    );
}

// ---------- CAPABILITY ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_capability(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    std::string caps = "* CAPABILITY IMAP4rev1 AUTH=LOGIN";
    // 非 SSL 连接可以通告 STARTTLS
    if constexpr (!std::is_same_v<ConnectionType, SslConnection>) {
        caps += " STARTTLS";
    }
    caps += " IDLE UIDPLUS MOVE\r\n";

    caps += tag + " OK CAPABILITY completed\r\n";

    SessionBase<ConnectionType>::do_async_write(
        std::move(session),
        caps,
        nullptr
    );
}

// ---------- LOGIN ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_login(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    // 解析 "username password"
    std::string username, password;
    size_t space = args.find(' ');
    if (space != std::string::npos && space > 0) {
        // 可能带引号
        if (args[0] == '"') {
            size_t end_quote = args.find('"', 1);
            if (end_quote != std::string::npos) {
                username = args.substr(1, end_quote - 1);
                password = args.substr(end_quote + 2); // skip " and space
            }
        } else {
            username = args.substr(0, space);
            password = args.substr(space + 1);
        }
        // trim password
        if (!password.empty() && password[0] == '"') {
            size_t end = password.find('"', 1);
            if (end != std::string::npos) {
                password = password.substr(1, end - 1);
            }
        }
    } else {
        username = args;
    }

    uint64_t user_id = 0;
    if (this->auth_user(session.get(), username, password, user_id)) {
        if (ctx) {
            ctx->is_authenticated = true;
            ctx->username = username;
            ctx->user_id = user_id;
        }
        session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));
        send_tagged(std::move(session), tag, "OK", "LOGIN completed");

        LOG_IMAP_INFO("IMAP login successful: {} (user_id={})", username, user_id);
    } else {
        LOG_IMAP_WARN("IMAP login failed: {}", username);
        send_tagged(std::move(session), tag, "NO", "LOGIN failed");
    }
}

// ---------- LOGOUT ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_logout(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    ctx->clear();

    session->set_current_state(static_cast<int>(ImapState::LOGOUT));

    std::string response = "* BYE IMAP4rev1 Server logging out\r\n";
    response += ctx->current_tag + " OK LOGOUT completed\r\n";

    SessionBase<ConnectionType>::do_async_write(
        std::move(session),
        response,
        [](std::unique_ptr<SessionBase<ConnectionType>> s,
           const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending LOGOUT reply: {}", ec.message());
                return;
            }
            auto timer = std::make_shared<boost::asio::steady_timer>(
                *s->get_server()->get_io_context());
            timer->expires_after(std::chrono::milliseconds(100));
            timer->async_wait([s = std::move(s), timer](const boost::system::error_code& ec) mutable {
                if (!ec) {
                    s->close();
                }
            });
        }
    );
}

// ---------- SELECT ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_select(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    std::string mailbox_name = args;
    // trim whitespace/quotes
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = mailbox_name.substr(1, end - 1);
        }
    }

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(std::move(session), tag, "BAD", "Not authenticated");
        return;
    }

    // 查找邮箱
    uint64_t mailbox_id = this->find_mailbox_id(ctx->user_id, mailbox_name);
    if (mailbox_id == 0) {
        send_tagged(std::move(session), tag, "NO", "Mailbox not found: " + mailbox_name);
        return;
    }

    // Save selected mailbox info
    ctx->mailbox_selected = true;
    ctx->selected_mailbox_name = mailbox_name;
    ctx->selected_mailbox_id = mailbox_id;
    ctx->read_only = false;

    // Generate UIDVALIDITY (use mailbox_id as validity)
    ctx->uid_validity = mailbox_id;

    size_t count = this->get_mailbox_count(mailbox_id, ctx->user_id);
    size_t unseen = this->get_mailbox_unseen_count(mailbox_id, ctx->user_id);
    uint64_t uidnext = this->get_mailbox_uidnext(mailbox_id, ctx->user_id);

    session->set_current_state(static_cast<int>(ImapState::SELECTED));

    // Build SELECT response
    std::string response;
    response += "* " + std::to_string(count) + " EXISTS\r\n";
    response += "* " + std::to_string(count - unseen) + " RECENT\r\n"; // approximate
    response += "* OK [UNSEEN " + std::to_string(unseen > 0 ? count - unseen + 1 : 1) + "]\r\n";
    response += "* OK [UIDVALIDITY " + std::to_string(ctx->uid_validity) + "]\r\n";
    response += "* OK [UIDNEXT " + std::to_string(uidnext) + "]\r\n";
    if (!ctx->read_only) {
        response += "* OK [READ-WRITE]\r\n";
    } else {
        response += "* OK [READ-ONLY]\r\n";
    }
    response += tag + " OK SELECT completed\r\n";

    SessionBase<ConnectionType>::do_async_write(std::move(session), response, nullptr);
}

// ---------- EXAMINE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_examine(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    std::string mailbox_name = args;
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = mailbox_name.substr(1, end - 1);
        }
    }

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(std::move(session), tag, "BAD", "Not authenticated");
        return;
    }

    uint64_t mailbox_id = this->find_mailbox_id(ctx->user_id, mailbox_name);
    if (mailbox_id == 0) {
        send_tagged(std::move(session), tag, "NO", "Mailbox not found: " + mailbox_name);
        return;
    }

    ctx->mailbox_selected = true;
    ctx->selected_mailbox_name = mailbox_name;
    ctx->selected_mailbox_id = mailbox_id;
    ctx->read_only = true;
    ctx->uid_validity = mailbox_id;

    size_t count = this->get_mailbox_count(mailbox_id, ctx->user_id);
    size_t unseen = this->get_mailbox_unseen_count(mailbox_id, ctx->user_id);
    uint64_t uidnext = this->get_mailbox_uidnext(mailbox_id, ctx->user_id);

    session->set_current_state(static_cast<int>(ImapState::SELECTED));

    std::string response;
    response += "* " + std::to_string(count) + " EXISTS\r\n";
    response += "* " + std::to_string(count - unseen) + " RECENT\r\n";
    response += "* OK [UNSEEN " + std::to_string(unseen > 0 ? count - unseen + 1 : 1) + "]\r\n";
    response += "* OK [UIDVALIDITY " + std::to_string(ctx->uid_validity) + "]\r\n";
    response += "* OK [UIDNEXT " + std::to_string(uidnext) + "]\r\n";
    response += "* OK [READ-ONLY]\r\n";
    response += tag + " OK EXAMINE completed\r\n";

    SessionBase<ConnectionType>::do_async_write(std::move(session), response, nullptr);
}

// ---------- LIST ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_list(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(std::move(session), tag, "BAD", "Not authenticated");
        return;
    }

    // LIST 参数: reference mailbox_name
    // 简单实现：列出用户所有邮箱
    std::vector<std::tuple<uint64_t, std::string, int>> mailboxes;
    if (!this->get_mailboxes(ctx->user_id, mailboxes)) {
        send_tagged(std::move(session), tag, "OK", "LIST completed");
        return;
    }

    std::string response;
    for (const auto& mb : mailboxes) {
        const std::string& name = std::get<1>(mb);
        int box_type = std::get<2>(mb);

        std::string encoded_name = encode_mailbox_name(name);

        // 如果是收件箱（box_type=1），也要以 INBOX 形式呈现
        if (box_type == 1) {
            // 发送 INBOX 和中文名两个条目
            response += "* LIST (\\HasNoChildren) \"/\" INBOX\r\n";
            response += "* LIST (\\HasNoChildren) \"/\" " + quote_string(encoded_name) + "\r\n";
            continue;
        }
        std::string attrs = "()";
        response += "* LIST " + attrs + " \"/\" " + quote_string(encoded_name) + "\r\n";
    }
    response += tag + " OK LIST completed\r\n";

    SessionBase<ConnectionType>::do_async_write(std::move(session), response, nullptr);
}

// ---------- LSUB ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_lsub(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(std::move(session), tag, "BAD", "Not authenticated");
        return;
    }

    // Simplified: return all mailboxes as subscribed
    std::vector<std::tuple<uint64_t, std::string, int>> mailboxes;
    this->get_mailboxes(ctx->user_id, mailboxes);

    std::string response;
    for (const auto& mb : mailboxes) {
        const std::string& name = std::get<1>(mb);
        int box_type = std::get<2>(mb);
        std::string encoded_name = encode_mailbox_name(name);

        if (box_type == 1) {
            response += "* LSUB (\\HasNoChildren) \"/\" INBOX\r\n";
        }
        response += "* LSUB () \"/\" " + quote_string(encoded_name) + "\r\n";
    }
    response += tag + " OK LSUB completed\r\n";

    SessionBase<ConnectionType>::do_async_write(std::move(session), response, nullptr);
}

// ---------- STATUS ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_status(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(std::move(session), tag, "BAD", "Not authenticated");
        return;
    }

    // Parse "mailbox (MESSAGES UNSEEN UIDNEXT UIDVALIDITY)"
    std::string mailbox_name = args;
    std::string status_attrs;

    size_t paren_open = args.find('(');
    if (paren_open != std::string::npos) {
        mailbox_name = args.substr(0, paren_open);
        // trim
        mailbox_name.erase(mailbox_name.find_last_not_of(" \t") + 1);
        size_t paren_close = args.find(')', paren_open);
        if (paren_close != std::string::npos) {
            status_attrs = args.substr(paren_open + 1, paren_close - paren_open - 1);
        }
    }

    // Trim quotes from mailbox name
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = mailbox_name.substr(1, end - 1);
        }
    }

    uint64_t mailbox_id = this->find_mailbox_id(ctx->user_id, mailbox_name);
    if (mailbox_id == 0) {
        send_tagged(std::move(session), tag, "NO", "Mailbox not found");
        return;
    }

    size_t messages = this->get_mailbox_count(mailbox_id, ctx->user_id);
    size_t unseen = this->get_mailbox_unseen_count(mailbox_id, ctx->user_id);
    uint64_t uidnext = this->get_mailbox_uidnext(mailbox_id, ctx->user_id);
    uint64_t uidvalidity = mailbox_id;

    std::string response = "* STATUS " + quote_string(encode_mailbox_name(mailbox_name)) + " (";
    if (status_attrs.find("MESSAGES") != std::string::npos || status_attrs.empty()) {
        response += "MESSAGES " + std::to_string(messages) + " ";
    }
    if (status_attrs.find("UNSEEN") != std::string::npos || status_attrs.empty()) {
        response += "UNSEEN " + std::to_string(unseen) + " ";
    }
    if (status_attrs.find("UIDNEXT") != std::string::npos || status_attrs.empty()) {
        response += "UIDNEXT " + std::to_string(uidnext) + " ";
    }
    if (status_attrs.find("UIDVALIDITY") != std::string::npos || status_attrs.empty()) {
        response += "UIDVALIDITY " + std::to_string(uidvalidity) + " ";
    }
    // Remove trailing space and close
    if (response.back() == ' ') response.pop_back();
    response += ")\r\n";
    response += tag + " OK STATUS completed\r\n";

    SessionBase<ConnectionType>::do_async_write(std::move(session), response, nullptr);
}

// ---------- FETCH ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_fetch(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(std::move(session), tag, "BAD", "No mailbox selected");
        return;
    }

    // Parse: <sequence-set> <message-data-item-names>
    // e.g. "1:* (FLAGS INTERNALDATE RFC822.SIZE ENVELOPE)"
    // or   "1:* (BODY[])"
    size_t space = args.find(' ');
    if (space == std::string::npos) {
        send_tagged(std::move(session), tag, "BAD", "FETCH requires arguments");
        return;
    }

    std::string seq_set = args.substr(0, space);
    std::string attrs = args.substr(space + 1);
    // trim ()
    if (!attrs.empty() && attrs[0] == '(') {
        size_t close = attrs.find(')');
        if (close != std::string::npos) {
            attrs = attrs.substr(1, close - 1);
        }
    }

    // 获取邮箱所有邮件
    std::vector<typename ImapsFsm<ConnectionType>::MailboxMailInfo> mails;
    if (!this->get_mailbox_mails(ctx->selected_mailbox_id, ctx->user_id, mails) || mails.empty()) {
        send_tagged(std::move(session), tag, "OK", "FETCH completed (empty)");
        return;
    }

    bool want_flags = attrs.find("FLAGS") != std::string::npos || attrs.find("ALL") != std::string::npos || attrs.find("FAST") != std::string::npos;
    bool want_internaldate = attrs.find("INTERNALDATE") != std::string::npos || attrs.find("ALL") != std::string::npos;
    bool want_rfc822_size = attrs.find("RFC822.SIZE") != std::string::npos || attrs.find("ALL") != std::string::npos || attrs.find("FAST") != std::string::npos;
    bool want_envelope = attrs.find("ENVELOPE") != std::string::npos || attrs.find("ALL") != std::string::npos;
    bool want_body = attrs.find("BODY") != std::string::npos;
    [[maybe_unused]] bool want_body_struct = attrs.find("BODYSTRUCTURE") != std::string::npos;

    // Determine sequence range
    uint64_t seq_start = 1;
    uint64_t seq_end = mails.size();
    if (seq_set.find(':') != std::string::npos) {
        size_t colon = seq_set.find(':');
        std::string start_str = seq_set.substr(0, colon);
        std::string end_str = seq_set.substr(colon + 1);
        if (start_str == "*") seq_start = 1;
        else seq_start = std::stoull(start_str);
        if (end_str == "*") seq_end = mails.size();
        else seq_end = std::min((uint64_t)std::stoull(end_str), (uint64_t)mails.size());
    } else if (seq_set == "*") {
        seq_start = 1;
        seq_end = mails.size();
    } else {
        seq_start = std::stoull(seq_set);
        seq_end = seq_start;
    }

    // Clamp
    if (seq_start < 1) seq_start = 1;
    if (seq_end > mails.size()) seq_end = mails.size();
    if (seq_start > seq_end) {
        send_tagged(std::move(session), tag, "OK", "FETCH completed");
        return;
    }

    // Build response
    std::string response;
    // 从 seq_start 到 seq_end（注意 mails 是按 send_time DESC 排的）
    // 序列号: mail 在列表的下标 + 1
    for (uint64_t seq = seq_start; seq <= seq_end; ++seq) {
        size_t idx = seq - 1;
        const auto& mail_info = mails[idx];

        response += "* " + std::to_string(seq) + " FETCH (";
        if (want_flags) {
            std::string flags = build_flags_string(
                mail_info.status,
                mail_info.is_starred,
                mail_info.is_deleted,
                mail_info.is_important);
            response += "FLAGS (" + flags + ") ";
        }
        if (want_internaldate) {
            response += "INTERNALDATE \"" + imap_timestamp(mail_info.send_time) + "\" ";
        }
        if (want_rfc822_size) {
            // Get body size
            std::string body = this->read_mail_body(mail_info.body_path);
            response += "RFC822.SIZE " + std::to_string(body.size()) + " ";
        }
        if (want_envelope) {
            // Get sender/recipients
            std::string sender = mail_info.sender;
            std::string to = mail_info.recipient;
            std::string date_str;
            {
                // Generate envelope date from send_time
                struct tm result;
                memset(&result, 0, sizeof(result));
                time_t t = mail_info.send_time;
#ifdef _WIN32
                gmtime_s(&result, &t);
#else
                gmtime_r(&t, &result);
#endif
                char buf[32];
                snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d +0000",
                         "Day", result.tm_mday, "Mon",
                         result.tm_year + 1900,
                         result.tm_hour, result.tm_min, result.tm_sec);
                date_str = buf;
            }
            std::string envelope = build_envelope_string(
                date_str,
                mail_info.subject,
                sender,
                sender,
                "", // reply-to
                to,
                "", // cc
                "", // bcc
                "", // in-reply-to
                std::to_string(mail_info.mail_id) // message-id
            );
            response += "ENVELOPE " + envelope + " ";
        }
        if (want_body) {
            std::string body_content = this->read_mail_body(mail_info.body_path);
            // BODY[] (entire message)
            response += "BODY[] " + build_fetch_body_response(body_content, body_content.size()) + " ";
        }
        // Remove trailing space
        if (response.back() == ' ') response.pop_back();
        response += ")\r\n";
    }

    response += tag + " OK FETCH completed\r\n";

    SessionBase<ConnectionType>::do_async_write(std::move(session), response, nullptr);
}

// ---------- STORE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_store(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(std::move(session), tag, "BAD", "No mailbox selected");
        return;
    }

    // Parse: <sequence-set> <data-item> <value>
    // e.g. "1:* +FLAGS (\\Seen \\Flagged)"
    //      "2:4 FLAGS (\\Deleted)"
    size_t space1 = args.find(' ');
    if (space1 == std::string::npos) {
        send_tagged(std::move(session), tag, "BAD", "STORE requires arguments");
        return;
    }

    std::string seq_set = args.substr(0, space1);
    std::string rest = args.substr(space1 + 1);

    // Handle optional SILENT
    bool silent = false;
    size_t space2 = rest.find(' ');
    if (space2 == std::string::npos) {
        send_tagged(std::move(session), tag, "BAD", "STORE requires flags");
        return;
    }

    std::string store_cmd = rest.substr(0, space2);
    std::string flags_part = rest.substr(space2 + 1);

    // Check for SILENT
    if (store_cmd.find("SILENT") != std::string::npos) {
        silent = true;
        std::string cmd_upper = store_cmd;
        std::transform(cmd_upper.begin(), cmd_upper.end(), cmd_upper.begin(), ::toupper);
        if (cmd_upper.find("FLAGS") != std::string::npos) {
            store_cmd = "FLAGS";
        } else if (cmd_upper.find("+FLAGS") != std::string::npos) {
            store_cmd = "+FLAGS";
        } else if (cmd_upper.find("-FLAGS") != std::string::npos) {
            store_cmd = "-FLAGS";
        }
    }

    // Parse flags from parentheses
    if (!flags_part.empty() && flags_part[0] == '(') {
        size_t close = flags_part.find(')');
        if (close != std::string::npos) {
            flags_part = flags_part.substr(1, close - 1);
        }
    }

    bool flag_seen = flags_part.find("\\Seen") != std::string::npos;
    bool flag_flagged = flags_part.find("\\Flagged") != std::string::npos;
    bool flag_deleted = flags_part.find("\\Deleted") != std::string::npos;
    bool add = store_cmd.find('+') != std::string::npos || (store_cmd.find("FLAGS") != std::string::npos && store_cmd[0] != '-');
    bool remove = store_cmd.find('-') != std::string::npos;

    // Get mails
    std::vector<typename ImapsFsm<ConnectionType>::MailboxMailInfo> mails;
    this->get_mailbox_mails(ctx->selected_mailbox_id, ctx->user_id, mails);
    if (mails.empty()) {
        send_tagged(std::move(session), tag, "OK", "STORE completed");
        return;
    }

    // Parse sequence set
    uint64_t seq_start = 1, seq_end = mails.size();
    if (!parse_seq_set(seq_set, seq_start, seq_end, mails.size())) {
        send_tagged(std::move(session), tag, "BAD", "Invalid sequence set");
        return;
    }

    std::string user_email = this->get_user_email(ctx->user_id);
    std::string response;

    for (uint64_t seq = seq_start; seq <= seq_end; ++seq) {
        size_t idx = seq - 1;
        const auto& mail_info = mails[idx];

        if ((add || !remove) && flag_seen) {
            this->update_mail_seen(mail_info.mail_id, user_email, true);
        }
        if (remove && flag_seen) {
            this->update_mail_seen(mail_info.mail_id, user_email, false);
        }
        if ((add || !remove) && flag_flagged) {
            this->update_mail_flagged(mail_info.mail_id, ctx->user_id, ctx->selected_mailbox_id, true);
        }
        if (remove && flag_flagged) {
            this->update_mail_flagged(mail_info.mail_id, ctx->user_id, ctx->selected_mailbox_id, false);
        }
        if ((add || !remove) && flag_deleted) {
            this->update_mail_deleted(mail_info.mail_id, ctx->user_id, ctx->selected_mailbox_id, true);
        }
        if (remove && flag_deleted) {
            this->update_mail_deleted(mail_info.mail_id, ctx->user_id, ctx->selected_mailbox_id, false);
        }

        if (!silent) {
            int new_status = (add && flag_seen) ? 0 : mail_info.status;
            if (remove && flag_seen) new_status = 1;

            std::string flags = build_flags_string(
                new_status,
                (add && flag_flagged) ? true : (remove && flag_flagged) ? false : mail_info.is_starred,
                (add && flag_deleted) ? true : (remove && flag_deleted) ? false : mail_info.is_deleted,
                mail_info.is_important);
            response += "* " + std::to_string(seq) + " FETCH (FLAGS (" + flags + "))\r\n";
        }
    }

    response += tag + " OK STORE completed\r\n";
    SessionBase<ConnectionType>::do_async_write(std::move(session), response, nullptr);
}

// ---------- EXPUNGE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_expunge(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(std::move(session), tag, "BAD", "No mailbox selected");
        return;
    }

    // Get mails before expunge
    std::vector<typename ImapsFsm<ConnectionType>::MailboxMailInfo> mails;
    this->get_mailbox_mails(ctx->selected_mailbox_id, ctx->user_id, mails);

    // Find which sequences are deleted
    std::vector<uint64_t> expunged_seqs;
    for (size_t i = 0; i < mails.size(); ++i) {
        if (mails[i].is_deleted) {
            expunged_seqs.push_back(i + 1);
        }
    }

    // Actually delete from database
    this->expunge_mailbox(ctx->selected_mailbox_id, ctx->user_id);

    std::string response;
    for (auto seq : expunged_seqs) {
        response += "* " + std::to_string(seq) + " EXPUNGE\r\n";
    }
    response += tag + " OK EXPUNGE completed\r\n";

    SessionBase<ConnectionType>::do_async_write(std::move(session), response, nullptr);
}

// ---------- CLOSE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_close(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());

    // If SELECTED, expunge deleted mails first
    if (static_cast<ImapState>(session->get_current_state()) == ImapState::SELECTED && ctx) {
        this->expunge_mailbox(ctx->selected_mailbox_id, ctx->user_id);
    }

    // Clear mailbox selection
    if (ctx) {
        ctx->mailbox_selected = false;
        ctx->selected_mailbox_name.clear();
        ctx->selected_mailbox_id = 0;
        ctx->read_only = false;
    }

    session->set_current_state(static_cast<int>(ImapState::AUTHENTICATED));

    std::string tag = ctx ? ctx->current_tag : "*";
    send_tagged(std::move(session), tag, "OK", "CLOSE completed");
}

// ---------- NOOP ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_noop(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    send_tagged(std::move(session), tag, "OK", "NOOP completed");
}

// ---------- CHECK ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_check(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    send_tagged(std::move(session), tag, "OK", "CHECK completed");
}

// ---------- STARTTLS ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_starttls(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    // STARTTLS only available on TCP connections (not already SSL)
    // Handoff logic: send OK, extract TCP socket, call server handoff
    SessionBase<ConnectionType>::do_async_write(
        std::move(session),
        tag + " OK Begin TLS negotiation now\r\n",
        [](std::unique_ptr<SessionBase<ConnectionType>> self,
           const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending STARTTLS response: {}", ec.message());
                return;
            }
            auto server = self->get_server();
            auto tcp_sock = self->release_connection()->release_socket();
            server->handoff_starttls_socket(std::move(tcp_sock));
        }
    );
}

// ---------- CREATE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_create(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(std::move(session), tag, "BAD", "Not authenticated");
        return;
    }

    std::string mailbox_name = args;
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = mailbox_name.substr(1, end - 1);
        }
    }

    if (mailbox_name.empty()) {
        send_tagged(std::move(session), tag, "BAD", "CREATE requires mailbox name");
        return;
    }

    // 解码 IMAP-UTF-7 → UTF-8（客户端发来的名称可能是编码后的）
    mailbox_name = this->decode_mailbox_name(mailbox_name);

    auto connection = this->m_dbPool->get_connection();
    if (!connection || !connection->is_connected()) {
        send_tagged(std::move(session), tag, "NO", "Server error");
        return;
    }

    if (connection->execute(
            "INSERT INTO mailboxes (user_id, name, is_system, box_type) VALUES (?, ?, 0, 0)",
            {std::to_string(ctx->user_id), mailbox_name})) {
        send_tagged(std::move(session), tag, "OK", "CREATE completed");
    } else {
        send_tagged(std::move(session), tag, "NO", "CREATE failed (maybe already exists)");
    }
}

// ---------- DELETE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_delete(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(std::move(session), tag, "BAD", "Not authenticated");
        return;
    }

    std::string mailbox_name = args;
    if (!mailbox_name.empty() && mailbox_name[0] == '"') {
        size_t end = mailbox_name.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = mailbox_name.substr(1, end - 1);
        }
    }

    uint64_t mailbox_id = this->find_mailbox_id(ctx->user_id, mailbox_name);
    if (mailbox_id == 0) {
        send_tagged(std::move(session), tag, "NO", "Mailbox not found");
        return;
    }

    auto connection = this->m_dbPool->get_connection();
    if (!connection || !connection->is_connected()) {
        send_tagged(std::move(session), tag, "NO", "Server error");
        return;
    }

    // Check if it's a system mailbox
    auto result = connection->query(
        "SELECT is_system FROM mailboxes WHERE id = ?",
        {std::to_string(mailbox_id)});
    if (result && result->get_row_count() > 0) {
        if (result->get_value(0, "is_system") == "1") {
            send_tagged(std::move(session), tag, "NO", "Cannot delete system mailbox");
            return;
        }
    }

    if (connection->execute("DELETE FROM mail_mailbox WHERE mailbox_id = ?",
                            {std::to_string(mailbox_id)}) &&
        connection->execute("DELETE FROM mailboxes WHERE id = ?",
                            {std::to_string(mailbox_id)})) {
        send_tagged(std::move(session), tag, "OK", "DELETE completed");
    } else {
        send_tagged(std::move(session), tag, "NO", "DELETE failed");
    }
}

// ---------- RENAME ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_rename(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(std::move(session), tag, "BAD", "Not authenticated");
        return;
    }

    // Parse old_name new_name
    std::string old_name, new_name;
    size_t space = args.find(' ');
    if (space != std::string::npos) {
        old_name = args.substr(0, space);
        new_name = args.substr(space + 1);
    }

    // Trim quotes
    auto trim_quotes = [](std::string& s) {
        if (!s.empty() && s[0] == '"') {
            size_t end = s.find('"', 1);
            if (end != std::string::npos) s = s.substr(1, end - 1);
        }
    };
    trim_quotes(old_name);
    trim_quotes(new_name);

    if (old_name.empty() || new_name.empty()) {
        send_tagged(std::move(session), tag, "BAD", "RENAME requires old and new names");
        return;
    }

    uint64_t mailbox_id = this->find_mailbox_id(ctx->user_id, old_name);
    if (mailbox_id == 0) {
        send_tagged(std::move(session), tag, "NO", "Mailbox not found");
        return;
    }

    // 新名称也要解码（客户端发来的可能是 IMAP-UTF-7 编码）
    new_name = this->decode_mailbox_name(new_name);

    auto connection = this->m_dbPool->get_connection();
    if (!connection || !connection->is_connected()) {
        send_tagged(std::move(session), tag, "NO", "Server error");
        return;
    }

    if (connection->execute("UPDATE mailboxes SET name = ? WHERE id = ?",
                            {new_name, std::to_string(mailbox_id)})) {
        send_tagged(std::move(session), tag, "OK", "RENAME completed");
    } else {
        send_tagged(std::move(session), tag, "NO", "RENAME failed");
    }
}

// ---------- SUBSCRIBE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_subscribe(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    // Simplified: always OK
    send_tagged(std::move(session), tag, "OK", "SUBSCRIBE completed");
}

// ---------- UNSUBSCRIBE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_unsubscribe(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    send_tagged(std::move(session), tag, "OK", "UNSUBSCRIBE completed");
}

// ---------- APPEND ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_append(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated) {
        send_tagged(std::move(session), tag, "BAD", "Not authenticated");
        return;
    }

    if (ctx->pending_append_preamble.empty()) {
        send_tagged(std::move(session), tag, "BAD", "APPEND missing preamble");
        return;
    }

    // 解析 preamble: mailbox (flags) "InternalDate"
    std::string preamble = ctx->pending_append_preamble;
    std::string mailbox_name;
    std::string flags_str;

    // 提取邮箱名（可能带引号）
    if (preamble[0] == '"') {
        size_t end = preamble.find('"', 1);
        if (end != std::string::npos) {
            mailbox_name = preamble.substr(1, end - 1);
            std::string rest = preamble.substr(end + 1);
            // trim leading spaces
            rest.erase(0, rest.find_first_not_of(" \t"));
            if (!rest.empty() && rest[0] == '(') {
                size_t paren = rest.find(')');
                if (paren != std::string::npos) {
                    flags_str = rest.substr(0, paren + 1);
                }
            }
        }
    } else {
        size_t sp = preamble.find(' ');
        if (sp != std::string::npos) {
            mailbox_name = preamble.substr(0, sp);
            std::string rest = preamble.substr(sp + 1);
            rest.erase(0, rest.find_first_not_of(" \t"));
            if (!rest.empty() && rest[0] == '(') {
                size_t paren = rest.find(')');
                if (paren != std::string::npos) {
                    flags_str = rest.substr(0, paren + 1);
                }
            }
        } else {
            mailbox_name = preamble;
        }
    }

    // 解析 flags
    int init_status = 0; // 0=read
    if (flags_str.find("\\Seen") != std::string::npos || flags_str.find("\\seen") != std::string::npos) {
        init_status = 0;
    } else if (flags_str.find("\\Unseen") != std::string::npos || flags_str.find("\\Draft") != std::string::npos) {
        init_status = 1; // unread / draft
    }
    if (flags_str.find("\\Deleted") != std::string::npos) {
        init_status = 5; // deleted
    }

    // 解码邮箱名（IMAP-UTF-7 → UTF-8）
    mailbox_name = this->decode_mailbox_name(mailbox_name);

    // 查找目标邮箱
    uint64_t target_mbox_id = this->find_mailbox_id(ctx->user_id, mailbox_name);
    if (target_mbox_id == 0) {
        send_tagged(std::move(session), tag, "NO", "APPEND failed: mailbox not found");
        return;
    }

    // 正文内容即 literal 数据（args）
    std::string body_content = args;
    std::string subject = "(APPEND)";

    // 尝试提取 Subject（正文第一行或全文首 200 字符）
    if (!body_content.empty()) {
        std::istringstream ss(body_content);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.find("Subject:") == 0 || line.find("subject:") == 0) {
                subject = line.substr(8);
                // trim
                subject.erase(0, subject.find_first_not_of(" \t"));
                break;
            }
        }
    }

    // 保存邮件
    std::string body_path;
    std::string error;
    uint64_t mail_id = this->create_mail(subject, body_content, body_path, error);
    if (mail_id == 0) {
        send_tagged(std::move(session), tag, "NO", "APPEND failed: " + error);
        return;
    }

    // 关联到邮箱
    std::string user_email = this->get_user_email(ctx->user_id);
    if (user_email.empty()) user_email = ctx->username;
    this->link_mail_to_mailbox(mail_id, ctx->user_id, target_mbox_id,
                                user_email, user_email, init_status);

    // 返回 APPENDUID
    uint64_t uidvalidity = target_mbox_id;
    std::string response = tag + " OK [APPENDUID " + std::to_string(uidvalidity)
                          + " " + std::to_string(mail_id) + "] APPEND completed\r\n";
    SessionBase<ConnectionType>::do_async_write(std::move(session), response, nullptr);

    LOG_IMAP_INFO("APPEND: mail_id={}, mailbox={}, user={}", mail_id, mailbox_name, ctx->username);
}

// ---------- SEARCH ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_search(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(std::move(session), tag, "BAD", "No mailbox selected");
        return;
    }

    std::vector<typename ImapsFsm<ConnectionType>::MailboxMailInfo> mails;
    this->get_mailbox_mails(ctx->selected_mailbox_id, ctx->user_id, mails);

    // 解析搜索关键词（简单实现常用关键词）
    std::string upper_args = args;
    std::transform(upper_args.begin(), upper_args.end(), upper_args.begin(), ::toupper);

    bool search_unseen = (upper_args.find("UNSEEN") != std::string::npos
                         || upper_args.find("NEW") != std::string::npos);
    bool search_seen = (upper_args.find("SEEN") != std::string::npos
                       && upper_args.find("UNSEEN") == std::string::npos
                       && upper_args.find("UNSEEN") == std::string::npos);
    bool search_deleted = (upper_args.find("DELETED") != std::string::npos
                          && upper_args.find("UNDELETED") == std::string::npos);

    std::string response = "* SEARCH";
    for (size_t i = 0; i < mails.size(); ++i) {
        const auto& m = mails[i];
        size_t seq = i + 1;

        bool match = true;
        if (search_unseen) match = (m.status == 1);
        else if (search_seen) match = (m.status == 0);
        if (search_deleted) match = m.is_deleted;

        if (match) {
            response += " " + std::to_string(seq);
        }
    }
    response += "\r\n";
    response += tag + " OK SEARCH completed\r\n";

    SessionBase<ConnectionType>::do_async_write(std::move(session), response, nullptr);
}

// ---------- UID ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_uid(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(std::move(session), tag, "BAD", "No mailbox selected");
        return;
    }

    size_t space = args.find(' ');
    if (space == std::string::npos) {
        send_tagged(std::move(session), tag, "BAD", "UID requires subcommand");
        return;
    }

    std::string subcmd = args.substr(0, space);
    std::string subargs = args.substr(space + 1);
    std::transform(subcmd.begin(), subcmd.end(), subcmd.begin(), ::toupper);

    if (subcmd == "FETCH") {
        handle_fetch(std::move(session), subargs);
    } else if (subcmd == "STORE") {
        handle_store(std::move(session), subargs);
    } else if (subcmd == "SEARCH") {
        handle_search(std::move(session), subargs);
    } else if (subcmd == "COPY") {
        handle_copy(std::move(session), subargs);
    } else {
        send_tagged(std::move(session), tag, "BAD", "Unknown UID subcommand");
    }
}

// ---------- COPY / MOVE (共享实现) ----------
// 把当前选中的邮箱中的一组邮件复制/移动到目标邮箱
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_copy_move(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args,
    bool is_move)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx || !ctx->is_authenticated || !ctx->mailbox_selected) {
        send_tagged(std::move(session), tag, "BAD", "No mailbox selected");
        return;
    }

    // 解析 "seq_set mailbox"
    size_t sp = args.rfind(' ');
    if (sp == std::string::npos) {
        send_tagged(std::move(session), tag, "BAD", "COPY/MOVE requires sequence set and mailbox");
        return;
    }

    std::string seq_set = args.substr(0, sp);
    std::string target_name = args.substr(sp + 1);
    // trim quotes
    if (!target_name.empty() && target_name[0] == '"') {
        size_t end = target_name.find('"', 1);
        if (end != std::string::npos) target_name = target_name.substr(1, end - 1);
    }
    target_name = this->decode_mailbox_name(target_name);

    uint64_t target_id = this->find_mailbox_id(ctx->user_id, target_name);
    if (target_id == 0) {
        send_tagged(std::move(session), tag, "NO", "COPY/MOVE failed: mailbox not found");
        return;
    }

    // 获取所有邮件，建立序号→mail_id 映射
    std::vector<typename ImapsFsm<ConnectionType>::MailboxMailInfo> mails;
    this->get_mailbox_mails(ctx->selected_mailbox_id, ctx->user_id, mails);

    std::vector<uint64_t> mail_ids;
    if (seq_set == "*") {
        for (const auto& m : mails) mail_ids.push_back(m.mail_id);
    } else if (seq_set.find(':') != std::string::npos) {
        size_t colon = seq_set.find(':');
        uint64_t start = std::stoull(seq_set.substr(0, colon));
        uint64_t end_val = std::stoull(seq_set.substr(colon + 1));
        if (end_val > mails.size()) end_val = mails.size();
        for (uint64_t s = start; s <= end_val && s <= mails.size(); ++s) {
            mail_ids.push_back(mails[s - 1].mail_id);
        }
    } else {
        uint64_t n = std::stoull(seq_set);
        if (n >= 1 && n <= mails.size())
            mail_ids.push_back(mails[n - 1].mail_id);
    }

    if (mail_ids.empty()) {
        send_tagged(std::move(session), tag, "OK", "COPY/MOVE completed (no messages)");
        return;
    }

    // 获取用户邮箱
    std::string user_email = this->get_user_email(ctx->user_id);
    if (user_email.empty()) user_email = ctx->username;

    auto conn = this->m_dbPool->get_connection();
    if (!conn || !conn->is_connected()) {
        send_tagged(std::move(session), tag, "NO", "Server error");
        return;
    }

    int copied = 0;
    for (uint64_t mid : mail_ids) {
        // 检查是否已在目标邮箱
        auto existing = conn->query(
            "SELECT id FROM mail_mailbox WHERE mail_id = ? AND mailbox_id = ? AND user_id = ?",
            {std::to_string(mid), std::to_string(target_id), std::to_string(ctx->user_id)});
        if (existing && existing->get_row_count() > 0) continue; // 已存在

        int64_t mmid = algorithm::get_snowflake_generator().next_id();
        if (conn->execute(
                "INSERT INTO mail_mailbox (id, mail_id, mailbox_id, user_id, is_starred, "
                "is_important, is_deleted, add_time) VALUES (?, ?, ?, ?, 0, 0, 0, NOW())",
                {std::to_string(mmid), std::to_string(mid), std::to_string(target_id),
                 std::to_string(ctx->user_id)})) {
            copied++;

            // MOVE: 从源邮箱删除
            if (is_move) {
                this->update_mail_deleted(mid, ctx->user_id, ctx->selected_mailbox_id, true);
                // 发送 EXPUNGE 通知
                // 简化处理：不逐个发 untagged EXPUNGE
            }
        }
    }

    std::string cmd_name = is_move ? "MOVE" : "COPY";
    std::string response = tag + " OK " + cmd_name + " completed (" + std::to_string(copied) + " messages)\r\n";
    SessionBase<ConnectionType>::do_async_write(std::move(session), response, nullptr);
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_copy(
    std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args) {
    handle_copy_move(std::move(session), args, false);
}

template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_move(
    std::unique_ptr<SessionBase<ConnectionType>> session, const std::string& args) {
    handle_copy_move(std::move(session), args, true);
}

// ---------- IDLE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_idle(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";

    if (!ctx) {
        send_tagged(std::move(session), tag, "BAD", "Not authenticated");
        return;
    }

    ctx->idle_mode = true;

    // RFC 2177: send continuation
    SessionBase<ConnectionType>::do_async_write(
        std::move(session),
        "+ idling\r\n",
        [](std::unique_ptr<SessionBase<ConnectionType>> s,
           const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending IDLE continuation: {}", ec.message());
                return;
            }
            // Continue reading — DONE command will be sent by client
            // NOTE: IDLE push notifications require a background polling
            // mechanism that is beyond the current scope. The IMAP server
            // accepts IDLE/DONE correctly; new-mail notifications (untagged
            // EXISTS) will be sent when the client exits IDLE and issues
            // a NOOP or re-selects the mailbox.
            SessionBase<ConnectionType>::do_async_read(std::move(s), nullptr);
        }
    );
}

// ---------- DONE ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_done(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    if (!ctx) return;

    ctx->idle_mode = false;

    std::string tag = ctx->current_tag;
    send_tagged(std::move(session), tag, "OK", "IDLE terminated");
}

// ---------- ERROR ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_error(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    auto* ctx = static_cast<ImapContext*>(session->get_context());
    std::string tag = ctx ? ctx->current_tag : "*";
    send_tagged(std::move(session), tag, "BAD", args.empty() ? "Protocol error" : args);
}

// ---------- TIMEOUT ----------
template <typename ConnectionType>
void TraditionalImapsFsm<ConnectionType>::handle_timeout(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args)
{
    SessionBase<ConnectionType>::do_async_write(
        std::move(session),
        "* BYE TIMEOUT\r\n",
        [](std::unique_ptr<SessionBase<ConnectionType>> s,
           const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_IMAP_ERROR("Error sending TIMEOUT BYE: {}", ec.message());
                return;
            }
            s->close();
        }
    );
}

// ========== 辅助函数 ==========

// 序列集解析（简单实现，仅处理数字和星号范围）
template <typename ConnectionType>
bool TraditionalImapsFsm<ConnectionType>::parse_seq_set(
    const std::string& seq_set, uint64_t& start, uint64_t& end, size_t total)
{
    if (seq_set.find(':') != std::string::npos) {
        size_t colon = seq_set.find(':');
        std::string s = seq_set.substr(0, colon);
        std::string e = seq_set.substr(colon + 1);
        start = (s == "*") ? 1 : std::stoull(s);
        end = (e == "*") ? total : std::min((uint64_t)std::stoull(e), (uint64_t)total);
    } else if (seq_set == "*") {
        start = 1;
        end = total;
    } else {
        start = std::stoull(seq_set);
        end = start;
    }
    if (start < 1) start = 1;
    if (end > total) end = total;
    return start <= end;
}

} // namespace mail_system

#endif // TRADITIONAL_IMAPS_FSM_TPP
