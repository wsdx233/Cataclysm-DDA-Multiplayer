# ADR-0003：standalone Asio/TCP 与 TLS 发布策略

- 状态：已接受
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
- standalone Asio 固定为 `1.38.1`（tag `asio-1-38-1`、commit
  `dfd7b3e3145bac5d0e91a99fde69c6ae1442f971`、archive SHA-256
  `2827b229972be80cdb14e5497962fa393d1adf036b5869e2b9c99f644daadacc`），许可证为
  Boost Software License 1.0。生产依赖仍须以独立于 spike 的构建边界接入。

本次 spike 没有选择或打包嵌入式 TLS 后端，因此首个可用版本采用明确的
`loopback / 显式受信 LAN / 外部认证加密隧道` 策略：默认 plaintext 监听只能是 loopback；非 loopback
plaintext 必须由管理员显式启用并只用于受信 LAN；WAN 部署必须使用 WireGuard、Tailscale 等外部认证加密
隧道。未来若引入嵌入式 TLS，必须另行完成 TLS 1.3、证书验证、Android/MSVC/Linux 打包和依赖更新门禁，
不能把本 ADR 的接受状态解释为已经验证 TLS。

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
- `tools/multiplayer/transport_spike/` 是冻结的可行性和回归工具，不链接游戏目标，也不能逐步演变为生产协议实现。
- Android 交叉构建只证明编译和 ABI；原生 loopback 行为合同由 Linux 和 Windows 执行。

## Phase 0 验证结果

- GitHub Actions run `29205262750`（提交
  `a39e06eb8620b377f515b6a8a7c8731b30543ebe`）的 Linux GCC 13、Linux Clang 18、Windows
  MSVC 17.14 和 Android NDK `28.1.13356709` arm64 三个 job 全部成功。
- Linux GCC/Clang 和 Windows MSVC 原生 loopback 均通过 one-byte fragmentation、coalesced frames、
  1 MiB 边界及超长拒绝、半关闭、accept cancellation、有界队列饱和/FIFO 和有序 shutdown；同一 harness
  本地连续运行 100 次无失败。
- Android artifact 是 ELF64 AArch64，内部 binary SHA-256 为
  `5065fdb41b6ec45f18d165d28bf5e36d75b6e7bc4c58e10980a3c669bc380b46`。完整 baseline run
  `29205262759` 的 APK 所有原生库均只位于 `arm64-v8a/`，包含 `libmain.so`，并声明
  `android.permission.INTERNET`。
- 三个 transport artifacts 的 ID 分别为 Linux `8263559175`、Android `8263560332`、Windows
  `8263560944`；GitHub artifact digest、内部 binary hash、provenance 和 Asio 许可证均已独立下载核对。
- 未验证嵌入式 TLS；门禁按上述 LAN/VPN 限制分支关闭。生产实现若违反默认 loopback 或允许隐式明文公网
  监听，即违反本 ADR。
