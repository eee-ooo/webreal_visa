# ADR-0007：VXI-11 与 HiSLIP 消息协议边界

状态：Accepted（2026-08-11）

## 背景

`0.3` 需要在现有同步 VISA C API 和共享 Asio runtime 上实现 TCPIP `INSTR`。VXI-11 建立在 ONC RPC/XDR 与多个 program/channel 上；HiSLIP 使用带 16 字节头的同步、异步双通道协议。二者都具有消息边界、事务状态、远端控制和取消恢复语义，不能作为 raw TCP 字节流处理。系统 SunRPC/libtirpc 也不是 Windows、Linux、macOS 一致可用的核心前提。

## 决策

VXI-11 使用第一方最小 XDR、ONC RPC v2、record-marking 和 portmapper 客户端，分别建模 core 与 abort 通道。HiSLIP 使用独立帧编解码和同步数据/异步控制通道，只声明并实现 1.x 同步模式；overlap、HiSLIP 2/TLS 不以降级方式开放。

两种协议复用一个有界 `RequestChannel`：它持有共享 `IoRuntime`、为每个通道建立 strand 请求队列，并在分配 payload 前按 RPC record 或 HiSLIP 协商上限校验长度。`IoRuntime` 由进程生命周期强引用持有，不能由某个完成回调释放最后引用并在 worker 上自我 join。VXI-11 取消通过 abort 通知设备并排空对应 RPC 响应；HiSLIP 取消通过异步 device-clear/interrupt 握手恢复。恢复结束前不调度后续请求。

资源解析依据 LAN device name 确定性选择协议，不以连接失败静默回退。默认使用 VXI-11 portmapper 111 和 HiSLIP 4880；项目扩展 `wrvisaSetTcpipServicePort` 允许在一个 RM 内按主机/协议覆盖端口，以支持非特权测试模拟器和自定义网关，不改变标准资源字符串。

本地锁先取得，第一次嵌套加锁时再请求远端；远端失败回滚本地锁，最后一次解锁时释放远端。HiSLIP 映射共享与排他远端锁；VXI-11 只映射协议原生的排他锁，VISA 共享锁仍为进程内协调。

## 后果

协议响应不会被任意 TCP 分片或上一条已取消请求污染，单个恶意长度也不能触发无界分配。VXI-11/HiSLIP 不引入新的第三方库，继续共享固定 Asio 依赖和有限线程 runtime。生产库只包含客户端；loopback 协议服务器仅存在于测试代码。

当前 HiSLIP 初始化使用临时项目 vendor ID `WR`；正式互操作声明前必须按 IVI VPP-9 完成注册或取得有效分配。Linux 模拟器测试不替代真实仪器、Windows 或 macOS 互操作验证。

## 被否决方案

- 将 VXI-11/HiSLIP 强行适配为 raw stream：会丢失消息边界、远端控制和取消恢复语义。
- 依赖系统 SunRPC、`rpcgen` 或 libtirpc：无法形成一致的三平台核心构建前提，并引入新的发行许可边界。
- 每个通道独占永久线程：线程数会随会话与协议通道线性增长。
- VXI-11 与 HiSLIP 在连接失败后相互回退：会隐藏资源配置错误并可能连接到错误服务。
- 取消时只关闭 socket 或直接开始下一请求：会破坏可复用会话，或把残留响应错配给下一 operation。
