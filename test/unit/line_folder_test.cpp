// LineFolder 单元测试 —— 入站正文清洗：RFC 5321 §4.5.2 去点填充 + 超长行
// RFC 5322 折叠（纯函数，零 I/O）
//
// 剧情：SMTP 入站正文落盘前，'.' 起始的行剥掉一个前导点（还原发件方
// stuffing 前的字节——2026-09-25 入站 DKIM bh mismatch 事故根因）；
// 单行超过 2048 字节的行被折叠（CRLF + 空格），防止 BODY[HEADER]/POP3
// RETR 原始内容路径噎死弱缓冲客户端；无点行、非超长行逐字直通。

#undef NDEBUG
#include <cassert>
#include <iostream>
#include <string>

#include "mail_system/back/algorithm/line_folder.h"

using mail_system::algorithm::LineFolder;

namespace {

std::string repeat(const std::string& unit, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += unit;
    return s;
}

// RFC unfold：删除折叠序列 "\r\n "（我们插入的折叠标记）
std::string unfold(const std::string& s) {
    std::string out = s;
    std::string fold = "\r\n ";
    size_t pos;
    while ((pos = out.find(fold)) != std::string::npos) out.erase(pos, fold.size());
    return out;
}

// 校验输出每一行内容（不含 CRLF）都不超限
void check_line_lengths(const char* name, const std::string& folded) {
    size_t start = 0;
    while (start < folded.size()) {
        size_t nl = folded.find("\r\n", start);
        size_t end = (nl == std::string::npos) ? folded.size() : nl;
        size_t len = end - start;
        assert(len <= LineFolder::kMaxStorageLineBytes);
        (void)len;
        if (nl == std::string::npos) break;
        start = nl + 2;
    }
    (void)name;
}

} // namespace

int main() {
    std::cout << "Running LineFolder tests...\n";

    // 1. 短行直通：无任何超长行时字节逐字一致
    {
        LineFolder f;
        std::string in = "Subject: hello\r\nFrom: a@b.c\r\n\r\nbody line\r\n";
        std::string out = f.feed(in) + f.flush();
        assert(out == in);
        std::cout << "  [PASS] passthrough_short_lines\n";
    }

    // 2. 超长 ASCII 行被折叠，unfold 后可完整还原
    {
        LineFolder f;
        std::string line = repeat("A", 6000);
        std::string out = f.feed(line + "\r\n") + f.flush();
        assert(out.find("\r\n ") != std::string::npos);   // 确实发生了折叠
        assert(unfold(out) == line + "\r\n");
        check_line_lengths("long_ascii", out);
        std::cout << "  [PASS] fold_long_ascii_unfold_roundtrip\n";
    }

    // 3. 超长 UTF-8 行：折叠点不切多字节字符，每行都是合法 UTF-8
    {
        LineFolder f;
        std::string line = repeat("\xe4\xb8\xad", 800);   // 2400 字节
        std::string out = f.feed(line + "\r\n") + f.flush();
        check_line_lengths("long_utf8", out);
        assert(unfold(out) == line + "\r\n");
        // 每行起始字节都不是 UTF-8 续字节（10xxxxxx）
        size_t start = 0;
        while (start < out.size()) {
            assert((static_cast<unsigned char>(out[start]) & 0xC0) != 0x80);
            size_t nl = out.find("\r\n", start);
            if (nl == std::string::npos) break;
            start = nl + 2;
        }
        std::cout << "  [PASS] fold_long_utf8_char_boundary\n";
    }

    // 4. 跨块携带半行：分多次 feed 与一次 feed 结果一致
    {
        std::string line = repeat("B", 5000);
        LineFolder whole;
        std::string expect = whole.feed(line + "\r\nmore\r\n") + whole.flush();

        LineFolder piecewise;
        std::string got;
        got += piecewise.feed(line.substr(0, 100));
        got += piecewise.feed(line.substr(100, 2900));   // 切在行中间
        got += piecewise.feed(line.substr(3000) + "\r\n");
        got += piecewise.feed("mo");
        got += piecewise.feed("re\r\n");
        got += piecewise.flush();
        assert(got == expect);
        std::cout << "  [PASS] chunk_boundary_carry\n";
    }

    // 5. flush：无换行结尾的半行在 flush 时被折叠冲出
    {
        LineFolder f;
        std::string out = f.feed(repeat("C", 5000));      // 无 CRLF
        assert(out.empty());                               // 半行被携带
        out += f.flush();
        assert(!out.empty());
        check_line_lengths("flush_partial", out);
        assert(unfold(out) == repeat("C", 5000));
        std::cout << "  [PASS] flush_carried_partial\n";
    }

    // 6. 多行混合：一封"邮件"只有超长头被折叠 + stuffed 行被还原
    {
        LineFolder f;
        std::string long_subject = "Subject: " + repeat("x", 5000);
        std::string in = "From: a@b.c\r\n" + long_subject + "\r\nTo: t@x.y\r\n\r\n" + repeat("z", 100) + "\r\n..stuffed\r\n";
        std::string out = f.feed(in) + f.flush();
        assert(out.substr(0, 13) == "From: a@b.c\r\n");
        assert(out.find("\r\n ") != std::string::npos);
        assert(out.find("\r\n.stuffed\r\n") != std::string::npos);  // "..stuffed" 还原为 ".stuffed"
        check_line_lengths("mixed_mail", out);
        std::cout << "  [PASS] mixed_mail_fold_and_unstuff\n";
    }

    // 7. reset：丢弃携带的半行（对应 reset_mail_state 丢弃半途邮件）
    {
        LineFolder f;
        f.feed(repeat("D", 100));
        f.reset();
        assert(f.flush().empty());
        std::cout << "  [PASS] reset_discards_carry\n";
    }

    // 8. 去点填充基础：'.' 起始行剥一个前导点，普通行不动
    {
        LineFolder f;
        std::string out = f.feed("a\r\n..b\r\n.c\r\nd\r\n") + f.flush();
        assert(out == "a\r\n.b\r\nc\r\nd\r\n");
        std::cout << "  [PASS] unstuff_basic\n";
    }

    // 9. 去点填充跨块安全：stuffed 行 "..y" 恰好被 TCP 分块切成 ".." + "y\r\n"
    //    （这是 2026-09-25 旧非流式实现的跨块 bug 场景）
    {
        LineFolder f;
        std::string out = f.feed("x\r\n..") + f.feed("y\r\nz\r\n") + f.flush();
        assert(out == "x\r\n.y\r\nz\r\n");
        std::cout << "  [PASS] unstuff_chunk_boundary\n";
    }

    // 10. flush 路径同样去填充：结尾半行以 '.' 起始
    {
        LineFolder f;
        std::string out = f.feed("...tail");   // 无 CRLF，半行被携带
        assert(out.empty());
        out += f.flush();
        assert(out == "..tail");
        std::cout << "  [PASS] unstuff_flush_partial\n";
    }

    // 11. 单点行（stuffed 空行）还原为空行
    {
        LineFolder f;
        std::string out = f.feed("a\r\n.\r\nb\r\n") + f.flush();
        assert(out == "a\r\n\r\nb\r\n");
        std::cout << "  [PASS] unstuff_single_dot_line\n";
    }

    std::cout << "All LineFolder tests passed!\n";
    return 0;
}
