# ADR-0003：standalone Asio/TCP 与 TLS 发布策略

- 状态：待验证
- 日期：2026-07-12
- 关联计划：第 12、15、19、20 节

## 背景

首版只需要两到四名玩家直连私服。命令流量低，但必须可靠、有序，并支持 Linux/Windows server、Windows client 和 Android client。网络层不能依赖 SDL、curses 或具体 UI，也不能从 I/O 线程调用游戏规则。

公网传输需要成熟加密。Android NDK、Windows MSVC 和 Linux 的 TLS 依赖、证书存储与打包方式尚未在本 fork 基线上验证，因此不能提前锁定未经编译证明的 TLS 后端。

## 决策

Phase 0 transport spike 采用 standalone Asio + TCP，并通过抽象 transport 接口隔离协议和连接状态机：

- TCP 只提供字节流；应用层实现有长度和版本的 frame codec，并正确处理分片与粘包。
- 一个 I/O 线程负责 socket 状态机，将不可变 DTO 放入有界队列；不读取 live game object。
- 默认只绑定 loopback。plaintext 只能由管理员显式启用，用于开发或受信 LAN。
- 面向公网的发布必须使用 TLS 1.3，或明确要求 WireGuard、Tailscale 等外部加密隧道。
- 不设计自定义加密、密钥交换或证书协议。
- Asio 与 TLS 依赖必须固定版本、校验值和许可证，并按仓库现有第三方依赖方式打包。

TLS 后端的最终选择由三平台 spike 决定。如果嵌入式 TLS 在 Android、MSVC 或发布打包门禁失败，首个可用版本必须明确限制为 loopback/LAN/VPN，不能悄悄降级为默认明文公网监听。

## 替代方案

- UDP 或自定义可靠层：拒绝。回合制命令不需要承担重传、拥塞和乱序协议复杂度。
- SDL_net：拒绝作为核心边界。服务器网络层必须独立于 SDL，且仍需另行解决 TLS、framing 和异步状态机。
- WebSocket：暂不采用。首版没有浏览器客户端，额外 framing 和 HTTP 握手没有直接收益。
- 自定义加密：拒绝。安全性和维护成本不可接受。

## 后果

- 业务幂等、revision 和重连恢复仍由协议层负责，TCP 有序不等于命令最多执行一次。
- frame 大小、解压后大小、速率、pending request 和发送队列都必须有硬上限。
- Android manifest 的 `INTERNET` 权限与网络生命周期处理应随 transport spike 一起加入，而不是作为无关基线改动。
- TLS 证书、私钥和 session token 必须由平台私有存储处理，日志不得记录秘密。

## Phase 0 验证门禁

- 同一最小 Asio/TCP echo/frame 程序在 Linux GCC/Clang、Windows MSVC 和 Android NDK arm64 编译通过。
- loopback 测试覆盖 frame 分片、粘包、半关闭、连接取消、队列满和有序 shutdown。
- TLS 候选后端在三平台完成握手、证书校验和发布产物打包验证，或形成明确的 LAN/VPN 限制决定。
- Android APK 只包含请求的 ABI，新增权限和原生库均通过 APK 检查。
- 通过门禁后更新本 ADR 为 `已接受`，记录 Asio/TLS 版本、构建方式和验证证据。
