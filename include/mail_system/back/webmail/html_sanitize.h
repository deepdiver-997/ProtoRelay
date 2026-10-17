#ifndef MAIL_SYSTEM_WEBMAIL_HTML_SANITIZE_H
#define MAIL_SYSTEM_WEBMAIL_HTML_SANITIZE_H

// ================================================================
// html_sanitize — HTML 邮件白名单清洗（kickoff 设计决策 5 的第一道防线）
//
// 策略：白名单而非黑名单。标签/属性只放行明单内的；其余一律剥除。
//   - 脚本类元素（script/style/iframe/object/embed/svg/math/...）连内容整块剥
//   - 事件属性（on*）/style 属性/自定义属性一律不留（无属性白名单外的出口）
//   - <a href> 只放 http/https/mailto 绝对地址；<img> 的 src 默认整体剥除
//     （kickoff：远程图片默认代理/禁用——MVP 无代理基建，先禁；alt 文本保留）
//   - 注释/DOCTYPE/PI 整段剥（IE 条件注释是历史载体）
//   - 文本节点 & < > 重新转义；非法 '<' 按字面转义
//
// ⚠ 定位与边界：这是无依赖的保守过滤，挡住已知注入面，但不宣称对全部
//   mXSS 免疫。红线要求前端**必须**叠加 sandbox iframe（无 same-origin、
//   无脚本）+ CSP 渲染本函数的输出——两层都不可省。
// ================================================================

#include <cctype>
#include <string>

namespace mail_system::webmail {
namespace html_sanitize {

namespace detail {

inline bool is_tag_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == ':' ||
           c == '_';
}

inline std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline void append_escaped(std::string& out, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            default:   out.push_back(c); break;
        }
    }
}

inline void append_utf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) { out.push_back(static_cast<char>(cp)); }
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// 实体解码（保守集：命名 amp/lt/gt/quot/apos + 十/十六进制数字引用）。
// 输入是原文片段，输出是"浏览器解码一次后"的字节——之后统一重新转义，
// 保证恰好一次解码语义（避免 &amp; → &amp;amp; 双重转义，同时让
// javascript&#58; 这类实体编码 scheme 在解码后的白名单校验里现形）。
inline std::string decode_entities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] != '&') { out.push_back(s[i++]); continue; }
        const size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 12) { out.push_back(s[i++]); continue; }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp")        { out.push_back('&');  i = semi + 1; }
        else if (ent == "lt")    { out.push_back('<');  i = semi + 1; }
        else if (ent == "gt")    { out.push_back('>');  i = semi + 1; }
        else if (ent == "quot")  { out.push_back('"');  i = semi + 1; }
        else if (ent == "apos")  { out.push_back('\''); i = semi + 1; }
        else if (ent.size() >= 2 && ent[0] == '#') {
            const char* begin = ent.c_str() + 1;
            char* end = nullptr;
            const int base = (ent[1] == 'x' || ent[1] == 'X') ? 16 : 10;
            if (base == 16) ++begin;
            const unsigned long cp = std::strtoul(begin, &end, base);
            if (end && *end == '\0' && cp > 0 && cp < 0x11000) {
                append_utf8(out, cp);
                i = semi + 1;
            } else {
                out.push_back('&'); ++i;
            }
        } else {
            out.push_back('&'); ++i;   // 未知实体按字面保留
        }
    }
    return out;
}

inline bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// URL scheme 白名单：绝对 http/https/mailto 才放行（相对地址在邮件语境里
// 本就无意义；实体编码/控制字符混杂的 scheme 过不了这层前缀匹配 = 剥除）
inline bool safe_url(const std::string& raw) {
    size_t i = 0;
    while (i < raw.size() && (is_ws(raw[i]) || static_cast<unsigned char>(raw[i]) < 0x20))
        ++i;
    const std::string rest = lower_ascii(raw.substr(i));
    return rest.rfind("http://", 0) == 0 || rest.rfind("https://", 0) == 0 ||
           rest.rfind("mailto:", 0) == 0;
}

inline const char* const kAllowedTags[] = {
    "a", "abbr", "b", "bdi", "bdo", "blockquote", "br", "caption", "center",
    "cite", "code", "col", "colgroup", "dd", "del", "dfn", "div", "dl", "dt",
    "em", "figcaption", "figure", "font", "h1", "h2", "h3", "h4", "h5", "h6",
    "hr", "i", "img", "ins", "kbd", "li", "mark", "ol", "p", "pre", "q", "s",
    "samp", "small", "span", "strike", "strong", "sub", "sup", "table",
    "tbody", "td", "tfoot", "th", "thead", "tr", "u", "ul",
};

// 连内容整块剥除的元素（找不到闭合标签就剥到串尾——宁可错杀，html 邮件
// 里这些元素的"内容"本来就不该被看见）
inline bool is_strip_with_content(const std::string& tag) {
    return tag == "script" || tag == "style" || tag == "iframe" ||
           tag == "object" || tag == "embed" || tag == "applet" ||
           tag == "title" || tag == "svg" || tag == "math" || tag == "noscript" ||
           tag == "template" || tag == "head" || tag == "frame" ||
           tag == "frameset" || tag == "xmp" || tag == "plaintext" ||
           tag == "textarea";
}

inline bool is_allowed_tag(const std::string& tag) {
    for (const char* t : kAllowedTags)
        if (tag == t) return true;
    return false;
}

// 属性白名单（按标签；无单项即无属性）。值都会重转义输出。
// 返回值：0 = 剥除属性；1 = 原样值放行；2 = URL 校验后放行
inline int attr_policy(const std::string& tag, const std::string& attr) {
    if (tag == "a" && attr == "href") return 2;
    if (tag == "a" && attr == "title") return 1;
    if (tag == "img") {
        if (attr == "alt" || attr == "width" || attr == "height" ||
            attr == "align") return 1;
        return 0;   // src 及其余一律剥（远程图片默认禁用）
    }
    if (tag == "font" && (attr == "color" || attr == "face" || attr == "size"))
        return 1;
    if ((tag == "td" || tag == "th" || tag == "table" || tag == "tr") &&
        (attr == "bgcolor" || attr == "width" || attr == "height" ||
         attr == "colspan" || attr == "rowspan" || attr == "align" ||
         attr == "valign")) return 1;
    if (attr == "colspan" || attr == "rowspan") return 1;
    return 0;
}

} // namespace detail

// 主入口。输入假定 ≤ 数 MB（读信路径已有 1MB 解码上限），线性扫描。
inline std::string sanitize(const std::string& in) {
    std::string out;
    out.reserve(in.size() + in.size() / 8);
    const size_t n = in.size();
    // strip-with-content 搜索用的小写副本（整个循环共用一份，避免每标签重拷）
    const std::string hay = detail::lower_ascii(in);
    size_t i = 0;

    while (i < n) {
        if (in[i] != '<') {                       // 文本节点
            const size_t lt = in.find('<', i);
            const std::string chunk =
                in.substr(i, (lt == std::string::npos ? n : lt) - i);
            detail::append_escaped(out, detail::decode_entities(chunk));
            i = (lt == std::string::npos) ? n : lt;
            continue;
        }

        // ── '<' 的分支：注释 / DOCTYPE / PI / 闭合标签 / 开标签 / 字面 '<' ──
        if (in.compare(i, 4, "<!--") == 0) {      // 注释整段剥
            size_t e = in.find("-->", i + 4);
            i = (e == std::string::npos) ? n : e + 3;
            continue;
        }
        if (in.compare(i, 2, "<!") == 0) {        // DOCTYPE/CDATA 剥到 '>'
            size_t e = in.find('>', i + 2);
            i = (e == std::string::npos) ? n : e + 1;
            continue;
        }
        if (in.compare(i, 2, "<?") == 0) {        // PI 剥到 '>'
            size_t e = in.find('>', i + 2);
            i = (e == std::string::npos) ? n : e + 1;
            continue;
        }

        const bool closing = (i + 1 < n && in[i + 1] == '/');
        size_t p = i + 1 + (closing ? 1 : 0);
        if (p >= n || !std::isalpha(static_cast<unsigned char>(in[p]))) {
            out += "&lt;";                        // "a < b" 的字面 <
            ++i;
            continue;
        }
        const size_t name_start = p;
        while (p < n && detail::is_tag_name_char(in[p])) ++p;
        const std::string name = detail::lower_ascii(in.substr(name_start, p - name_start));

        if (closing) {
            // 闭合标签：白名单内原样重发（自闭合无意义但无害，也放行）
            size_t e = in.find('>', p);
            if (e == std::string::npos) { i = n; continue; }
            i = e + 1;
            if (detail::is_allowed_tag(name)) {
                out += "</" + name + ">";
            }
            continue;
        }

        // 开标签：解析属性区（引号/单引号/无引号），到 '>' 或串尾
        struct Attr { std::string name; std::string value; };
        std::string attrs;                        // 已重序列化的属性串
        bool ok_attr_area = true;
        while (ok_attr_area) {
            while (p < n && detail::is_ws(in[p])) ++p;
            if (p >= n) { i = n; ok_attr_area = false; break; }   // 未闭合标签：整体丢弃
            if (in[p] == '>') { ++p; break; }
            if (in[p] == '/' && p + 1 < n && in[p + 1] == '>') {  // 自闭合
                ++p;
                break;
            }
            // 属性名
            const size_t an = p;
            while (p < n && in[p] != '=' && in[p] != '>' && !detail::is_ws(in[p]) &&
                   in[p] != '/')
                ++p;
            std::string attr_name = detail::lower_ascii(in.substr(an, p - an));
            // 防 '/>' 被吞进属性名（src= 后无值的情况）
            while (!attr_name.empty() && attr_name.back() == '/') attr_name.pop_back();
            if (attr_name.empty()) { ++p; continue; }
            // 属性值
            std::string attr_value;
            size_t q = p;
            while (q < n && detail::is_ws(in[q])) ++q;
            if (q < n && in[q] == '=') {
                ++q;
                while (q < n && detail::is_ws(in[q])) ++q;
                if (q < n && (in[q] == '"' || in[q] == '\'')) {
                    const char quote = in[q];
                    const size_t vs = ++q;
                    while (q < n && in[q] != quote) ++q;
                    attr_value = in.substr(vs, q - vs);
                    p = (q < n) ? q + 1 : n;
                } else {
                    const size_t vs = q;
                    while (q < n && !detail::is_ws(in[q]) && in[q] != '>')
                        ++q;
                    attr_value = in.substr(vs, q - vs);
                    p = q;
                }
            } else {
                p = q;                            // 无值属性（checked 等）
            }
            attr_value = detail::decode_entities(attr_value);

            const int policy = detail::attr_policy(name, attr_name);
            if (policy == 0) continue;
            if (policy == 2 && !detail::safe_url(attr_value)) continue;
            attrs += ' ';
            attrs += attr_name;
            attrs += "=\"";
            detail::append_escaped(attrs, attr_value);
            attrs += '"';
        }
        if (!ok_attr_area) continue;              // i 已置 n

        if (detail::is_strip_with_content(name)) {
            // 连内容剥到首个闭合标签（大小写不敏感），找不到就剥到尾
            const std::string closer = "</" + name;
            size_t e = hay.find(closer, p);
            i = (e == std::string::npos) ? n : e + closer.size();
            // 吃掉闭合标签的 '>'
            if (i < n) {
                const size_t gt = in.find('>', i);
                i = (gt == std::string::npos) ? n : gt + 1;
            }
            continue;
        }
        if (!detail::is_allowed_tag(name)) continue;   // 未知标签：剥标签留文本

        out += '<';
        out += name;
        out += attrs;
        out += '>';
        i = p;
    }
    return out;
}

} // namespace html_sanitize
} // namespace mail_system::webmail

#endif // MAIL_SYSTEM_WEBMAIL_HTML_SANITIZE_H
