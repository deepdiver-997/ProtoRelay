// MIME 解析单元测试 — 构造合法 MIME，验证：
//   parse_mime_tree             （SMTP 收信时的预解析，写 sidecar 的数据源）
//   build_bodystructure_tree    （IMAP BODYSTRUCTURE 生成，RFC 3501 §7.4.2）
//   extract_part_content        （MIME part 正文提取，原始编码返回；
//                                 2026-10-18 自 imap_utils 上移至此，webmail 共用）
//
// 覆盖历史修复的 bug 类：multipart 格式、非 multipart 根 length、charset 不带引号、
// folded header boundary、DKIM h= 误匹配、BODY[n] 空内容。
// 2026-10-18：网易邮件大师不列附件，BODYSTRUCTURE 字段错位/缺 disposition；
// 见 docs/bugfixes/2026-10-18-imap-attachment-bodystructure.md。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "framework/storage/mapped_file.h"
#include "mail_system/back/entities/mail.h"
#include "mail_system/back/common/mime_parser.h"
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.h"
#include "mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.tpp"

using namespace mail_system;
using namespace pr;

struct DummyConn {};
using Fsm = TraditionalImapsFsm<DummyConn>;

static int g_pass = 0, g_fail = 0;

static void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}
static void expect_str(const std::string& got, const std::string& want, const char* what) {
    if (got == want) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s: got [%s] want [%s]\n", what, got.c_str(), want.c_str()); }
}
static void expect_contains(const std::string& hay, const std::string& needle, const char* what) {
    if (hay.find(needle) != std::string::npos) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s: [%s] not found in [%s]\n", what, needle.c_str(), hay.c_str()); }
}
static void expect_num(uint64_t got, uint64_t want, const char* what) {
    if (got == want) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s: got %llu want %llu\n", what,
        (unsigned long long)got, (unsigned long long)want); }
}

// ========== 用例 ==========

// 1. 单 part text/plain 7bit
static void test_single_text_plain() {
    const std::string raw =
        "From: a@test.com\r\n"
        "To: b@test.com\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "Hello world\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "text", "type");
    expect_str(root.subtype, "plain", "subtype");
    expect_str(root.charset, "utf-8", "charset");
    expect_str(root.encoding, "7bit", "encoding");
    // 根 part length 必须覆盖整封邮件（之前只到 header 末尾导致 BODY[1] 空）
    expect_num(root.length, raw.size(), "root.length == raw.size()");
    expect_num(root.body_size, 13, "body_size (Hello world + CRLF)");
    expect_num(root.lines, 1, "lines");
    // BODY[1] 提取
    expect_str(extract_part_content(raw, root), "Hello world", "BODY[1] content");
    // BODYSTRUCTURE
    std::string bs = imap_utils::build_bodystructure_tree(root);
    expect_contains(bs, "(\"text\" \"plain\" (\"CHARSET\" \"utf-8\") NIL NIL \"7bit\" 13 1", "BS single-part");
}

// 2. 单 part text/html quoted-printable，charset 不带引号
static void test_single_html_qp_unquoted_charset() {
    const std::string raw =
        "Content-Type: text/html; charset=us-ascii\r\n"
        "Content-Transfer-Encoding: quoted-printable\r\n"
        "\r\n"
        "<!doctype html><p>Hello=20world</p>\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "text", "type");
    expect_str(root.subtype, "html", "subtype");
    expect_str(root.charset, "us-ascii", "unquoted charset");
    expect_str(root.encoding, "quoted-printable", "encoding");
    expect_num(root.length, raw.size(), "root.length");
    expect_str(extract_part_content(raw, root),
               "<!doctype html><p>Hello=20world</p>", "BODY[1] QP content (raw)");
}

// 3. multipart/alternative，2 个 base64 子 part（QQ 邮件场景）
static void test_multipart_alternative_base64() {
    const std::string raw =
        "Content-Type: multipart/alternative; boundary=\"BOUND1\"\r\n"
        "\r\n"
        "--BOUND1\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "SGVsbG8gd29ybGQ=\r\n"
        "--BOUND1\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "PGgxPkhlbGxvPC9oMT4=\r\n"
        "--BOUND1--\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "multipart", "root type");
    expect_str(root.subtype, "alternative", "root subtype");
    expect_str(root.boundary, "BOUND1", "root boundary");
    expect_num(root.subs.size(), 2, "sub count");
    if (root.subs.size() == 2) {
        expect_str(root.subs[0].type, "text", "sub0 type");
        expect_str(root.subs[0].subtype, "plain", "sub0 subtype");
        expect_str(root.subs[0].encoding, "base64", "sub0 encoding");
        expect_str(root.subs[1].type, "text", "sub1 type");
        expect_str(root.subs[1].subtype, "html", "sub1 subtype");
        expect_str(root.subs[1].encoding, "base64", "sub1 encoding");
        // BODY[1] 返回原始 base64（RFC 3501：客户端自行解码）
        expect_str(extract_part_content(raw, root.subs[0]), "SGVsbG8gd29ybGQ=", "BODY[1] base64");
        expect_str(extract_part_content(raw, root.subs[1]), "PGgxPkhlbGxvPC9oMT4=", "BODY[2] base64");
    }
    // BODYSTRUCTURE 必须：子 part 在前、subtype 在后（RFC 3501 §7.4.2）
    std::string bs = imap_utils::build_bodystructure_tree(root);
    expect_true(bs.size() >= 2 && bs[0] == '(' && bs[1] == '(', "BS starts with (( (subparts first)");
    expect_contains(bs, "(\"text\" \"plain\" (\"CHARSET\" \"utf-8\") NIL NIL \"base64\"", "BS has text/plain subpart");
    expect_contains(bs, "(\"text\" \"html\" (\"CHARSET\" \"utf-8\") NIL NIL \"base64\"", "BS has text/html subpart");
    expect_contains(bs, "\"alternative\" (\"BOUNDARY\" \"BOUND1\")", "BS subtype+boundary last");
    expect_true(bs.find("(\"multipart\" \"alternative\"") == std::string::npos,
                "BS NOT in wrong single-part layout");
}

// 4. multipart/mixed，带 base64 附件
static void test_multipart_mixed_attachment() {
    const std::string raw =
        "Content-Type: multipart/mixed; boundary=SEP9\r\n"
        "\r\n"
        "--SEP9\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "Body text\r\n"
        "--SEP9\r\n"
        "Content-Type: application/octet-stream; name=\"file.bin\"\r\n"
        "Content-Disposition: attachment; filename=\"file.bin\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "AAECAwQ=\r\n"
        "--SEP9--\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "multipart", "root type");
    expect_str(root.subtype, "mixed", "root subtype");
    expect_str(root.boundary, "SEP9", "unquoted boundary");
    expect_num(root.subs.size(), 2, "sub count");
    if (root.subs.size() == 2) {
        expect_str(root.subs[1].name, "file.bin", "attachment name");
        expect_str(root.subs[1].encoding, "base64", "attachment encoding");
        expect_str(extract_part_content(raw, root.subs[1]), "AAECAwQ=", "attachment body (base64)");
    }
    std::string bs = imap_utils::build_bodystructure_tree(root);
    expect_contains(bs, "\"mixed\"", "BS subtype mixed");
}

// 5. 嵌套 multipart（mixed 内含 alternative）
static void test_nested_multipart() {
    const std::string raw =
        "Content-Type: multipart/mixed; boundary=OUTER1\r\n"
        "\r\n"
        "--OUTER1\r\n"
        "Content-Type: multipart/alternative; boundary=INNER1\r\n"
        "\r\n"
        "--INNER1\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "plain\r\n"
        "--INNER1\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "\r\n"
        "<b>html</b>\r\n"
        "--INNER1--\r\n"
        "--OUTER1\r\n"
        "Content-Type: application/pdf; name=\"doc.pdf\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "JVBERi0xLjQ=\r\n"
        "--OUTER1--\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.subtype, "mixed", "root subtype");
    expect_num(root.subs.size(), 2, "root sub count");
    if (root.subs.size() == 2) {
        expect_str(root.subs[0].subtype, "alternative", "nested multipart subtype");
        expect_num(root.subs[0].subs.size(), 2, "nested sub count");
        if (root.subs[0].subs.size() == 2) {
            expect_str(root.subs[0].subs[0].subtype, "plain", "nested leaf1");
            expect_str(root.subs[0].subs[1].subtype, "html", "nested leaf2");
        }
        expect_str(root.subs[1].name, "doc.pdf", "attachment name");
    }
    // 嵌套 BODYSTRUCTURE：内层 alternative 也必须是子 part 在前
    std::string bs = imap_utils::build_bodystructure_tree(root);
    expect_contains(bs, "\"alternative\" (\"BOUNDARY\" \"INNER1\")", "nested BS alternative");
}

// 6. folded Content-Type header（boundary 在续行）
static void test_folded_content_type() {
    const std::string raw =
        "DKIM-Signature: v=1; a=rsa-sha256; d=test.com;\r\n"
        "\tb=abc123\r\n"
        "Content-Type: multipart/alternative;\r\n"
        "\tboundary=\"FOLDED1\"\r\n"
        "\r\n"
        "--FOLDED1\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "folded test\r\n"
        "--FOLDED1--\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "multipart", "type with folded header");
    expect_str(root.subtype, "alternative", "subtype with folded header");
    expect_str(root.boundary, "FOLDED1", "boundary from folded line");
    expect_num(root.subs.size(), 1, "sub count");
    if (root.subs.size() == 1) {
        expect_str(extract_part_content(raw, root.subs[0]), "folded test", "BODY[1] folded");
    }
}

// 7. DKIM-Signature h= 含 "content-type:" 子串（不误匹配）
static void test_dkim_h_contains_content_type() {
    const std::string raw =
        "DKIM-Signature: v=1; a=rsa-sha256; h=content-type:date:from:subject;\r\n"
        "\tbh=abc;\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        "dkim test\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "text", "type not confused by DKIM h=");
    expect_str(root.subtype, "plain", "subtype not confused by DKIM h=");
    expect_str(root.charset, "utf-8", "charset");
    expect_str(extract_part_content(raw, root), "dkim test", "BODY[1]");
}

// 9. 畸形邮件：HTML 正文在前、真实 header 在 </html> 之后（如部分 OpenAI 通知邮件）。
//    首段（第一个 \r\n\r\n 之前）无 Content-Type，必须从整封消息找回退 content-type。
static void test_malformed_headers_after_body() {
    const std::string raw =
        "        <html>\r\n"
        "          <body style=\"color:#fff\">\r\n"
        "          </body>\r\n"      // HTML 里的空行会误当 header/body 分隔
        "        </html>\r\n"
        "\r\n"                       // 第一个空行在 HTML 内部
        "DKIM-Signature: v=1; a=rsa-sha256; h=content-type:date:from:subject;\r\n"
        "\tbh=abc;\r\n"
        "From: noreply@tm.openai.com\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Transfer-Encoding: quoted-printable\r\n"
        "\r\n"
        "trailing\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "text", "malformed: type via fallback");
    expect_str(root.subtype, "html", "malformed: subtype via fallback");
    expect_str(root.charset, "utf-8", "malformed: charset via fallback");
}

// 8. charset 带引号
static void test_quoted_charset() {
    const std::string raw =
        "Content-Type: text/plain; charset=\"gb2312\"\r\n"
        "\r\n"
        "quote test\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.charset, "gb2312", "quoted charset");
}

// 10. 不带引号的 boundary + 后面有带引号的 header（如 To: "test3"）
//     —— 之前 find('"') 向后搜会误匹配 To 的引号，把 boundary 提取成 "test3"
static void test_unquoted_boundary_with_quoted_to() {
    const std::string raw =
        "From: sender@feishu.cn\r\n"
        "To: \"test3\" <test3@test.local>\r\n"
        "Subject: test\r\n"
        "Content-Type: multipart/alternative;\r\n"
        "\tboundary=77c32bd97011b71999b9c42bd3ffb1141e49e616cb30dd3cf5dd4684ef41\r\n"
        "\r\n"
        "--77c32bd97011b71999b9c42bd3ffb1141e49e616cb30dd3cf5dd4684ef41\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "hello feishu\r\n"
        "--77c32bd97011b71999b9c42bd3ffb1141e49e616cb30dd3cf5dd4684ef41--\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.boundary, "77c32bd97011b71999b9c42bd3ffb1141e49e616cb30dd3cf5dd4684ef41",
               "unquoted boundary not confused by To: quote");
    expect_num(root.subs.size(), 1, "sub count");
    if (root.subs.size() == 1) {
        expect_str(extract_part_content(raw, root.subs[0]), "hello feishu", "BODY[1]");
    }
}

// 9. Content-Disposition filename 提取
static void test_content_disposition_name() {
    const std::string raw =
        "Content-Type: image/png; name=pic.png\r\n"
        "Content-Disposition: attachment; filename=\"pic.png\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "iVBORw0KGgo=\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_str(root.type, "image", "image type");
    expect_str(root.subtype, "png", "image subtype");
    expect_str(root.name, "pic.png", "filename from Content-Disposition");
}

// encoded-word 附件名必须保留大小写：Base64 大小写敏感，lowercase 后解码即损坏。
// 真实案例：QQ 邮箱 PDF 附件名在 sidecar 全小写，客户端解析失败（2026-09-14）。
static void test_encoded_word_name_case_preserved() {
    const std::string pdf_name_enc = "=?UTF-8?B?SW52b2ljZS1JTlYtQUktMjAyNi0wMDAwMDAwMDQ2RjkucGRm?=";
    const std::string raw =
        "Content-Type: multipart/mixed; boundary=\"----=_M_1\"\r\n"
        "\r\n"
        "------=_M_1\r\n"
        "Content-Type: text/plain; charset=UTF-8\r\n"
        "\r\n"
        "body\r\n"
        "------=_M_1\r\n"
        "Content-Type: application/pdf; name=\"" + pdf_name_enc + "\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "Content-Disposition: attachment; filename=\"" + pdf_name_enc + "\"\r\n"
        "\r\n"
        "JVBERi0xLjQK\r\n"
        "------=_M_1--\r\n";
    MimePart root;
    parse_mime_tree(raw, root);
    expect_num(root.subs.size(), 2, "two parts");
    expect_str(root.subs[1].name, pdf_name_enc,
               "encoded-word attachment name preserves case");
}

// 独立消费者：按 IMAP list/string/number/literal 语法读回响应，按 RFC 字段位置识别附件。
static nlohmann::json read_structure(const std::string& s, size_t& pos) {
    while (pos < s.size() && s[pos] == ' ') ++pos;
    assert(pos < s.size());
    if (s[pos] == '(') {
        ++pos;
        auto out = nlohmann::json::array();
        while (pos < s.size() && s[pos] != ')') {
            if (s[pos] == ' ') { ++pos; continue; }
            out.push_back(read_structure(s, pos));
        }
        assert(pos < s.size()); ++pos; return out;
    }
    if (s[pos] == '"') {
        ++pos; std::string out;
        while (pos < s.size() && s[pos] != '"') {
            if (s[pos] == '\\') ++pos;
            assert(pos < s.size());out += s[pos++];
        }
        assert(pos < s.size());++pos;return out;
    }
    if (s[pos] == '{') {
        auto end = s.find("}\r\n", ++pos);assert(end != std::string::npos);
        auto n = std::stoull(s.substr(pos, end - pos));pos = end + 3;
        assert(n <= s.size() - pos);auto out = s.substr(pos, n);pos += n;return out;
    }
    auto end = s.find_first_of(" )", pos);if (end == std::string::npos) end = s.size();
    auto atom = s.substr(pos, end - pos);pos = end;
    if (atom == "NIL") return nullptr;
    return std::stoull(atom);
}

static void test_attachment_structure_consumer() {
    const std::string raw =
        "Content-Type: multipart/mixed; boundary=AttB\r\n\r\n"
        "--AttB\r\nContent-Type: text/plain; charset=utf-8\r\n\r\nhello\r\n"
        "--AttB\r\nContent-Type: text/plain\r\nContent-Transfer-Encoding: base64\r\n"
        "Content-Disposition: attachment;\r\n"
        " filename*0*=UTF-8''%E9%99%84%E4%BB%B6;\r\n filename*1*=%E8%AF%B4%E6%98%8E.txt\r\n\r\naGk=\r\n"
        "--AttB\r\nContent-Type: image/png\r\nContent-ID: <pic@example.test>\r\n"
        "Content-Disposition: inline; filename=\"pic.png\"\r\n"
        "Content-Transfer-Encoding: base64\r\n\r\naGk=\r\n--AttB--\r\n";
    MimePart tree;parse_mime_tree(raw, tree);assert(tree.subs.size() == 3);
    const auto response = imap_utils::build_bodystructure_tree(tree);size_t pos = 0;
    const auto body = read_structure(response, pos);assert(pos == response.size());
    expect_str(tree.subs[1].name, "附件说明.txt", "RFC2231 continuation filename");
    expect_true(body[1][3].is_null(), "attachment name does not occupy body-id");
    expect_true(body[1][9].is_array(), "text attachment disposition at RFC position");
    if (body[1][9].is_array()) {
        expect_str(body[1][9][0].get<std::string>(), "attachment", "attachment disposition");
        expect_str(body[1][9][1][1].get<std::string>(), "附件说明.txt", "consumer filename");
    }
    expect_num(body[1].size(), 12, "text fields include line count and four extensions");
    expect_num(body[2].size(), 11, "binary fields omit text line count");
    expect_true(body[2][8].is_array(), "binary disposition at RFC position");
    if (body[2][8].is_array()) {
        expect_str(body[2][8][0].get<std::string>(), "inline", "inline disposition preserved");
        expect_str(body[2][8][1][1].get<std::string>(), "pic.png", "inline filename");
    }
    expect_true(body[2][3].is_string(), "Content-ID is an nstring");
    MimePart single;
    parse_mime_tree("Content-Type: application/pdf\r\n"
                    "Content-Disposition: attachment; filename*=utf-8''%E9%99%84%E4%BB%B6.pdf\r\n\r\nx", single);
    expect_str(single.name, "附件.pdf", "RFC2231 single extended filename");
    parse_mime_tree("Content-Type: image/png\r\n"
                    "Content-Disposition: attachment; filename=\"pi\\\"c\\\\name.png\"\r\n\r\nx", single);
    auto quoted = imap_utils::build_bodystructure_tree(single);pos = 0;
    const auto decoded = read_structure(quoted, pos);
    expect_str(decoded[8][1][1].get<std::string>(), "pi\"c\\name.png", "quoted-pair filename survives wire escaping");

    // 老 sidecar 不含 disposition/RFC2231 名称，升级后必须从原 MIME 重建并回写。
    const std::string path = "/tmp/mime_attachment_legacy.eml";
    { std::ofstream f(path);f << raw; }
    { std::ofstream f(path + ".mime");f << "{\"t\":\"multipart\",\"s\":\"mixed\",\"p\":[{\"t\":\"text\",\"s\":\"plain\"}]}"; }
    MimePart unavailable;
    expect_true(!ensure_mime_tree(path, "", unavailable), "old cache cannot be upgraded without source");
    MimePart upgraded;expect_true(ensure_mime_tree(path, raw, upgraded), "legacy cache upgraded");
    expect_num(upgraded.subs.size(), 3, "legacy cache does not hide attachments");
    std::ifstream f(path + ".mime");const std::string cache((std::istreambuf_iterator<char>(f)), {});
    expect_contains(cache, "\"v\":2", "upgraded MIME metadata version persisted");
    std::filesystem::remove(path);std::filesystem::remove(path + ".mime");
}

static void test_repeated_boundary_is_bounded() {
    const std::string header = "Content-Type: multipart/mixed; boundary=B\r\n\r\n";
    std::string raw = header;
    for (int i = 0; i < 24; ++i) raw += "--B\r\n" + header + "x\r\n";
    raw += "--B--\r\n";
    MimePart root;parse_mime_tree(raw, root);
    expect_num(root.subs.size(), 24, "reused boundary has 24 siblings");
    for (const auto& child : root.subs)
        expect_true(child.subs.empty(), "sibling is not recursively treated as descendant");
}

// ========== sidecar 往返 / 回写 ==========

// 临时目录下造一个正文文件，返回其路径
static std::string write_temp_body(const std::string& name, const std::string& raw) {
    const auto dir = std::filesystem::temp_directory_path() / "mime_parser_test";
    std::filesystem::create_directories(dir);
    const auto path = (dir / name).string();
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".mime");
    std::ofstream f(path, std::ios::binary);
    f.write(raw.data(), static_cast<std::streamsize>(raw.size()));
    f.close();
    return path;
}

static const std::string kMultipartRaw =
    "From: a@test.com\r\n"
    "Content-Type: multipart/alternative; boundary=\"BND1\"\r\n"
    "\r\n"
    "--BND1\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "\r\n"
    "aGVsbG8=\r\n"
    "--BND1\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Content-Transfer-Encoding: quoted-printable\r\n"
    "\r\n"
    "<b>hi</b>\r\n"
    "--BND1--\r\n";

// save → load 必须无损往返
static void test_sidecar_round_trip() {
    const std::string path = write_temp_body("roundtrip.eml", kMultipartRaw);

    MimePart original;
    parse_mime_tree(kMultipartRaw, original);
    expect_true(save_mime_tree(path, original), "save_mime_tree succeeds");
    expect_true(std::filesystem::exists(path + ".mime"), "sidecar file created");

    MimePart loaded;
    expect_true(load_mime_tree(path, loaded), "load_mime_tree succeeds");
    expect_str(loaded.type, original.type, "sidecar type round-trips");
    expect_str(loaded.subtype, original.subtype, "sidecar subtype round-trips");
    expect_str(loaded.boundary, original.boundary, "sidecar boundary round-trips");
    expect_num(loaded.subs.size(), original.subs.size(), "sidecar sub-part count round-trips");
    for (size_t i = 0; i < loaded.subs.size() && i < original.subs.size(); ++i) {
        expect_str(loaded.subs[i].subtype, original.subs[i].subtype, "sub subtype round-trips");
        expect_str(loaded.subs[i].encoding, original.subs[i].encoding, "sub encoding round-trips");
        expect_str(loaded.subs[i].charset, original.subs[i].charset, "sub charset round-trips");
        expect_str(loaded.subs[i].disposition, original.subs[i].disposition, "sub disposition round-trips");
        expect_str(loaded.subs[i].content_id, original.subs[i].content_id, "sub Content-ID round-trips");
        expect_num(loaded.subs[i].metadata_version, kMimeMetadataVersion, "sub metadata version round-trips");
        expect_num(loaded.subs[i].offset, original.subs[i].offset, "sub offset round-trips");
        expect_num(loaded.subs[i].length, original.subs[i].length, "sub length round-trips");
    }
}

// ensure_mime_tree 首次调用要落 sidecar，第二次直接命中
static void test_ensure_writes_back_sidecar() {
    const std::string path = write_temp_body("writeback.eml", kMultipartRaw);
    expect_true(!std::filesystem::exists(path + ".mime"), "no sidecar before first access");

    MimePart first;
    expect_true(ensure_mime_tree(path, kMultipartRaw, first), "ensure_mime_tree parses");
    expect_true(std::filesystem::exists(path + ".mime"), "sidecar written back on first access");
    expect_str(first.subtype, "alternative", "parsed subtype correct");

    // 第二次必须能纯靠 sidecar 命中（传空原文，解析不出东西也应成功）
    MimePart second;
    expect_true(ensure_mime_tree(path, "", second), "second access hits sidecar");
    expect_str(second.subtype, "alternative", "sidecar hit returns same tree");
    expect_num(second.subs.size(), 2, "sidecar hit keeps sub-parts");

    // 不得留下临时文件
    int tmp_leftovers = 0;
    for (const auto& e : std::filesystem::directory_iterator(
             std::filesystem::path(path).parent_path())) {
        if (e.path().string().find(".mime.tmp.") != std::string::npos) tmp_leftovers++;
    }
    expect_num((uint64_t)tmp_leftovers, 0, "no .tmp leftovers after atomic rename");
}

// 附件名里的引号/反斜杠必须能转义并读回
static void test_sidecar_escapes_filename() {
    const std::string raw =
        "From: a@test.com\r\n"
        "Content-Type: image/png\r\n"
        "Content-Disposition: attachment; filename=\"we\\\"ird\\\\name.png\"\r\n"
        "\r\n"
        "iVBORw0KGgo=\r\n";
    const std::string path = write_temp_body("escape.eml", raw);

    MimePart original;
    parse_mime_tree(raw, original);
    expect_true(save_mime_tree(path, original), "save with tricky filename succeeds");

    MimePart loaded;
    expect_true(load_mime_tree(path, loaded), "load with tricky filename succeeds");
    expect_str(loaded.name, original.name, "filename with quotes/backslashes round-trips");
}

// 向后兼容：线上已有的 sidecar 是旧代码写的（无转义），
// 新的 read_str 必须照样读得回来，否则一部署所有旧邮件的结构就没了。
static void test_loads_legacy_sidecar() {
    const std::string path = write_temp_body("legacy.eml", kMultipartRaw);
    // 取自生产环境 /opt/smtpServer/mail/2089048057244024832.mime 的真实内容
    const std::string legacy =
        "{\"t\":\"multipart\",\"s\":\"alternative\",\"e\":\"7bit\","
        "\"b\":\"----=_Part_561808_1606723650.1786902842953\","
        "\"o\":0,\"l\":1924,\"z\":425,\"ln\":12,\"p\":["
        "{\"t\":\"text\",\"s\":\"plain\",\"c\":\"gbk\",\"e\":\"base64\","
        "\"o\":1545,\"l\":96,\"z\":20,\"ln\":8},"
        "{\"t\":\"text\",\"s\":\"html\",\"c\":\"gbk\",\"e\":\"base64\","
        "\"o\":1689,\"l\":185,\"z\":110,\"ln\":3}]}";
    std::ofstream sf(path + ".mime", std::ios::binary | std::ios::trunc);
    sf << legacy;
    sf.close();

    MimePart t;
    expect_true(load_mime_tree(path, t), "legacy sidecar still loads");
    expect_str(t.type, "multipart", "legacy type");
    expect_str(t.subtype, "alternative", "legacy subtype");
    expect_str(t.boundary, "----=_Part_561808_1606723650.1786902842953", "legacy boundary");
    expect_num(t.length, 1924, "legacy length");
    expect_num(t.subs.size(), 2, "legacy sub-part count");
    expect_str(t.subs[0].charset, "gbk", "legacy sub charset");
    expect_num(t.subs[1].offset, 1689, "legacy sub offset");
}

// ========== mmap 路径 ==========

// 同一份内容，从 std::string 解析和从只读映射区解析必须得到完全一致的树
static void expect_same_tree(const MimePart& a, const MimePart& b, const char* what) {
    expect_str(a.type, b.type, what);
    expect_str(a.subtype, b.subtype, what);
    expect_str(a.charset, b.charset, what);
    expect_str(a.encoding, b.encoding, what);
    expect_str(a.boundary, b.boundary, what);
    expect_str(a.name, b.name, what);
    expect_str(a.disposition, b.disposition, what);
    expect_str(a.content_id, b.content_id, what);
    expect_num(a.metadata_version, b.metadata_version, what);
    expect_num(a.offset, b.offset, what);
    expect_num(a.length, b.length, what);
    expect_num(a.body_size, b.body_size, what);
    expect_num(a.lines, b.lines, what);
    expect_num(a.subs.size(), b.subs.size(), what);
    for (size_t i = 0; i < a.subs.size() && i < b.subs.size(); ++i) {
        expect_same_tree(a.subs[i], b.subs[i], what);
    }
}

static void parse_via_mmap(const std::string& name, const std::string& raw, MimePart& out) {
    const std::string path = write_temp_body(name, raw);
    std::string err;
    auto m = MappedFile::open(path, err);
    expect_true(m != nullptr, "MappedFile::open succeeds");
    if (!m) return;
    expect_num(m->size(), raw.size(), "mapped size matches file size");
    parse_mime_tree(m->view(), out);
}

static void test_mmap_parse_matches_string_parse() {
    MimePart from_string, from_mmap;
    parse_mime_tree(kMultipartRaw, from_string);
    parse_via_mmap("mmap_multipart.eml", kMultipartRaw, from_mmap);
    expect_same_tree(from_string, from_mmap, "mmap parse == string parse (multipart)");
}

// 回归：畸形报文（Content-Type 出现在 body 之后）会走到「整封转小写」的兜底分支。
// 那里若就地 tolower，就是往 PROT_READ 的映射页里写 → SIGSEGV。
// 这条用例保证该分支在只读映射上也能安全跑完。
static void test_mmap_malformed_fallback_does_not_write() {
    const std::string raw =
        "        <html>\r\n"
        "          <body style=\"color:#fff\">\r\n"
        "          </body>\r\n"
        "        </html>\r\n"
        "\r\n"
        "DKIM-Signature: v=1; a=rsa-sha256; h=content-type:date:from:subject;\r\n"
        "\tbh=abc;\r\n"
        "From: noreply@tm.openai.com\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Transfer-Encoding: quoted-printable\r\n"
        "\r\n"
        "trailing\r\n";

    MimePart from_string, from_mmap;
    parse_mime_tree(raw, from_string);
    parse_via_mmap("mmap_malformed.eml", raw, from_mmap);
    expect_str(from_mmap.type, "text", "mmap malformed: type via fallback");
    expect_str(from_mmap.subtype, "html", "mmap malformed: subtype via fallback");
    expect_same_tree(from_string, from_mmap, "mmap parse == string parse (malformed)");

    // 兜底分支不得改动被映射的文件内容
    const auto dir = std::filesystem::temp_directory_path() / "mime_parser_test";
    std::ifstream f((dir / "mmap_malformed.eml").string(), std::ios::binary);
    std::string after((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    expect_str(after, raw, "mapped file content unchanged after parse");
}

static void test_mmap_empty_file() {
    const std::string path = write_temp_body("mmap_empty.eml", "");
    std::string err;
    auto m = MappedFile::open(path, err);
    expect_true(m != nullptr, "empty file maps without error");
    if (m) {
        expect_true(m->empty(), "empty mapping reports empty");
        expect_num(m->size(), 0, "empty mapping size is 0");
    }
    std::string err2;
    expect_true(MappedFile::open("/nonexistent/path/xx.eml", err2) == nullptr,
                "missing file fails to map");
    expect_true(!err2.empty(), "failed map fills error string");
}

int main() {
    test_single_text_plain();
    test_single_html_qp_unquoted_charset();
    test_multipart_alternative_base64();
    test_multipart_mixed_attachment();
    test_nested_multipart();
    test_folded_content_type();
    test_dkim_h_contains_content_type();
    test_malformed_headers_after_body();
    test_quoted_charset();
    test_content_disposition_name();
    test_attachment_structure_consumer();
    test_repeated_boundary_is_bounded();
    test_encoded_word_name_case_preserved();
    test_unquoted_boundary_with_quoted_to();
    test_sidecar_round_trip();
    test_ensure_writes_back_sidecar();
    test_sidecar_escapes_filename();
    test_loads_legacy_sidecar();
    test_mmap_parse_matches_string_parse();
    test_mmap_malformed_fallback_does_not_write();
    test_mmap_empty_file();

    std::filesystem::remove_all(std::filesystem::temp_directory_path() / "mime_parser_test");

    std::printf("\n================================\n  Passed: %d  Failed: %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
