# ProtoRelay Webmail

本仓库自建的 React / TypeScript / Vite 前端，使用 Radix Dialog、Tiptap 编辑器与
Lucide 图标。生产构建由现有 C++ `webmailServer` 同源提供，浏览器只访问 HTTP API。

## 运行

需要 Node.js >=22.12、npm，以及已配置数据库的 `webmailServer`。
在仓库根目录执行：

```bash
npm ci --prefix webmail
npm run --prefix webmail build
./build-release/webmailServer config/webmailConfig.json 8081 ./build-webmail
```

打开 `http://127.0.0.1:8081`，使用现有邮箱账号登录。`webmailServer` 默认静态目录
已改为 `./build-webmail`；`--help` 查看参数。数据库迁移和节点配置见
[API 契约](../docs/mail-system/architecture/webmail-api.md)。静态目录只放构建产物。

开发时先启动 API，再运行：

```bash
WEBMAIL_API_URL=http://127.0.0.1:8081 npm run --prefix webmail dev
```

Vite 将 `/api` 代理到上述地址；从 Vite 打印的地址访问页面。开发模式移除顶层
HTML CSP，以允许 Vite 注入的刷新脚本和 WebSocket；生产构建保留 CSP。
邮件正文 iframe 始终带独立 CSP 和空 sandbox，不受此开发设置影响。
`npm run --prefix webmail preview` 只预览静态产物，不配置 API 代理，收发联调使用
C++ 服务或 Vite 开发代理。

## 已实现

- 登录、退出和新登录踢掉旧会话；全局 401 提示重新登录。
- 桌面三栏邮箱，窄屏列表/阅读切换，手机侧栏导航。
- 实际文件夹、分页、当前页关键词筛选、刷新、加载/空/错误状态。
- MIME 文本/HTML 阅读；服务端清洗后仍放入无脚本、无 same-origin 的 sandbox iframe；
  远程图片禁用，长 HTML 正文可在隔离阅读区内滚动。
- 自动标记已读、标未读、标星；鉴权后下载附件，不通过前端 Blob 整体缓存。
- 富文本写信，To/Cc、主题、文本/HTML，成功后刷新邮箱与已发送副本。
- 超时/断网不自动重发，必须先核实结果再手动解锁；在途提交遇到会话失效也保留防重发状态。

草稿仅存于当前页面的内存：收起写信、同账号会话失效后重新登录可继续；刷新页面会丢失，
明确退出或切换账号会清空。不使用 localStorage 保存邮件、密码或 token。
所有资源 ID 使用十进制字符串，禁止转 JavaScript Number。

## 当前边界与扩展点

尚未提供附件上传、密送、服务器草稿保存、删除/移动/文件夹管理、全库搜索、邮件线程、
回复引用/转发及联系人。列表筛选只作用于当前页；文件夹中已有的历史草稿可以阅读，
写信内容不会自动存入草稿箱。TLS/Secure cookie 感知装配继续按后端计划推进。
下载目前支持 local/distributed provider；S3/HDFS 未实现有界下载时返回 503，
不会退回整对象入内存。

`src/api.ts` 是类型和请求入口，`App.tsx` 处理会话/邮箱/阅读，`Compose.tsx` 按需加载
编辑器，`style.css` 控制响应式布局。服务端业务契约集中在 `/api`，可独立替换 UI 组件。

## 验证

```bash
npm run --prefix webmail build
npm exec --prefix webmail -- playwright install chromium
.venv/bin/python test/scripts/webmail_ui_fixture.py --db-config config/db_config.json
```

Python fixture 仅允许本地 DB，启动真实 C++ HTTP 服务、创建随机测试账号、24 封 MIME
邮件及二进制附件，再运行 Playwright；最终清理自身账号、邮件、临时文件和进程。
浏览器真实走登录/读信/标记/下载/写信/已发送/互踢，只有超时、断网和 503 场景注入故障。
截图保存在 `artifacts/webmail-preview/`，失败 trace 在 `artifacts/webmail-tests/`。
手动检查可加 `--serve --state /tmp/webmail-ui-state.json`，测试凭据仅保存在权限 0600
的临时状态文件，Ctrl+C 自动清理。不要将该状态文件用于生产环境。
