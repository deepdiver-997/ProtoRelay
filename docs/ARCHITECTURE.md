# Mail System V6 - 架构设计文档

## 1. 项目概述

Mail System V6 是一个基于 C++20 的现代化 SMTPS (SMTP over SSL/TLS) 邮件服务器项目，采用异步网络编程架构，提供高性能、可扩展的邮件收发服务。

### 1.1 技术栈

| 技术 | 版本 | 用途 |
|------|------|------|
| C++ 标准 | C++20 | 现代 C++ 特性支持 |
| 构建系统 | CMake 3.10+ | 跨平台构建 |
| 网络库 | Boost.Asio (system, thread) | 异步 I/O |
| SSL/TLS | OpenSSL | 加密通信 |
| 日志系统 | spdlog | 模块化日志 |
| 数据库 | MySQL (mysqlclient) | 数据持久化 |
| JSON 解析 | nlohmann/json (内嵌) | 配置文件解析 |

### 1.2 核心特性

- ✅ 完全异步的非阻塞 I/O 架构
- ✅ SSL/TLS 加密通信支持
- ✅ 模块化日志系统（10 个独立模块）
- ✅ 连接池管理（数据库、网络连接）
- ✅ 状态机驱动的 SMTP 协议实现
- ✅ MIME 多部分邮件解析和附件处理
- ✅ 类型擦除设计（隐藏底层 socket 细节）
- ✅ 内存安全（完全避免 shared_ptr 循环引用）

---

## 2. 架构分层设计

项目采用清晰的分层架构，每一层职责明确，便于维护和扩展：

```
┌─────────────────────────────────────────────────────────────┐
│                     应用层 (Application)                     │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │ smtps_test   │  │  测试工具     │  │  其他应用     │    │
│  └──────────────┘  └──────────────┘  └──────────────┘    │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                    业务逻辑层 (Business)                     │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │ SMTPS 服务器 │  │  状态机 (FSM) │  │  Session管理  │    │
│  └──────────────┘  └──────────────┘  └──────────────┘    │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                    数据访问层 (Data Access)                   │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │ 数据库连接池 │  │  MySQL 服务   │  │  实体模型     │    │
│  └──────────────┘  └──────────────┘  └──────────────┘    │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                    基础设施层 (Infrastructure)                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │ IO 线程池     │  │ 工作线程池    │  │  连接抽象     │    │
│  └──────────────┘  └──────────────┘  └──────────────┘    │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                    网络通信层 (Network)                       │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │ TCP 连接      │  │ SSL 连接      │  │  IO 上下文    │    │
│  └──────────────┘  └──────────────┘  └──────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. 核心模块详解

### 3.1 连接层 (Connection Layer)

#### 设计目标

使用**类型擦除**技术隐藏底层 socket 细节，为上层提供统一的异步 I/O 接口。

#### 接口定义

```cpp
class IConnection {
public:
    using ReadHandler = std::function<void(const boost::system::error_code&, std::size_t)>;
    using WriteHandler = std::function<void(const boost::system::error_code&, std::size_t)>;
    using HandshakeHandler = std::function<void(const boost::system::error_code&)>;

    virtual ~IConnection() = default;

    // 异步读取数据到 buffer
    virtual void async_read(
        boost::asio::mutable_buffer buffer,
        ReadHandler handler
    ) = 0;

    // 异步写入数据
    virtual void async_write(
        boost::asio::const_buffer buffer,
        WriteHandler handler
    ) = 0;

    // SSL/TLS 握手
    virtual void async_handshake(
        boost::asio::ssl::stream_base::handshake_type type,
        HandshakeHandler handler
    ) = 0;

    // 关闭连接
    virtual void close() = 0;

    // 检查连接是否打开
    virtual bool is_open() const = 0;

    // 获取本地端口
    virtual uint16_t get_local_port() const = 0;

    // 获取远程 IP
    virtual std::string get_remote_ip() const = 0;

    // 释放底层 socket
    virtual std::unique_ptr<tcp::socket> release_socket() = 0;
};
```

#### 实现类

##### 3.1.1 TcpConnection

**文件**: `include/mail_system/back/mailServer/connection/tcp_connection.h`

**特点**:
- 包装原始 TCP socket
- 握手操作直接调用成功回调（模拟 SSL 行为）
- 无额外加密开销

**关键实现**:
```cpp
class TcpConnection : public IConnection {
public:
    explicit TcpConnection(std::unique_ptr<tcp::socket> socket);

    void async_read(buffer, handler) override;
    void async_write(buffer, handler) override;
    void async_handshake(type, handler) override {
        // TCP 无需握手，直接调用成功回调
        handler(boost::system::error_code());
    }
    // ...

private:
    std::unique_ptr<tcp::socket> socket_;
};
```

##### 3.1.2 SslConnection

**文件**: `include/mail_system/back/mailServer/connection/ssl_connection.h`

**特点**:
- 包装 SSL stream
- 执行 SSL/TLS 握手
- 提供加密通信

**关键实现**:
```cpp
class SslConnection : public IConnection {
public:
    SslConnection(
        std::unique_ptr<tcp::socket> socket,
        ssl::context& ssl_context
    );

    void async_read(buffer, handler) override {
        stream_->async_read_some(buffer, handler);
    }

    void async_write(buffer, handler) override {
        boost::asio::async_write(*stream_, buffer, handler);
    }

    void async_handshake(type, handler) override {
        stream_->async_handshake(type, handler);
    }

    // ...

private:
    std::unique_ptr<ssl::stream<tcp::socket>> stream_;
};
```

---

### 3.2 会话层 (Session Layer)

#### 设计目标

管理单个客户端连接的 SMTP 协议交互，使用**unique_ptr + 静态函数传递**避免 shared_ptr 循环引用。

#### 核心特性

1. **完全避免 shared_ptr**: 使用 `unique_ptr` 明确所有权
2. **回调可为空**: 回调为空时自动调用 `process_read()`
3. **移动语义**: Session 对象在异步回调中通过移动传递

#### SessionBase 基类模板

**文件**: `include/mail_system/back/mailServer/session/session_base.h`

**关键接口**:

```cpp
template <typename ConnectionType>
class SessionBase {
public:
    // 构造函数
    SessionBase(
        std::unique_ptr<ConnectionType> connection,
        ServerBase* server
    );

    // 异步读取 (回调可选)
    static void do_async_read(
        std::unique_ptr<SessionBase<ConnectionType>> self,
        ReadCallback callback = nullptr  // 可选回调
    );

    // 异步写入 (回调可选)
    static void do_async_write(
        std::unique_ptr<SessionBase<ConnectionType>> self,
        const std::string& data,
        WriteCallback callback = nullptr  // 可选回调
    );

    // SSL/TLS 握手
    template <typename HandshakeHandler>
    static void do_handshake(
        std::unique_ptr<SessionBase<ConnectionType>> self,
        ssl::stream_base::handshake_type type,
        HandshakeHandler&& handler
    );

    // 纯虚函数（派生类实现）
    virtual void handle_read(const std::string& data) = 0;
    virtual void process_read(
        std::unique_ptr<SessionBase<ConnectionType>> self
    ) = 0;

    // 状态机相关接口
    virtual void set_current_state(int state) = 0;
    virtual void set_next_event(int event) = 0;
    virtual void* get_fsm() const = 0;
    virtual int get_current_state() const = 0;

protected:
    std::unique_ptr<ConnectionType> connection_;
    std::vector<char> read_buffer_;
    std::unique_ptr<mail> mail_;
    std::vector<std::unique_ptr<mail>> mails_;
    ServerBase* m_server;
};
```

#### make_copyable 辅助函数

**作用**: 将只可移动的 lambda 包装为可复制的，用于 Boost.Asio 回调。

```cpp
template <typename F>
auto make_copyable(F&& f) {
    auto s = std::make_shared<std::decay_t<F>>(std::forward<F>(f));
    return [s](auto&&... args) {
        return (*s)(std::forward<decltype(args)>(args)...);
    };
}
```

#### SmtpsSession 实现

**文件**: `include/mail_system/back/mailServer/session/smtps_session.h`

**核心功能**:
1. SMTP 命令解析（EHLO, AUTH, MAIL FROM, RCPT TO, DATA, QUIT）
2. MIME 多部分邮件处理
3. 附件流式写入磁盘
4. Base64 编码解码

**关键方法**:

```cpp
template <typename ConnectionType>
class SmtpsSession : public SessionBase<ConnectionType> {
public:
    // 启动会话
    static void start(std::unique_ptr<SmtpsSession> self);

    // 处理读取的数据
    void handle_read(const std::string& data) override;

    // 处理读取后的逻辑
    void process_read(
        std::unique_ptr<SessionBase<ConnectionType>> self
    ) override;

private:
    // SMTP 命令解析
    void parse_smtp_command(const std::string& data);

    // 邮件数据处理（MIME 解析）
    void process_message_data(const std::string& data);

    // 处理多部分行
    void handle_multipart_line(const std::string& line);

    // Base64 解码
    static std::string decode_base64_block(const std::string& input);

    // 打开附件流
    void open_attachment_stream(const std::string& filename);

    // 写入附件数据
    void write_attachment_body_line(const std::string& line);

private:
    std::shared_ptr<SmtpsFsm<ConnectionType>> fsm_;
    int current_state_;
    int next_event_;
    SmtpsContext context_;
    std::string last_command_args_;
};
```

**MIME 解析流程**:

```
收到 DATA 命令
  ↓
状态: WAIT_DATA → IN_MESSAGE
  ↓
发送 "354 End data with <CR><LF>.<CR><LF>"
  ↓
读取邮件内容（流式处理）
  ↓
解析邮件头 (header_buffer)
  ↓
检测 Content-Type: multipart
  ↓
提取 boundary 标记
  ↓
逐行解析 body:
  ├─ 检测 boundary → 分段处理
  ├─ 解析 part headers → 判断是否为附件
  ├─ 文本 part → 累积到 text_body_buffer
  └─ 附件 part → 流式写入磁盘（base64 解码）
  ↓
检测 "\r\n.\r\n" → DATA_END 事件
```

---

### 3.3 状态机层 (FSM Layer)

#### 设计目标

使用状态机模式管理 SMTP 协议的状态转换，保证协议流程的正确性。

#### SMTPS 状态定义

**文件**: `include/mail_system/back/mailServer/fsm/smtps/smtps_fsm.hpp`

```cpp
enum class SmtpsState {
    INIT = 0,                  // 初始状态
    GREETING = 1,             // 发送欢迎消息
    WAIT_EHLO = 2,            // 等待 EHLO/HELO
    WAIT_AUTH = 3,            // 等待 AUTH 命令
    WAIT_AUTH_USERNAME = 4,   // 等待用户名
    WAIT_AUTH_PASSWORD = 5,   // 等待密码
    WAIT_MAIL_FROM = 6,       // 等待 MAIL FROM
    WAIT_RCPT_TO = 7,         // 等待 RCPT TO
    WAIT_DATA = 8,            // 等待 DATA 命令
    IN_MESSAGE = 9,           // 正在接收邮件内容
    WAIT_QUIT = 10,           // 等待 QUIT
    CLOSED = 11               // 已关闭
};

enum class SmtpsEvent {
    CONNECT = 0,      // 连接建立
    EHLO = 1,         // EHLO/HELO 命令
    AUTH = 2,         // AUTH 命令
    MAIL_FROM = 3,    // MAIL FROM 命令
    RCPT_TO = 4,      // RCPT TO 命令
    DATA = 5,         // DATA 命令
    DATA_END = 6,     // 数据结束
    QUIT = 7,         // QUIT 命令
    STARTTLS = 8,     // STARTTLS 命令
    ERROR = 9,        // 错误
    TIMEOUT = 10      // 超时
};
```

#### TraditionalSmtpsFsm 实现

**文件**: `include/mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.h`

**核心架构**:

```cpp
template <typename ConnectionType>
class TraditionalSmtpsFsm : public SmtpsFsm<ConnectionType> {
public:
    TraditionalSmtpsFsm(
        std::shared_ptr<ThreadPoolBase> io_thread_pool,
        std::shared_ptr<ThreadPoolBase> worker_thread_pool,
        std::shared_ptr<DBPool> db_pool
    );

    // 处理事件
    void process_event(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        SmtpsEvent event,
        const std::string& args
    ) override;

private:
    // 初始化状态转换表
    void init_transition_table();

    // 初始化状态处理函数
    void init_state_handlers();

    // 状态转换表
    std::map<
        std::pair<SmtpsState, SmtpsEvent>,
        SmtpsState
    > transition_table_;

    // 状态处理函数表
    std::map<
        SmtpsState,
        std::map<
            SmtpsEvent,
            std::function<void(
                std::unique_ptr<SessionBase<ConnectionType>>,
                const std::string&
            )>
        >
    > state_handlers_;
};
```

**状态转换示例**:

```cpp
// 初始化转换表
transition_table_[{SmtpsState::INIT, SmtpsEvent::CONNECT}] = SmtpsState::GREETING;
transition_table_[{SmtpsState::GREETING, SmtpsEvent::EHLO}] = SmtpsState::WAIT_MAIL_FROM;
transition_table_[{SmtpsState::WAIT_MAIL_FROM, SmtpsEvent::MAIL_FROM}] = SmtpsState::WAIT_RCPT_TO;
transition_table_[{SmtpsState::WAIT_RCPT_TO, SmtpsEvent::RCPT_TO}] = SmtpsState::WAIT_DATA;
transition_table_[{SmtpsState::WAIT_DATA, SmtpsEvent::DATA}] = SmtpsState::IN_MESSAGE;
transition_table_[{SmtpsState::IN_MESSAGE, SmtpsEvent::DATA_END}] = SmtpsState::GREETING;
transition_table_[{SmtpsState::GREETING, SmtpsEvent::QUIT}] = SmtpsState::CLOSED;
```

**事件处理流程**:

```
收到客户端命令
  ↓
SessionBase::handle_read() 解析命令 → 设置 next_event
  ↓
SessionBase::process_read() 调用
  ↓
FSM::auto_process_event() 读取 next_event
  ↓
FSM::process_event(session, event, args)
  ↓
查找 transition_table[state, event]
  ↓
查找并执行 state_handlers[state][event]
  ↓
更新 session->current_state_
  ↓
发送响应 + 继续读取
```

---

### 3.4 数据库层 (Database Layer)

#### 设计目标

提供数据库连接池管理和统一的数据库访问接口。

#### DBPool 抽象

**文件**: `include/mail_system/back/db/db_pool.h`

```cpp
class DBPool {
public:
    virtual ~DBPool() = default;

    // 获取连接
    virtual std::shared_ptr<IDBConnection> get_connection() = 0;

    // 释放连接
    virtual void release_connection(
        std::shared_ptr<IDBConnection> connection
    ) = 0;

    // 获取连接池大小
    virtual size_t get_pool_size() const = 0;

    // 获取可用连接数
    virtual size_t get_available_connections() const = 0;

    // 关闭连接池
    virtual void close() = 0;
};

struct DBPoolConfig {
    std::string achieve;              // 数据库类型 (mysql)
    std::string host;
    std::string user;
    std::string password;
    std::string database;
    std::string initialize_script;    // 初始化 SQL 脚本路径
    unsigned int port;
    size_t initial_pool_size;
    size_t max_pool_size;
    unsigned int connection_timeout;
    unsigned int idle_timeout;
};
```

#### MySQLPool 实现

**文件**: `include/mail_system/back/db/mysql_pool.h`

**特性**:
1. **连接池管理**: 维护可用连接队列
2. **连接复用**: 使用 ConnectionWrapper 包装连接
3. **连接验证**: 定期检查连接有效性
4. **自动清理**: 维护线程清理空闲连接

**关键实现**:

```cpp
class MySQLPool : public DBPool {
private:
    struct ConnectionWrapper {
        std::shared_ptr<IDBConnection> connection;
        std::chrono::steady_clock::time_point last_used;
        bool in_use;
    };

    std::vector<std::shared_ptr<ConnectionWrapper>> m_connections;
    std::queue<std::shared_ptr<ConnectionWrapper>> m_availableConnections;
    std::thread m_maintenanceThread;  // 维护线程

    void maintenance_thread();  // 定期清理空闲连接
    void cleanup_idle_connections();
    bool validate_connection(
        std::shared_ptr<IDBConnection> connection
    );
};
```

**⚠️ 重要问题**: Prepared Statement 兼容性问题

详见 `docs/prepared-statement-connection-pool-issue.md`

**问题本质**:
- MySQL C API 的 prepared statement 与连接池的验证机制存在兼容性问题
- 当连接池使用普通查询（`SELECT 1`）验证连接后，prepared statement 的状态会被重置
- 虽然执行成功，但参数值丢失，导致数据库中存储空值

**解决方案**:
- 使用 `escape_string()` + 直接 SQL 执行（推荐）
- 为 prepared statement 创建专门的连接池（可选）

#### 数据库表结构

**文件**: `sql/create_tables.sql`

```sql
-- 用户表
users (id, mail_address, password, name, telephone, register_time)

-- 邮件表
mails (id, sender, recipient, subject, body, send_time, status)

-- 附件表
attachments (id, mail_id, filename, filepath, file_size, mime_type, upload_time)

-- 邮箱表
mailboxes (id, user_id, name, is_system, box_type, create_time)

-- 邮件-邮箱关联表
mail_mailbox (id, mail_id, mailbox_id, user_id, is_starred, is_important, is_deleted)
```

---

### 3.5 线程池层 (ThreadPool Layer)

#### 设计目标

分离 I/O 线程和工作线程，提高并发性能。

#### ThreadPoolBase 基类

**文件**: `include/mail_system/back/thread_pool/thread_pool_base.h`

```cpp
class ThreadPoolBase {
public:
    // 提交任务（有返回值）
    template<class F, class... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>;

    // 提交任务（无返回值）
    template<class F>
    void post(F&& f);

    virtual void start() = 0;
    virtual void stop(bool wait_for_tasks = true) = 0;
    virtual size_t thread_count() const = 0;
    virtual bool is_running() const = 0;
};
```

#### IOThreadPool (IO 线程池)

**文件**: `include/mail_system/back/thread_pool/io_thread_pool.h`

**特点**:
1. **多个 io_context**: 每个 IO 线程对应一个 io_context
2. **负载均衡**: 通过 round-robin 分配任务
3. **工作守卫**: 使用 `executor_work_guard` 保持线程运行

**关键实现**:

```cpp
class IOThreadPool : public ThreadPoolBase {
public:
    IOThreadPool(size_t thread_count = std::thread::hardware_concurrency());

    // 获取 io_context（负载均衡）
    boost::asio::io_context& get_io_context() {
        static int counter = 0;
        counter %= m_thread_count;
        return *m_io_contexts[counter++];
    }

private:
    size_t m_thread_count;
    std::vector<std::shared_ptr<io_context>> m_io_contexts;
    std::vector<executor_work_guard<io_context::executor_type>> m_work_guards;
    std::vector<std::thread> m_threads;
};
```

#### BoostThreadPool (工作线程池)

**文件**: `include/mail_system/back/thread_pool/boost_thread_pool.h`

**特点**:
1. **基于 Boost.Asio thread_pool**: 适用于 CPU 密集型任务
2. **阻塞性任务**: 数据库查询、文件操作等

**关键实现**:

```cpp
class BoostThreadPool : public ThreadPoolBase {
private:
    size_t m_thread_count;
    std::unique_ptr<boost::asio::thread_pool> m_pool;
};
```

---

### 3.6 服务器层 (Server Layer)

#### ServerBase 基类

**文件**: `include/mail_system/back/mailServer/server_base.h`

**核心组件**:
1. **IO 上下文**: `std::shared_ptr<io_context>`
2. **SSL 上下文**: `ssl::context`
3. **接受器**: TCP 接受器和 SSL 接受器
4. **线程池**: IO 线程池和工作线程池
5. **数据库连接池**

**关键接口**:

```cpp
class ServerBase {
public:
    ServerBase(
        const ServerConfig& config,
        std::shared_ptr<ThreadPoolBase> ioThreadPool = nullptr,
        std::shared_ptr<ThreadPoolBase> workerThreadPool = nullptr,
        std::shared_ptr<DBPool> dbPool = nullptr
    );

    void start();   // 启动服务器
    void run();     // 运行服务器（阻塞）
    void stop(ServerState state = ServerState::Pausing);

protected:
    virtual void accept_ssl_connection();
    virtual void accept_tcp_connection();
    virtual void handle_accept(
        std::unique_ptr<ssl::stream<tcp::socket>>&& ssl_socket,
        const error_code& error
    ) = 0;
    virtual void handle_tcp_accept(
        std::unique_ptr<tcp::socket>&& socket,
        const error_code& error
    ) = 0;

protected:
    std::shared_ptr<io_context> m_ioContext;
    ssl::context m_sslContext;
    std::shared_ptr<tcp::acceptor> m_ssl_acceptor;
    std::shared_ptr<tcp::acceptor> m_tcp_acceptor;
    std::shared_ptr<ThreadPoolBase> m_ioThreadPool;
    std::shared_ptr<ThreadPoolBase> m_workerThreadPool;
    std::shared_ptr<DBPool> m_dbPool;
    ServerConfig m_config;
};
```

**启动流程**:

```
ServerBase::start()
  ↓
启动 IO 线程池
  ↓
启动工作线程池
  ↓
初始化数据库连接池
  ↓
创建 SSL/TCP 接受器
  ↓
启动监听线程 → 循环调用 accept
  ↓
状态 = Running
```

#### SmtpsServer 实现

**文件**: `include/mail_system/back/mailServer/smtps_server.h`

```cpp
class SmtpsServer : public ServerBase {
public:
    SmtpsServer(
        const ServerConfig& config,
        std::shared_ptr<ThreadPoolBase> ioThreadPool = nullptr,
        std::shared_ptr<ThreadPoolBase> workerThreadPool = nullptr,
        std::shared_ptr<DBPool> dbPool = nullptr
    );

    std::shared_ptr<SmtpsFsm<TcpConnection>> get_tcp_fsm() const;
    std::shared_ptr<SmtpsFsm<SslConnection>> get_ssl_fsm() const;

protected:
    void handle_accept(
        std::unique_ptr<ssl::stream<tcp::socket>>&& ssl_socket,
        const error_code& error
    ) override;
    void handle_tcp_accept(
        std::unique_ptr<tcp::socket>&& socket,
        const error_code& error
    ) override;

private:
    std::shared_ptr<SmtpsFsm<TcpConnection>> m_tcp_fsm;
    std::shared_ptr<SmtpsFsm<SslConnection>> m_ssl_fsm;
};
```

**连接处理流程**:

```
接受新连接
  ↓
创建 Connection (TcpConnection 或 SslConnection)
  ↓
创建 SmtpsSession
  ↓
SmtpsSession::start(self)
  ↓
执行 SSL 握手（或直接成功）
  ↓
FSM->process_event(CONNECT)
  ↓
发送欢迎消息
  ↓
开始读取 → 进入状态机循环
```

---

### 3.7 日志系统 (Logging System)

#### 设计目标

提供模块化、高性能的日志系统，支持编译时和运行时配置。

#### 日志模块分类

**文件**: `include/mail_system/back/common/logger.h`

| 模块 | 日志标识 | 宏前缀 | 默认状态 |
|------|----------|--------|----------|
| SERVER | SERVER | `LOG_SERVER_*` | INFO |
| NETWORK | NETWORK | `LOG_NETWORK_*` | INFO |
| DATABASE | DATABASE | `LOG_DATABASE_*` | INFO |
| DB_QUERY | DB_QUERY | `LOG_DB_QUERY_*` | WARN（默认禁用）|
| SMTP | SMTP | `LOG_SMTP_*` | INFO |
| SMTP_DETAIL | SMTP_DETAIL | `LOG_SMTP_DETAIL_*` | INFO |
| SESSION | SESSION | `LOG_SESSION_*` | INFO |
| THREAD_POOL | THREAD_POOL | `LOG_THREAD_POOL_*` | INFO |
| FILE_IO | FILE_IO | `LOG_FILE_IO_*` | INFO |
| AUTH | AUTH | `LOG_AUTH_*` | INFO |

#### 编译时控制

```bash
# 默认构建（禁用调试日志）
cmake -B build

# 启用所有调试日志
cmake -DENABLE_DEBUG_LOGS=ON -B build
```

#### 日志级别

```
trace - 最详细的跟踪信息
debug - 调试信息
info - 一般信息（生产环境默认）
warn - 警告信息
error - 错误信息
critical - 严重错误
```

#### 日志格式

```
[时间戳] [级别] [线程ID] [模块] 消息
```

示例:
```
[2026-01-05 10:30:45.123] [info] [12345] [SERVER] Server initialized with SSL: enabled
[2026-01-05 10:30:45.124] [info] [12346] [NETWORK] New SSL connection accepted
[2026-01-05 10:30:45.125] [warn] [12345] [DATABASE] Database pool is not initialized
[2026-01-05 10:30:45.126] [error] [12345] [DB_QUERY] MySQL connection error
```

详见:
- `docs/logging-guide.md` - 详细的日志配置和使用说明
- `docs/logging-completion.md` - 日志集成完成总结

---

## 4. 完整请求流程

### 4.1 客户端连接流程

```
客户端连接
  ↓
SSL 接受器接受连接（或 TCP 接受器）
  ↓
创建 SslConnection（或 TcpConnection）
  ↓
创建 SmtpsSession
  ↓
SmtpsSession::start()
  ↓
SSL 握手（SslConnection）或直接成功（TcpConnection）
  ↓
FSM->process_event(CONNECT)
  ↓
发送欢迎消息 "220 SMTPS Server Ready"
  ↓
SessionBase::do_async_read()
  ↓
[异步等待客户端数据]
  ↓
收到数据
  ↓
SmtpsSession::handle_read() → 解析 SMTP 命令
  ↓
设置 next_event (EHLO, AUTH, MAIL FROM, etc.)
  ↓
SmtpsSession::process_read()
  ↓
FSM->auto_process_event()
  ↓
FSM->process_event(session, event, args)
  ↓
查找并执行状态处理函数
  ↓
发送响应
  ↓
SessionBase::do_async_write()
  ↓
回调为空 → 自动调用 do_async_read()
  ↓
[循环回到异步读取]
  ↓
收到 QUIT 命令
  ↓
发送 "221 Bye"
  ↓
关闭连接 → Session 销毁
```

### 4.2 邮件接收流程

```
DATA 命令
  ↓
状态: WAIT_DATA → IN_MESSAGE
  ↓
发送 "354 End data with <CR><LF>.<CR><LF>"
  ↓
读取邮件内容（流式处理）
  ↓
解析邮件头 (header_buffer)
  ↓
检测 Content-Type: multipart
  ↓
提取 boundary 标记
  ↓
逐行解析 body:
  ├─ 检测 boundary
  ├─ 解析 part headers
  ├─ 文本 part → text_body_buffer
  └─ 附件 part → 流式写入磁盘
  ↓
检测 "\r\n.\r\n"
  ↓
状态: IN_MESSAGE → GREETING
  ↓
保存邮件到数据库
  ↓
发送 "250 OK"
```

---

## 5. 配置文件

### 5.1 SMTPS 服务器配置

**文件**: `config/smtpsConfig.json`

```json
{
  "address": "0.0.0.0",
  "port": 465,
  "use_ssl": true,
  "enable_ssl": true,
  "enable_tcp": true,
  "ssl_port": 465,
  "tcp_port": 25,
  "certFile": "crt/server.crt",
  "keyFile": "crt/server.key",
  "maxMessageSize": 1048576,
  "maxConnections": 1000,
  "io_thread_count": 4,
  "worker_thread_count": 4,
  "use_database": true,
  "db_config_file": "db_config.json",
  "connection_timeout": 300,
  "read_timeout": 60,
  "write_timeout": 60,
  "require_auth": false,
  "max_auth_attempts": 3,
  "log_level": "info",
  "log_file": "../logs/server.log",
  "mail_storage_path": "../mail/",
  "attachment_storage_path": "../attachments/"
}
```

### 5.2 数据库配置

**文件**: `config/db_config.json`

```json
{
  "achieve": "mysql",
  "host": "localhost",
  "user": "mail_test",
  "password": "abjskKA09qjf",
  "database": "mail",
  "initialize_script": "sql/create_tables.sql",
  "port": 3306,
  "initial_pool_size": 5,
  "max_pool_size": 10,
  "connection_timeout": 5,
  "idle_timeout": 60
}
```

---

## 6. 构建和运行

### 6.1 构建说明

```bash
cd /Users/zhuhongrui/Desktop/code/c++/project/mail-system/v6
mkdir build && cd build
cmake ..
make -j4
```

### 6.2 运行测试

```bash
./test/smtpsServer
```

### 6.3 查看日志

```bash
# 实时查看日志
tail -f logs/mail_system.log

# 只看错误
tail -f logs/mail_system.log | grep -E "(error|critical)"

# 只看网络日志
tail -f logs/mail_system.log | grep NETWORK

# 统计错误数量
grep -c "error" logs/mail_system.log
```

---

## 7. 设计模式应用

| 设计模式 | 应用场景 | 文件位置 |
|----------|----------|----------|
| 类型擦除 | IConnection 接口 | `connection/i_connection.h` |
| 策略模式 | ThreadPoolBase | `thread_pool/thread_pool_base.h` |
| 模板方法 | SmtpsFsm | `fsm/smtps/smtps_fsm.hpp` |
| 工厂模式 | MySQLPoolFactory | `db/mysql_pool.h` |
| 观察者模式 | 信号处理 | `test/smtps_test.cpp` |
| RAII | unique_ptr | 整个项目 |
| 单例模式 | Logger | `common/logger.h` |

---

## 8. 架构优势

### 8.1 内存管理

- ✅ **完全避免 shared_ptr**: 使用 unique_ptr 明确所有权
- ✅ **移动语义**: 减少拷贝开销
- ✅ **静态函数传递 self**: 避免循环引用

### 8.2 性能优化

- ✅ **零拷贝**: 使用 buffer 避免数据复制
- ✅ **异步非阻塞**: 基于 Boost.Asio 的异步 I/O
- ✅ **连接池**: 复用数据库连接
- ✅ **线程池**: 分离 IO 线程和工作线程

### 8.3 可扩展性

- ✅ **接口抽象**: 易于添加新的连接类型/数据库
- ✅ **模板化**: Session 支持不同 ConnectionType
- ✅ **状态机可配置**: 易于修改 SMTP 协议流程

### 8.4 代码质量

- ✅ **C++20 现代特性**: Concepts, Ranges 等
- ✅ **日志系统**: spdlog 支持
- ✅ **配置驱动**: JSON 配置文件

---

## 9. 遇到的问题和解决方案

### 9.1 Prepared Statement 与连接池兼容性问题

**问题描述**:
在使用 MySQL prepared statement 时，虽然日志显示参数绑定正确且 `affected_rows: 1`，但数据库中实际保存的数据全是空的。

**根本原因**:
- MySQL C API 的 prepared statement 与连接池的验证机制存在兼容性问题
- 当连接池使用普通查询（`SELECT 1`）验证连接后，prepared statement 的状态会被重置

**解决方案**:
- 使用 `escape_string()` + 直接 SQL 执行（推荐）
- 为 prepared statement 创建专门的连接池（可选）

**详细分析**: `docs/prepared-statement-connection-pool-issue.md`

### 9.2 日志系统集成

**问题**:
原系统使用 `std::cout` 和 `std::cerr` 输出，不利于生产环境管理。

**解决方案**:
- 集成 spdlog 日志系统
- 实现 10 个独立模块的日志分类
- 支持编译时和运行时配置

**详细文档**:
- `docs/logging-guide.md` - 详细的日志配置和使用说明
- `docs/logging-completion.md` - 日志集成完成总结

### 9.3 快速配置指南

**问题**:
用户需要快速了解如何配置和运行系统。

**解决方案**:
- 提供快速配置指南
- 涵盖常见场景（调试、生产环境等）

**详细文档**: `docs/quick-config.md`

---

## 10. 待完成功能

根据 README.md，以下功能尚未完成：


---

## 11. 总结

Mail System V6 项目展现了一个现代化的 C++ 网络服务架构，具有以下特点：

1. **清晰的分层架构**: 连接层 → 会话层 → 状态机层 → 服务器层
2. **高效的内存管理**: 完全避免 shared_ptr，使用 unique_ptr 和移动语义
3. **完善的异步支持**: 基于 Boost.Asio 的全异步 I/O
4. **可扩展的设计**: 接口抽象，易于添加新功能
5. **生产级质量**: 完整的日志系统、配置管理、错误处理

该架构特别适合高并发的邮件服务场景，能够高效处理大量 SMTP 连接和邮件投递请求。

---

## 12. 参考资料

- [Boost.Asio 官方文档](https://www.boost.org/doc/libs/release/libs/asio/)
- [spdlog 官方文档](https://github.com/gabime/spdlog)
- [MySQL C API 文档](https://dev.mysql.com/doc/c-api/)
- [SMTP 协议 RFC](https://tools.ietf.org/html/rfc5321)
