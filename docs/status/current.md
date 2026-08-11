# 当前工程状态

更新时间：2026-08-11

当前版本：`0.2.0`。阶段 0 与 `0.1` 已验收；`0.2` 授权实现已完成并通过 Linux 原生验证，真实 Windows 运行门禁尚未执行，因此当前准确状态是“实现完成、跨平台阶段验收待完成”，不能宣称 `0.2` 已完整验收。实施证据见 [`docs/progress/2026-08-11-stage-0.2.md`](../progress/2026-08-11-stage-0.2.md)。

## 现在可用

- 原有 21 个 `vi*` C ABI、模拟后端、资源解析/查找、代际句柄、operation、进程内锁和 CMake 安装消费能力。
- 真实 `TCPIP[board]::host::port::SOCKET`：DNS/IPv4/括号 IPv6、连接超时、读写、终止符、read-ahead、逐 operation 取消、TCP no-delay/keepalive 属性和立即关闭。
- 真实 ASRL：Linux/macOS 设备节点发现、Windows COM 枚举实现、`wrvisaSetSerialPath` 显式本机路径映射、9600-8N1 默认值、基础波特率/帧/流控属性、读写、清除和刷新。
- 进程共享的 standalone Asio I/O runtime、每会话 strand、独立读写队列、绝对 deadline 与取消后会话复用。
- Asio 1.38.2 固定提交和归档 SHA-256、本地离线源码入口、第三方声明和 Boost Software License 1.0 原文。

## 已验证

- Linux Debug/Release CTest 均为 9/9；ASan/UBSan 9/9；本机 TCP 服务端和 POSIX PTY 串口集成测试包含在内。
- 并发、TCP、串口测试各连续 100 轮通过；安装后独立 C 消费通过；默认固定下载与离线 Asio 路径均已验证。
- ELF 只导出 21 个 0.1 `vi*` 和 1 个 0.2 扩展；Windows x64 MinGW 交叉构建、22 函数 PE 导出、安装与独立 C 交叉消费通过。这仍不等于 Windows 原生运行通过。

## 当前限制与发布阻塞

- Windows ASRL 与 TCP 原生运行状态：`NOT_TESTED`。仓库尚未推送，配置的 GitHub Windows 门禁没有可执行的远端提交。
- 版权主体仍为 `[TBD_COPYRIGHT_HOLDER]`；正式项目 `LICENSE` 和对外发布被阻塞。Asio 第三方许可证不受此影响，已经随仓库保留。
- TCPIP `INSTR`、VXI-11、HiSLIP、USB、GPIB、厂商 VISA、TLS、动态插件加载和异步 job API 未实现。
- ASRL 的 mark/space parity、DTR/DSR 流控以及完整 VISA 串口属性未实现；macOS 构建/运行也尚未验证。
- 锁只协调当前进程；Find 属性过滤表达式、完整别名/资源类型和稳定版二进制兼容尚未实现。
- ThreadSanitizer 在当前容器因运行时内存映射不兼容而无法启动；不得记为通过。

## 下一步

先在真实 Windows runner 运行 Debug/Release 构建与 TCP/串口适用门禁，补足 `0.2` 阶段验收；Windows 串口最好再以真实 COM 或可控虚拟串口做集成验证。除此之外保持 `0.2` 范围冻结，不自动进入 `0.3`。获得下一阶段授权后，再从 TCPIP `INSTR`/VXI-11、HiSLIP、完整 ASRL 或 USB 中定义新的最小纵向切片；对外发布前先确认版权主体。
