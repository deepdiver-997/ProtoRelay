# Webmail 浏览器接入契约回归

2026-10-18，实现 SPA 时从浏览器消费端发现以下问题，均保留专项回归与修复前负验证。

## 雪花 ID 丢失精度

API 把资源 ID 序列化成 JSON number。`9007199254740993` 经过 JavaScript JSON.parse
会变成 `9007199254740992`，随后列表打开邮件会请求另一个 ID。统一 JSON 出口递归将
资源 `id` 转成十进制字符串；状态、计数、时间不变。前端类型 Id=string，不转 Number。

`webmail_test::test_resource_ids_are_lossless` 验证大整数精确往返并检查 status 仍为数字。
保留新增测试、撤去字符串转换时，is_string 断言失败（负验证日志 `/tmp/webmail-id-before.log`）；
恢复后通过。真实浏览器 fixture 使用大于 2^53 的邮件 ID，成功打开、修改与下载附件。

## 详情丢失未读状态

读信元数据查询未取收件人状态，MessageDetail.status 一直为默认 0。前端据此跳过已读
PATCH，实际收件箱仍未读。查询按当前用户收件行获取状态并映射到详情，Sent 强制为 0；
打开详情的 GET 本身仍无写副作用，前端显式标记已读。

`webmail_test --unread-detail-regression` 经 MockDb + 实际本地 MIME 文件走 API 完整链，
给未读行后断言详情 status=1。修复前在此断言 FAIL（`/tmp/webmail-status-before.log`），
修复后 PASS。真实 DB/浏览器还验证本人的已读不会改变抄送收件人的状态，读取 Sent
不会把自发自收的 Inbox 标为已读。

## HEAD 错误响应带正文

新增附件 HEAD 接口后，成功路径无正文，但鉴权/存储失败的 JSON 响应仍会经通用
send_response 写出正文。HTTP 客户端不消费 HEAD 正文，会破坏持久连接上的后续响应边界。
HttpSession 在 HEAD 下只写完整响应头，保留 GET 对应 Content-Length，禁止写正文/source。

`http_session_read_test::bounded_download(true)` 通过真实会话 FSM/MockConnection 读取 wire，
刻意给 HEAD response 放入非空 fallback body，验证头后没有字节、存储 read 未发生。
修复前 `wire.size()==end+4` FAIL（`/tmp/webmail-head-before.log`），恢复后 PASS。
同一测试还验证 >3 个 64 KiB 块的 NUL/0xff 二进制完全一致，第二块读取失败时关闭连接，
不会发送下一块或把不完整长度伪装成完整成功。

## 会话失效丢失在途提交的防重发状态

写信窗口的 sending 状态只存在组件内。在 POST 尚未响应时，另一个登录撤销会话，
窗口被卸载；重新登录后会重新启用发送，可能重复提交。旧请求的迟到成功回调也可能
清空新窗口草稿。现在发起请求前把待确认标记存入 App 的内存草稿，卸载后不执行旧表单
回调；明确成功清空草稿，明确失败解除标记，未知结果需核实后手动解锁。

Playwright `session loss during an in-flight submission` 暂停 send 响应，使用另一个真实
HTTP 会话登录使旧 cookie 失效，触发页面 focus 刷新，重新登录后检查按钮仍禁用，再
交付迟到的 202，确认当前草稿未被清空。旧构建下按钮 enabled，负验证 FAIL
（`/tmp/webmail-inflight-before.log`）；恢复后 PASS（`/tmp/webmail-inflight-after.log`）。
超时和网络断开另有场景，确认没有自动重发，关闭/重开写信仍保留限制。

## 编辑器格式状态未订阅事务

Tiptap 的 useEditor 默认不因每个事务重渲染。工具栏直接调用 isActive 的结果停留在
上次 React render，空文档点加粗后按钮仍 aria-pressed=false。使用 useEditorState 订阅
加粗/斜体/列表/引用状态。真实浏览器在输入前先点加粗，断言 pressed=true，再发送正文，
读回确认 HTML 包含 strong。旧构建在 pressed 断言 FAIL（`/tmp/webmail-bold-before.log`），
修复后通过，验证实际格式也到达收件人。

## 验证命令

```bash
cmake --build build-release --target webmail_test http_session_read_test -j8
ctest --test-dir build-release -R '^(webmail_test|http_session_read_test)$' --output-on-failure
npm run --prefix webmail build
.venv/bin/python test/scripts/webmail_ui_fixture.py --db-config config/db_config.json
```

fixture 在本地独立创建账号/邮件/附件，真实收发链不发往外部地址；最终清理自身数据。
