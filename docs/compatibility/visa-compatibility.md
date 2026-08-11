# VISA 兼容矩阵

基线：IVI VPP-4.3 Rev. 7.2.1。状态含义：`Implemented` 已实现且有当前平台测试；`Partial` 只覆盖说明范围；`Declared` ABI 已声明但当前后端只提供最小行为；`Not implemented` 未实现；`NOT_TESTED` 已构建但未在该平台运行。

| 领域 | 0.2 状态 | 说明 |
|---|---|---|
| C ABI 与 VISA 基础类型 | Implemented | C11/C++20 头编译、固定宽度、平台导出/调用约定；0.1 符号版本继续保留 |
| 默认 RM、打开、关闭 | Implemented | 可打开模拟资源、TCPIP `SOCKET` 和已发现或显式映射的 ASRL；关闭立即取消并关闭真实传输 |
| 标准资源字符串解析 | Partial | ASRL/GPIB/TCPIP/USB 初始语法和括号 IPv6 TCP Socket；无 VXI/PXI/复杂别名 |
| `WRVISA0::MOCK::INSTR` | Implemented | `PROJECT_EXTENSION`，仅测试；通用查找不暴露，须显式使用 `WRVISA_MOCK_FIND_EXPRESSION` |
| Find 资源表达式 | Partial | VPP 字符、集合、分组、选择、`*`/`+`；可发现本机 ASRL；属性过滤未实现 |
| Read/Write | Implemented | 模拟、raw TCP Socket、ASRL；真实后端支持超时、逐 operation 取消和取消后复用 |
| Clear/Flush | Partial | 模拟与 ASRL 支持；raw TCP 的 `viClear` 不伪造协议行为，返回不支持 |
| 属性 | Partial | 公共超时/终止符/资源/RM/缓冲属性；TCP 地址/主机/端口/Nagle/keepalive；ASRL 波特率/数据位/奇偶/停止位/流控子集 |
| 状态描述 | Partial | 覆盖首批 API 与 `0.2` 真实传输会产生的状态 |
| `viTerminate` | Implemented | `VI_NULL` 取消当前会话排队及进行中的操作；异步 job ID 尚无来源 |
| 锁 | Partial | 同进程共享/排他和访问键；跨进程锁未实现 |
| 插件 | Declared | 版本化 ABI 契约；动态加载未实现 |
| TCPIP `SOCKET` | Implemented | DNS/IPv4/括号 IPv6、连接 deadline、终止符、read-ahead、读写和连接丢失；无 TLS |
| ASRL | Implemented (Linux) / NOT_TESTED (Windows runtime) | Linux 原生发现与 PTY 集成测试；Windows 枚举/构建已实现，尚无真实 Windows 运行证据 |
| TCPIP `INSTR` / VXI-11 / HiSLIP | Not implemented | 合法资源可解析，但不可打开 |
| USB / GPIB / 厂商 VISA | Not implemented | `0.2` 范围之外 |
| 二进制兼容承诺 | Not implemented | `0.x` 仅源代码迁移兼容；ELF 0.1/0.2 符号节点已分层，稳定版后才建立完整 ABI 承诺 |

标准资源可被成功解析不表示可被打开。`viOpen` 对没有已注册后端的合法资源返回 `VI_ERROR_NSUP_OPER`。Windows 交叉编译通过只证明可构建，不等同于 Windows 原生行为已验证。
