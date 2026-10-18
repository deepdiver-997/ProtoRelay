# HTTP 请求分片后停止续读

2026-10-18，webmail 写信的真实 HTTP→SMTP 验证发现：客户端将请求头和 JSON
正文分两次发送时，POST 停在 WAIT_BODY，最终由 watchdog 关闭；经本机代理访问时
表现为 502。一次读入完整请求的 curl 探针与直接调用 ApiRouter 的单测不触发。

`SessionBase::do_async_read` 完成回调只消费已经到达的完整帧，后续读由协议层
负责。SMTP 的命令 handler 会明确发起下一次读；HTTP 只切换解析状态，没有在
残缺请求行、请求头、定长正文或 chunked 帧上安排下一次读取。

为 SessionBase 增加默认关闭的 `auto_continue_read` 协议策略。HTTP 在收集请求的
三个状态启用，基座在本轮输入消费结束后检查 pause/closed 再续读。完整 API 请求
进入 RESPOND/pause 后不续读，由响应完成的原有 keep-alive 管线恢复。SMTP/IMAP/
POP3 默认保持原来的 handler 驱动规则。

专项回归 `test/unit/http_session_read_test.cpp` 通过 MockConnection 的 pending read
逐片投递请求，覆盖残缺请求行、请求头、定长正文、chunked size/data/trailer 与
异步 API pause；断言消费者只收到一次完整正文，不直接调用解析函数绕过传输层。

验证命令：

```bash
cmake --build build-release --target http_session_read_test -j 8
ctest --test-dir build-release -R http_session_read_test --output-on-failure
```

双向验证：保留测试、撤去续读修复时，在第一个残缺请求行后没有 pending read，
专项测试必须失败；恢复修复后，定长与 chunked 两条场景均通过。
