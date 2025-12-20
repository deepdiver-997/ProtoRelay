# 泛型邮件服务器设计

这个设计实现了支持SSL和非SSL两种模式的邮件服务器，可以在同一安全网络内使用非加密通信来节省资源。

## 架构概述

### 核心组件

1. **ConnectionBase** - 泛型连接基类
   - 支持SSL和非SSL连接
   - 统一的接口设计
   - 自动握手处理（SSL模式）

2. **GenericSessionBase** - 泛型会话基类
   - 基于模板的设计
   - 支持两种连接类型
   - 统一的读写接口

3. **HybridServerBase** - 混合模式服务器
   - 同时支持SSL和非SSL连接
   - 可配置启用/禁用各种模式
   - 独立的端口管理

### 编译选项

项目支持两种编译模式：

#### 启用SSL支持（默认）
```bash
mkdir build && cd build
cmake -DUSE_SSL=ON ..
make
```

#### 禁用SSL支持
```bash
mkdir build && cd build
cmake -DUSE_SSL=OFF ..
make
```

## 使用方式

### 1. 仅SSL模式
```cpp
ServerConfig ssl_config;
ssl_config.address = "0.0.0.0";
ssl_config.port = 465;
ssl_config.use_ssl = true;
ssl_config.certFile = "config/cert.pem";
ssl_config.keyFile = "config/key.pem";

auto server = std::make_unique<SMTPSServer>(ssl_config);
server->start();
```

### 2. 仅非SSL模式
```cpp
ServerConfig tcp_config;
tcp_config.address = "0.0.0.0";
tcp_config.port = 25;
tcp_config.use_ssl = false;

auto server = std::make_unique<SMTPSServer>(tcp_config);
server->start();
```

### 3. 混合模式（同时支持SSL和非SSL）
```cpp
ServerConfig ssl_config;
ssl_config.address = "0.0.0.0";
ssl_config.port = 465;
ssl_config.use_ssl = true;
ssl_config.certFile = "config/cert.pem";
ssl_config.keyFile = "config/key.pem";

ServerConfig tcp_config;
tcp_config.address = "0.0.0.0";
tcp_config.port = 25;
tcp_config.use_ssl = false;

auto server = std::make_unique<HybridSMTPSServer>(ssl_config, tcp_config);
server->start();
```

### 4. 从配置文件加载
```cpp
ServerConfig config;
config.loadFromFile("config/generic_server_config.json");

if (config.allow_insecure) {
    // 混合模式
    ServerConfig tcp_config = config;
    tcp_config.port = config.insecure_port;
    tcp_config.use_ssl = false;
    
    auto server = std::make_unique<HybridSMTPSServer>(config, tcp_config);
    server->start();
} else {
    // 仅SSL模式
    auto server = std::make_unique<SMTPSServer>(config);
    server->start();
}
```

## 配置选项

### ServerConfig 新增字段

- `use_ssl`: 是否使用SSL/TLS
- `allow_insecure`: 是否允许非SSL连接（用于内网通信）
- `insecure_port`: 非SSL连接端口（如果与SSL端口不同）

### 编译时优化

- 启用SSL：完整的SSL支持，适合生产环境
- 禁用SSL：完全编译掉SSL代码，减少二进制大小，适合纯内网环境

## 性能优势

### 内网通信优化
- 同一安全网络内的服务器间通信可使用非SSL模式
- 减少CPU开销（无需加解密）
- 降低延迟
- 保持连接复用优势

### 连接复用
- 支持基于域名的连接池
- 多封邮件可复用同一连接
- 减少握手开销

## 示例代码

查看 `examples/generic_server_example.cpp` 了解完整的使用示例。

## 配置文件示例

查看 `config/generic_server_config.json` 了解配置格式。

## 迁移指南

### 从现有代码迁移

1. **包含新的头文件**
   ```cpp
   #include "mail_system/back/mailServer/session/generic_session_base.h"
   #include "mail_system/back/mailServer/connection/connection_base.h"
   ```

2. **修改Session类**
   ```cpp
   // 原来的方式
   class MySession : public SessionBase {
   
   // 新的方式
   class MySSLSession : public SSLSessionBase {
   class MyTCPSession : public TCPSessionBase {
   ```

3. **修改Server类**
   ```cpp
   // 使用泛型基类
   class MyServer : public HybridServerBase<MySSLSession, MyTCPSession> {
   ```

## 注意事项

1. **安全性考虑**
   - 非SSL模式仅应用于可信的内网环境
   - 外网通信必须使用SSL
   - 可通过防火墙限制非SSL端口的访问

2. **配置验证**
   - SSL模式下必须提供有效的证书和私钥
   - 非SSL端口不能与SSL端口冲突
   - 建议在生产环境中禁用非SSL模式

3. **向后兼容**
   - 现有的SSL代码无需修改即可继续工作
   - 可以逐步迁移到新的泛型设计

## 技术实现细节

### 模板特化
- `ConnectionBase` 对SSL连接有特殊实现，包含握手逻辑
- 其他连接类型使用通用实现

### 条件编译
- 使用 `#ifdef USE_SSL` 进行条件编译
- 禁用SSL时完全不包含SSL相关代码

### 类型别名
- `SSLConnection` 和 `TCPConnection` 简化类型使用
- `SSLSessionBase` 和 `TCPSessionBase` 提供基类

这个设计在保持现有代码兼容性的同时，为内网通信提供了性能优化的选择。