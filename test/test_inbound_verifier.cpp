/**
 * test_inbound_verifier.cpp — inbound verifier 组件单元测试
 *
 * 覆盖 DKIM 签名解析、头解析、SPF 机制匹配、DMARC 对齐、
 * body/header 规范化、以及完整的 DKIM 验证流程。
 *
 * 编译（macOS 本地）:
 *   g++ -std=c++20 -O0 -g -o test_inbound_verifier test/test_inbound_verifier.cpp \
 *       -lssl -lcrypto -I include -I/opt/homebrew/opt/openssl/include \
 *       -L/opt/homebrew/opt/openssl/lib
 *
 * 运行: ./test_inbound_verifier
 */

#include "mail_system/back/common/mail_crypto.h"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace mail_system::outbound;

// ================================================================
// 测试框架（轻量，无外部依赖）
// ================================================================
static int g_pass = 0, g_fail = 0;

void check(const char* name, bool cond) {
    if (cond) { g_pass++; } else { std::cerr << "  FAIL: " << name << std::endl; g_fail++; }
}

#define TEST(name) do { std::cout << "  " << name << " ... "; } while(0)
#define OK() do { g_pass++; } while(0)

// ================================================================
// 从 inbound_verifier.cpp 复制的辅助函数（确保测试的是实际逻辑）
// ================================================================

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

void split(const std::string& s, char delim, std::vector<std::string>& out) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) out.push_back(trim(item));
}

std::unordered_map<std::string, std::string> parse_tags(const std::string& raw) {
    std::unordered_map<std::string, std::string> tags;
    std::string current;
    bool in_quotes = false;
    for (size_t i = 0; i < raw.size(); ++i) {
        char ch = raw[i];
        if (ch == '"') { in_quotes = !in_quotes; continue; }
        if (ch == ';' && !in_quotes) {
            auto eq = current.find('=');
            if (eq != std::string::npos)
                tags[trim(current.substr(0, eq))] = trim(current.substr(eq + 1));
            current.clear();
            continue;
        }
        current += ch;
    }
    if (!current.empty()) {
        auto eq = current.find('=');
        if (eq != std::string::npos)
            tags[trim(current.substr(0, eq))] = trim(current.substr(eq + 1));
    }
    return tags;
}

std::unordered_map<std::string, std::string> parse_headers_map(const std::string& raw) {
    std::unordered_map<std::string, std::string> out;
    std::istringstream ss(raw);
    std::string line, cur_key, cur_val;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if (line[0] == ' ' || line[0] == '\t') {
            if (!cur_key.empty()) { cur_val += " "; cur_val += trim(line); }
        } else {
            if (!cur_key.empty()) out[to_lower(cur_key)] = cur_val;
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                cur_key = line.substr(0, colon);
                cur_val = trim(line.substr(colon + 1));
            }
        }
    }
    if (!cur_key.empty()) out[to_lower(cur_key)] = cur_val;
    return out;
}

std::vector<std::string> get_header_values(const std::string& raw, const std::string& name_lower) {
    std::vector<std::string> values;
    std::istringstream ss(raw);
    std::string line, cur_name, cur_val;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        if (line[0] == ' ' || line[0] == '\t') {
            if (!cur_name.empty()) cur_val += " " + trim(line);
        } else {
            if (to_lower(cur_name) == name_lower && !cur_val.empty()) values.push_back(cur_val);
            auto c = line.find(':');
            if (c != std::string::npos) {
                cur_name = line.substr(0, c);
                cur_val = trim(line.substr(c + 1));
            } else { cur_name.clear(); cur_val.clear(); }
        }
    }
    if (to_lower(cur_name) == name_lower && !cur_val.empty()) values.push_back(cur_val);
    return values;
}

// SPF CIDR 匹配
bool ip4_match(const std::string& client_ip, const std::string& network, const std::string& cidr_str) {
    if (cidr_str.empty()) return client_ip == network;
    int prefix = std::stoi(cidr_str);
    auto ip_to_u32 = [](const std::string& ip) -> uint32_t {
        uint32_t result = 0;
        std::stringstream ss(ip);
        std::string octet;
        int shift = 24;
        while (std::getline(ss, octet, '.') && shift >= 0) {
            result |= (static_cast<uint32_t>(std::stoi(octet)) << shift);
            shift -= 8;
        }
        return result;
    };
    uint32_t client = ip_to_u32(client_ip);
    uint32_t net = ip_to_u32(network);
    uint32_t mask = (prefix == 0) ? 0 : (~0u << (32 - prefix));
    return (client & mask) == (net & mask);
}

// SPF 记录解析
struct SpfMechanism {
    char qualifier = '+';
    std::string mechanism;
    std::string value;
    std::string cidr;
};

std::vector<SpfMechanism> parse_spf_record(const std::string& record) {
    std::vector<SpfMechanism> mechs;
    if (record.substr(0, 6) != "v=spf1") return mechs;
    std::string content = record.substr(6);
    std::vector<std::string> parts;
    split(content, ' ', parts);
    for (const auto& part : parts) {
        if (part.empty()) continue;
        SpfMechanism m;
        std::string token = part;
        char first = token[0];
        if (first == '+' || first == '-' || first == '~' || first == '?') {
            m.qualifier = first; token = token.substr(1);
        }
        auto colon = token.find(':');
        if (colon != std::string::npos) {
            m.mechanism = to_lower(token.substr(0, colon));
            std::string val = token.substr(colon + 1);
            auto slash = val.find('/');
            if (slash != std::string::npos) {
                m.value = val.substr(0, slash);
                m.cidr = val.substr(slash + 1);
            } else { m.value = val; }
        } else {
            auto eq = token.find('=');
            if (eq != std::string::npos) {
                m.mechanism = to_lower(token.substr(0, eq));
                m.value = token.substr(eq + 1);
            } else { m.mechanism = to_lower(token); }
        }
        mechs.push_back(std::move(m));
    }
    return mechs;
}

// DMARC 对齐
bool is_aligned(const std::string& auth_domain, const std::string& from_domain) {
    if (auth_domain.empty() || from_domain.empty()) return false;
    if (auth_domain == from_domain) return true;
    auto ends_with_dot = [](const std::string& s, const std::string& suffix) -> bool {
        if (s.size() <= suffix.size() + 1) return false;
        return s[s.size() - suffix.size() - 1] == '.' &&
               s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (auth_domain.size() > from_domain.size())
        return ends_with_dot(auth_domain, from_domain);
    return ends_with_dot(from_domain, auth_domain);
}

// 域名提取
std::string extract_domain(const std::string& addr) {
    auto at = addr.find('@');
    if (at == std::string::npos) return {};
    std::string domain = addr.substr(at + 1);
    if (!domain.empty() && domain.back() == '>') domain.pop_back();
    if (!domain.empty() && domain.front() == '<') domain = domain.substr(1);
    return to_lower(domain);
}

// ================================================================
// 测试套件
// ================================================================

int main() {
    std::cout << "=== parse_tags ===" << std::endl;

    // 基本 DKIM tag 解析
    TEST("basic tags");
    {
        auto t = parse_tags("v=1; a=rsa-sha256; d=qq.com; s=s201512");
        check("v", t["v"] == "1");
        check("a", t["a"] == "rsa-sha256");
        check("d", t["d"] == "qq.com");
        check("s", t["s"] == "s201512");
    }

    // 带空格的 tag
    TEST("tags with whitespace");
    {
        auto t = parse_tags("v=1;  a=rsa-sha256 ; d = qq.com");
        check("v", t["v"] == "1");
        check("a", t["a"] == "rsa-sha256");
        check("d", t["d"] == "qq.com");  // trim 去空格
    }

    // 最后 tag 无分号
    TEST("last tag without semicolon");
    {
        auto t = parse_tags("v=1; d=qq.com; b=base64sig");
        check("b", t["b"] == "base64sig");
    }

    // DKIM DNS 记录格式
    TEST("DKIM DNS record");
    {
        auto t = parse_tags("v=DKIM1; k=rsa; p=MIGfMA0GCSqGSIb3");
        check("v", t["v"] == "DKIM1");
        check("k", t["k"] == "rsa");
        check("p len", t["p"].size() == 16);
    }

    // c= 带斜杠（relaxed/relaxed）
    TEST("c= tag with slash");
    {
        auto t = parse_tags("c=relaxed/relaxed; d=qq.com");
        check("c", t["c"] == "relaxed/relaxed");
    }

    // DMARC 记录
    TEST("DMARC record");
    {
        auto t = parse_tags("v=DMARC1; p=reject; rua=mailto:dmarc@qq.com");
        check("v", t["v"] == "DMARC1");
        check("p", t["p"] == "reject");
    }

    std::cout << std::endl << "=== get_header_values ===" << std::endl;

    std::string sample_headers =
        "From: sender@test.com\r\n"
        "To: rcpt@test.com\r\n"
        "Subject: Hello\r\n"
        "X-Custom: value1\r\n"
        "X-Custom: value2\r\n"
        "\r\n";

    // 基本查找
    TEST("basic lookup");
    {
        auto vals = get_header_values(sample_headers, "from");
        check("found", vals.size() == 1);
        check("value", vals[0] == "sender@test.com");
    }

    // to_lower fix: caller must pass lowercase; the raw header can be any case
    TEST("to_lower at call site (regression test for fix)");
    {
        // These work — caller passed lowercase
        auto v1 = get_header_values(sample_headers, "from");
        check("from→found", v1.size() == 1);
        // Simulate the original bug: caller passed raw header name "From"
        // without to_lower(). This MUST fail because name_lower must be lowercase.
        auto v2 = get_header_values(sample_headers, "From");
        check("From→NOT found (API contract)", v2.empty());
        // The correct usage pattern (what the fix does):
        std::string hname = "From";
        auto v3 = get_header_values(sample_headers, to_lower(hname));
        check("to_lower(From)→found", v3.size() == 1);
    }

    // 折叠头（continuation lines）
    TEST("folded headers");
    {
        std::string folded =
            "DKIM-Signature: v=1; a=rsa-sha256;\r\n"
            "\td=qq.com; s=s201512;\r\n"
            "\tb=base64sig\r\n"
            "From: sender@test.com\r\n"
            "\r\n";
        auto vals = get_header_values(folded, "dkim-signature");
        check("found", vals.size() == 1);
        check("contains d=", vals[0].find("d=qq.com") != std::string::npos);
    }

    // 重复头
    TEST("duplicate headers");
    {
        auto vals = get_header_values(sample_headers, "x-custom");
        check("count", vals.size() == 2);
        check("first", vals[0] == "value1");
        check("second", vals[1] == "value2");
    }

    // 不存在的头
    TEST("missing header");
    {
        auto vals = get_header_values(sample_headers, "x-nonexistent");
        check("empty", vals.empty());
    }

    // 头值为空
    TEST("empty header value");
    {
        auto vals = get_header_values("Empty:\r\nFrom: x@y.com\r\n\r\n", "empty");
        check("empty", vals.empty());
    }

    std::cout << std::endl << "=== extract_domain ===" << std::endl;

    TEST("plain addr");
    check("domain", extract_domain("user@domain.com") == "domain.com");

    TEST("angle brackets");
    check("domain", extract_domain("<user@domain.com>") == "domain.com");

    TEST("empty");
    check("empty", extract_domain("no-at-sign").empty());

    TEST("uppercase");
    check("lowercased", extract_domain("U@QQ.COM") == "qq.com");

    std::cout << std::endl << "=== ip4_match ===" << std::endl;

    TEST("exact match");     check("match", ip4_match("1.2.3.4", "1.2.3.4", ""));
    TEST("exact mismatch");  check("no",    !ip4_match("1.2.3.4", "1.2.3.5", ""));
    TEST("CIDR /24 match");  check("match", ip4_match("1.2.3.4", "1.2.3.0", "24"));
    TEST("CIDR /24 no");     check("no",    !ip4_match("1.2.4.4", "1.2.3.0", "24"));
    TEST("CIDR /32");        check("match", ip4_match("1.2.3.4", "1.2.3.4", "32"));
    TEST("CIDR /0");         check("match", ip4_match("1.2.3.4", "0.0.0.0", "0"));

    std::cout << std::endl << "=== parse_spf_record ===" << std::endl;

    TEST("basic record");
    {
        auto mechs = parse_spf_record("v=spf1 ip4:1.2.3.4 -all");
        check("count", mechs.size() == 2);
        check("first mech", mechs[0].mechanism == "ip4");
        check("first val", mechs[0].value == "1.2.3.4");
        check("first qual", mechs[0].qualifier == '+');
        check("second mech", mechs[1].mechanism == "all");
        check("second qual", mechs[1].qualifier == '-');
    }

    TEST("qualifiers");
    {
        auto mechs = parse_spf_record("v=spf1 ~all ?all");
        check("softfail", mechs[0].qualifier == '~');
        check("neutral",  mechs[1].qualifier == '?');
    }

    TEST("CIDR in SPF");
    {
        auto mechs = parse_spf_record("v=spf1 ip4:10.0.0.0/8 -all");
        check("cidr", mechs[0].cidr == "8");
    }

    TEST("not SPF");
    {
        auto mechs = parse_spf_record("v=DKIM1; p=key");
        check("empty", mechs.empty());
    }

    std::cout << std::endl << "=== is_aligned (DMARC) ===" << std::endl;

    TEST("exact match");    check("match", is_aligned("qq.com", "qq.com"));
    TEST("mismatch");       check("no",    !is_aligned("qq.com", "gmail.com"));
    TEST("subdomain");      check("match", is_aligned("mail.qq.com", "qq.com"));
    TEST("reverse sub");    check("match", is_aligned("qq.com", "mail.qq.com"));
    TEST("empty auth");     check("no",    !is_aligned("", "qq.com"));
    TEST("empty from");     check("no",    !is_aligned("qq.com", ""));
    TEST("partial match");  check("no",    !is_aligned("qq.com", "xqq.com"));

    std::cout << std::endl << "=== body canonicalization ===" << std::endl;

    // simple: 删除尾部空行，每行加 CRLF，空 body 变成单个 CRLF
    TEST("simple: normal body");
    {
        std::string result = normalize_body_simple("Hello\r\nWorld\r\n");
        check("lines", result == "Hello\r\nWorld\r\n");
    }

    TEST("simple: trailing empty lines stripped");
    {
        std::string result = normalize_body_simple("Hello\r\n\r\n\r\n");
        check("stripped", result == "Hello\r\n");
    }

    TEST("simple: empty body");
    {
        std::string result = normalize_body_simple("");
        check("single CRLF", result == "\r\n");
    }

    // relaxed: 删除尾部空行，WSP 折叠，每行加 CRLF
    TEST("relaxed: wsp collapse");
    {
        std::string result = normalize_body_relaxed("Hello   World\r\n");
        check("collapsed", result == "Hello World\r\n");
    }

    TEST("relaxed: tab collapse");
    {
        std::string result = normalize_body_relaxed("Hello\t\tWorld\r\n");
        check("collapsed", result == "Hello World\r\n");
    }

    TEST("relaxed: trailing wsp stripped");
    {
        // "Hello   " → collapse WSP → "Hello " → no trailing WSP appended → "Hello"
        std::string result = normalize_body_relaxed("Hello   \r\n");
        check("just Hello+CRLF", result == "Hello\r\n");
    }

    TEST("relaxed: empty body → empty string");
    {
        // RFC 6376: empty body → zero lines → empty canonical form
        std::string result = normalize_body_relaxed("");
        check("empty", result.empty());
    }

    TEST("relaxed: blank line preserved");
    {
        std::string result = normalize_body_relaxed("\r\nHello\r\n");
        check("blank then Hello", result == "\r\nHello\r\n");
    }

    std::cout << std::endl << "=== header canonicalization (relaxed) ===" << std::endl;

    TEST("basic canonicalization");
    {
        std::string result = canonicalize_header_relaxed("From", "sender@test.com");
        check("lowercase name", result == "from:sender@test.com\r\n");
    }

    TEST("wsp collapse in value");
    {
        std::string result = canonicalize_header_relaxed("Subject", "Hello    World");
        check("collapsed", result == "subject:Hello World\r\n");
    }

    TEST("collapse_ws standalone");
    {
        check("single space", collapse_ws("a  b\tc") == "a b c");
        check("leading ws",   collapse_ws("  hello") == "hello");
        check("trailing ws",  collapse_ws("hello  ") == "hello");
        check("CRLF in ws",   collapse_ws("a\r\nb") == "a b");
    }

    std::cout << std::endl << "=== DKIM full verification (QQ mail fixture) ===" << std::endl;

    // ---- 真实 QQ 邮件 fixture（从生产环境抓取，已验证 dkim=pass） ----
    const std::string raw_headers =
        "DKIM-Signature: v=1; a=rsa-sha256; c=relaxed/relaxed; d=qq.com; s=s201512;\r\n"
        "\tt=1783353315; bh=SFMxKRJds/1H9Vt0wd2tUM7QaMLL1sR7/mbKAGx4Jy4=;\r\n"
        "\th=From:To:Subject:Date;\r\n"
        "\tb=uGVkjlFb0ckSVEqYgpUGCQIfM4UjVj8Q3wvytvUwku4Egcfz1kiT7aF03OIomBUIv\r\n"
        "\t SVmQRpZRmkRD6857P85DEcdLfhR2DQ1tq09Evvzx7x6SgrkMDAAxA2Jd213cP48Sp2\r\n"
        "\t Ao6gyOhmg3kqHF4Mbe4pdBUGBUigc6Jog3rLlYbs=\r\n"
        "X-QQ-XMRINFO: NS+P29fieYNwqS3WCnRCOn9D1NpZuCnCRA==\r\n"
        "X-QQ-XMAILINFO: Ns47RJroNOOsR9CRKAtL+lcHsndieNdLUGITG7PfjHl5Jrjig2ZVFpxmXG14mM\r\n"
        "\t+t/OqtgHx+ZIBIYUHssHoAgbfVFVwQjcdwB7/Zh937N3D4N/xtjYEYCJ7AhtakyBRSUGlXyZdd+4G\r\n"
        "\tqcWYDSA2IzznNWQU3vLPTquCFYUIEN3S1tHDHC+wREbY+alTTCBRfW4LbasbHEnbfhMrRr4Le2sIE\r\n"
        "From: \"=?utf-8?B?Oi0p6Zu377yIICfilr8gJyDvvInnpZ4=?=\" <2466245103@qq.com>\r\n"
        "To: \"=?utf-8?B?cXQ=?=\" <qt@scut.email>\r\n"
        "Subject: =?utf-8?B?5rWL6K+V?=\r\n"
        "Mime-Version: 1.0\r\n"
        "Content-Type: multipart/alternative;\r\n"
        "\tboundary=\"----=_NextPart_6A4BCFE2_D3382C80_59309AE2\"\r\n"
        "Content-Transfer-Encoding: 8Bit\r\n"
        "Date: Mon, 6 Jul 2026 23:55:14 +0800\r\n"
        "X-Priority: 3\r\n"
        "Message-ID: <tencent_91263FF4C1CF5048A8892E4FC6A42AD44A05@qq.com>\r\n"
        "X-QQ-MIME: TCMime 1.0 by Tencent\r\n"
        "X-Mailer: QQMail 2.x\r\n"
        "X-QQ-Mailer: QQMail 2.x\r\n"
        "X-QQ-mid: xmapzb43-0t1783353314t1qfage4y\r\n"
        "\r\n";

    const std::string raw_body =
        "This is a multi-part message in MIME format.\r\n"
        "\r\n"
        "------=_NextPart_6A4BCFE2_D3382C80_59309AE2\r\n"
        "Content-Type: text/plain;\r\n"
        "\tcharset=\"utf-8\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "UVENCg0KDQoNCuWPkeiHquaIkeeahGlQaG9uZQ==\r\n"
        "\r\n"
        "------=_NextPart_6A4BCFE2_D3382C80_59309AE2\r\n"
        "Content-Type: text/html;\r\n"
        "\tcharset=\"utf-8\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "PGRpdiBzdHlsZT0ibWluLWhlaWdodDoyMnB4O21hcmdpbi1ib3R0b206OHB4OyI+UVE8L2Rp\r\n"
        "dj48ZGl2IHN0eWxlPSJtaW4taGVpZ2h0OjIycHg7bWFyZ2luLWJvdHRvbTo4cHg7Ij48YnIg\r\n"
        "IC8+PC9kaXY+PGRpdiBpZD0iUVFNYWlsU2lnbmF0dXJlIiBjbGFzcz0ibWFpbC1mb290ZXIi\r\n"
        "IGFyaWEtaGlkZGVuPSJ0cnVlIj48aHIgc3R5bGU9Im1hcmdpbjogMCAwIDEwcHggMDtib3Jk\r\n"
        "ZXI6IDA7Ym9yZGVyLWJvdHRvbToxcHggc29saWQgI0U2RThFQjtoZWlnaHQ6MDtsaW5lLWhl\r\n"
        "aWdodDowO2ZvbnQtc2l6ZTowO3BhZGRpbmc6IDIwcHggMCAwIDA7d2lkdGg6IDUwcHg7IiAg\r\n"
        "Lz7lj5Hoh6rmiJHnmoRpUGhvbmU8L2Rpdj48ZGl2IGlkPSJvcmlnaW5hbC1jb250ZW50Ij48\r\n"
        "L2Rpdj4=\r\n"
        "\r\n"
        "------=_NextPart_6A4BCFE2_D3382C80_59309AE2--";

    std::string pubkey_b64 =
        "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDPsFIOSteMStsN615gUWK2RpNJ"
        "/B/ekmm4jVlu2fNzXADFkjF8mCMgh0uYe8w46FVqxUS97habZq6P5jmCj/WvtPGZ"
        "AX49jmdaB38hzZ5cUmwYZkdue6dM17sWocPZO8e7HVdq7bQwfGuUjVuMKfeTB3i"
        "Neo6/hFhb9TmUgnwjpQIDAQAB";

    // ---- 步骤1: 解析 DKIM 签名头 ----
    TEST("parse DKIM-Signature header");
    auto dkim_vals = get_header_values(raw_headers, "dkim-signature");
    check("found", dkim_vals.size() == 1);

    auto tags = parse_tags(dkim_vals[0]);
    check("v=1", tags["v"] == "1");
    check("a=rsa-sha256", tags["a"] == "rsa-sha256");
    check("d=qq.com", to_lower(tags["d"]) == "qq.com");
    check("s=s201512", tags["s"] == "s201512");

    std::string body_hash = tags["bh"];
    std::string signature_b64 = tags["b"];
    std::string body_canon = "relaxed";
    {
        auto slash = tags["c"].find('/');
        if (slash != std::string::npos) body_canon = to_lower(trim(tags["c"].substr(slash + 1)));
    }
    check("c= parsed", body_canon == "relaxed");

    std::vector<std::string> signed_headers;
    split(tags["h"], ':', signed_headers);
    check("h= count", signed_headers.size() == 4);

    // ---- 步骤2: Body hash ----
    TEST("body hash verification");
    std::string canon_body = normalize_body_relaxed(raw_body);
    std::string computed_bh = sha256_base64(canon_body);
    check("bh match", computed_bh == body_hash);

    // ---- 步骤3: 头查找（含 to_lower 修复验证） ----
    TEST("signed header lookup");
    bool all_found = true;
    for (const auto& h : signed_headers) {
        auto vals = get_header_values(raw_headers, to_lower(h));
        if (vals.empty()) { all_found = false; break; }
    }
    check("all found", all_found);

    // ---- 步骤4: 构建签名输入 ----
    TEST("signing input construction");
    std::string signing_input;
    for (const auto& hname : signed_headers) {
        auto vals = get_header_values(raw_headers, to_lower(hname));
        signing_input += canonicalize_header_relaxed(hname, vals[0]);
    }

    // DKIM-Signature with b= emptied
    std::string dkim_sig = dkim_vals[0];
    auto bpos = dkim_sig.find("b=");
    auto endp = dkim_sig.find(';', bpos);
    dkim_sig = dkim_sig.substr(0, bpos + 2) + (endp != std::string::npos ? dkim_sig.substr(endp) : "");

    std::string dkim_canon = canonicalize_header_relaxed("DKIM-Signature", dkim_sig);
    // FIX: no trailing CRLF on last header
    if (dkim_canon.size() >= 2 && dkim_canon.substr(dkim_canon.size() - 2) == "\r\n")
        dkim_canon.resize(dkim_canon.size() - 2);
    signing_input += dkim_canon;
    check("len>0", signing_input.size() > 100);

    // ---- 步骤5: 解码公钥 ----
    TEST("public key decode");
    std::string clean_key;
    for (char c : pubkey_b64)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') clean_key += c;
    int pad = (4 - (clean_key.size() % 4)) % 4; clean_key.append(pad, '=');

    std::vector<unsigned char> kd(clean_key.size());
    int kl = EVP_DecodeBlock(kd.data(), (const unsigned char*)clean_key.data(),
                              static_cast<int>(clean_key.size()));
    if (clean_key.back() == '=') kl--;
    if (clean_key.size() > 1 && clean_key[clean_key.size()-2] == '=') kl--;
    kd.resize(static_cast<size_t>(kl));

    const unsigned char* kp = kd.data();
    EVP_PKEY* pkey = d2i_PUBKEY(nullptr, &kp, static_cast<long>(kl));
    check("d2i_PUBKEY", pkey != nullptr);

    // ---- 步骤6: 解码签名 ----
    TEST("signature decode");
    std::string clean_sig;
    for (char c : signature_b64)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') clean_sig += c;
    pad = (4 - (clean_sig.size() % 4)) % 4; clean_sig.append(pad, '=');

    std::vector<unsigned char> sd(clean_sig.size());
    int sl = EVP_DecodeBlock(sd.data(), (const unsigned char*)clean_sig.data(),
                              static_cast<int>(clean_sig.size()));
    if (clean_sig.back() == '=') sl--;
    if (clean_sig.size() > 1 && clean_sig[clean_sig.size()-2] == '=') sl--;
    check("decoded len=128", sl == 128);
    sd.resize(static_cast<size_t>(sl));

    // ---- 步骤7: 验证 ----
    TEST("RSA-SHA256 verify");
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    int verify_ok = 0;
    if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) > 0 &&
        EVP_DigestVerifyUpdate(ctx, signing_input.data(), signing_input.size()) > 0) {
        verify_ok = EVP_DigestVerifyFinal(ctx, sd.data(), static_cast<size_t>(sl));
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    check("DKIM PASS", verify_ok == 1);

    // ---- 步骤8: 测试错误的签名应该 FAIL ----
    TEST("tampered body → DKIM FAIL");
    {
        std::string bad_body = raw_body + "tampered";
        std::string bad_canon = normalize_body_relaxed(bad_body);
        std::string bad_bh = sha256_base64(bad_canon);
        check("bh changed", bad_bh != body_hash);
    }

    // ============================================================
    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  PASS: " << g_pass << "  FAIL: " << g_fail << std::endl;
    std::cout << "========================================" << std::endl;
    return g_fail > 0 ? 1 : 0;
}
