# VISA 兼容矩阵

基线：IVI VPP-4.3 Rev. 7.2.1。状态含义：`Implemented` 已实现且有当前平台测试；`Partial` 只覆盖说明范围；`Declared` ABI 已声明但当前后端只提供最小行为；`Not implemented` 未实现；`NOT_TESTED` 已构建但未在该平台运行。

| 领域 | 0.6 状态 | 说明 |
|---|---|---|
| C ABI 与 VISA 基础类型 | Implemented (Linux) / NOT_TESTED (Windows 0.4–0.6) | C11/C++20 头编译、固定宽度、平台导出/调用约定；0.6 未新增公共 ABI，独立历史清单仍冻结 26 个 0.1–0.5 符号所属节点 |
| 默认 RM、打开、关闭 | Implemented (Linux/Windows network) | 两平台可打开模拟和 TCPIP `SOCKET`/`INSTR`；ASRL runtime 仅 Linux 验证；关闭先使句柄失效，再取消、恢复或关闭真实传输 |
| 标准资源字符串解析 | Partial | ASRL/GPIB/TCPIP/USB 初始语法、括号 IPv6 Socket 及 TCPIP INSTR 的 VXI-11/HiSLIP 确定性路由；无 VXI/PXI；新增 RM 范围、非持久化的一对一 alias |
| `WRVISA0::MOCK::INSTR` | Implemented | `PROJECT_EXTENSION`，仅测试；通用查找不暴露，须显式使用 `WRVISA_MOCK_FIND_EXPRESSION` |
| Find 资源表达式 | Partial (Linux) / NOT_TESTED (Windows 0.4) | VPP 字符、集合、分组、选择、`*`/`+`；属性过滤支持 `&&`/`||`/`!`/括号、数值比较、字符串等值及五个静态属性；其他属性明确拒绝 |
| Find/Parse 可选输出 | Implemented (Linux) / NOT_TESTED (Windows 0.4) | `viFindRsrc` find-list/count 与 `viParseRsrcEx` class/expanded/alias 可为 `VI_NULL`；省略 find-list 不泄漏句柄 |
| 资源 alias | PROJECT_EXTENSION (Linux) / NOT_TESTED (Windows 0.4) | `wrvisaSetResourceAlias` 配置当前 RM；大小写无关，`viOpen`/`viParseRsrc`/`viParseRsrcEx` 一致；不持久化、不作为重复资源枚举 |
| Read/Write | Implemented (Linux/Windows network) | 两平台验证模拟、raw TCP、VXI-11、HiSLIP，Linux 另验证 ASRL、USBTMC、配置后的 USB RAW 与测试专用 GPIB provider；支持终止符、EOI/EOM/read-ahead、超时、逐 operation 取消和恢复后复用 |
| Clear/Flush | Partial | 模拟、ASRL、VXI-11、HiSLIP、USBTMC class clear、RAW 端点 halt 和具备能力的测试 GPIB transport 支持；raw TCP clear 不伪造消息协议行为；flush 仍按后端能力 |
| 属性 | Partial | 公共超时/终止符/资源/RM/缓冲属性；TCP 地址/主机/端口/Nagle/keepalive；ASRL 波特率/数据位/奇偶/停止位/流控子集 |
| 状态描述 | Partial | 覆盖首批 API 与 0.2–0.6 当前会产生的状态；不是完整 VISA 状态全集 |
| `viTerminate` | Implemented (Linux/Windows network; USB/GPIB simulators) | `VI_NULL` 取消当前会话排队及进行中操作；VXI-11 使用 abort，HiSLIP 使用 async clear，USBTMC 使用 libusb 异步 cancel callback 后的 bulk abort/clear 回退恢复，RAW 等待 callback，GPIB transport 接收取消通知；失败调用均不提交读取缓冲，异步 job ID 尚无来源 |
| 状态字/触发 | Partial | 模拟、VXI-11、HiSLIP、USB488 和具备能力的测试 GPIB transport 支持；生产 GPIB 控制器未接入，raw TCP/ASRL 不伪造协议能力 |
| 锁 | Partial | 进程内共享/排他和访问键；HiSLIP 远端共享/排他，VXI-11 远端排他；VXI-11 共享锁及跨进程协调未实现 |
| 插件 | Declared | 版本化 ABI 契约；动态加载未实现 |
| TCPIP `SOCKET` | Implemented (Linux/Windows loopback) | DNS/IPv4/括号 IPv6、连接 deadline、终止符、read-ahead、读写和连接丢失；无 TLS |
| ASRL | Implemented (Linux) / NOT_TESTED (Windows runtime) | Linux 原生发现与 PTY 集成测试；Windows 枚举/构建已实现，Windows ASRL runtime `NOT_TESTED` |
| TCPIP `INSTR` 路由 | Implemented (Linux/Windows) | `hislip...` 选 HiSLIP；省略设备名或 `vxi`/`gpib`/`inst...` 选 VXI-11；未知名明确不支持，无失败回退 |
| VXI-11 | Implemented (Linux/Windows simulator) | 自有 XDR/RPC/portmapper、core+abort、分块 I/O、状态字、清除、触发、远端排他锁、超时/取消恢复；真实仪器 `NOT_TESTED` |
| HiSLIP 1.x | Implemented (Linux/Windows simulator) | 同步模式、双通道、最大消息长度、Data/DataEnd、状态字、清除、触发、远端共享/排他锁、超时/取消恢复；overlap、TLS/HiSLIP 2 未实现，真实仪器 `NOT_TESTED` |
| HiSLIP vendor ID | Provisional | 当前发送项目临时 ID `WR`（`0x5752`）；尚未向 IVI Foundation 注册，不能据此声称正式互操作认证 |
| TCPIP 服务端口覆盖 | PROJECT_EXTENSION | `wrvisaSetTcpipServicePort` 按 RM/主机/协议覆盖 VXI-11 portmapper 或 HiSLIP 端口，用于测试与自定义网关 |
| USBTMC `INSTR` | Partial (production libusb adapter + simulator, Linux) | 可选 libusb 1.0.30 动态适配器已实现枚举/匹配、共享 claim、异步 bulk/control/interrupt、安全取消与热拔出；资源、DEV_DEP、class clear/abort、USB488 状态/触发及公共 `vi*` 闭环由受控 libusb API 模拟器验证。真实设备、驱动/权限组合与 Windows runtime 仍为 `NOT_TESTED` |
| USB `RAW` | DE_FACTO_EXTENSION (production libusb adapter + simulator, Linux) | 与 `INSTR` 分开解析/发现；`wrvisaSetUsbRawConfig` 以版本化结构在 RM 上固定 alternate setting 和 bulk/interrupt/none 读写端点，标准 `viRead`/`viWrite` 执行 I/O，`wrvisaUsbControlTransfer` 执行 16 位长度上限的端点零 IN/OUT。未配置返回 `VI_ERROR_INTF_NUM_NCONFIG`；真实设备与 Windows runtime `NOT_TESTED` |
| 本机 GPIB | Partial (Linux simulator) | 已实现 board/主地址/可选次地址身份、provider/transport、EOI/send-end、可选 clear/trigger/serial poll、取消/超时和公共 `vi*` 闭环；`INTFC` 会话及 linux-gpib、NI-488.2、Prologix、真实控制器/仪器均未实现或 `NOT_TESTED` |
| 厂商 VISA | Not implemented | 未加载或转发 NI、Keysight 等厂商 VISA；GPIB 形式的 VXI-11 LAN device name 不等于本机 GPIB 后端 |
| Windows 0.3 原生运行 | Partial | MSVC Debug/Release/ASan、raw TCP、VXI-11/HiSLIP loopback、安装消费和 PE 导出已验证；0.4–0.6 新增能力尚未原生重跑，Windows ASRL/runtime、GPIB 和真实仪器 `NOT_TESTED` |
| 二进制兼容承诺 | Partial | 尚未作稳定版 ABI 承诺；ELF 0.1–0.5 节点分层，机器清单禁止既有符号删除/迁移，ELF/PE 继续做精确导出检查 |

标准资源可被成功解析不表示可被打开。`viOpen` 对没有已注册后端或明确未支持模式的合法资源返回 `VI_ERROR_NSUP_OPER`；libusb 存在但设备不存在、权限不足或拔出时分别保留可诊断状态。公共 API 的错误参数/句柄/属性/锁边界以及协议截断和畸形长度已纳入 Linux 与 Windows 无硬件回归；loopback、USB/GPIB API 模拟器仍只证明本项目客户端与受控行为闭环，不等同于第三方真实仪器互操作。Windows 原生网络/协议已有运行证据，但 0.5 libusb、0.6 GPIB 与 ASRL runtime 均不能由其他平台或编译结果推断通过。
