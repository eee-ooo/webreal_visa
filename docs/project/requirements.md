# webreal_visa 项目需求（中文权威版）

状态：阶段 0 基线，已授权并适用于 `0.2`。本文件由父目录主提示词整理导入；进入仓库后，本文件是需求权威源。英文内容仅作辅助，冲突时以中文为准。

## 1. 项目目标

实现一个可独立构建、可测试、面向 Windows 与 Ubuntu 的 VISA 兼容库。公共边界采用稳定 C ABI，内部采用 C++20。`0.1` 建立可证明的核心闭环；`0.2` 在同一 operation 与后端边界上增加真实 raw TCP Socket 和 ASRL 串口纵向切片。

项目名与 CMake 命名空间为 `webreal_visa`。项目扩展使用 `wrvisa_` 函数前缀、`WRVISA_` 宏前缀；插件符号使用 `wrvisa_plugin_` 前缀，禁止占用 `vi*` 标准命名空间表达非标准能力。

## 2. 当前授权范围：阶段 0、0.1 与 0.2

阶段 0 必须交付：

- 自包含的 AI 接续文档、需求基线、架构说明、文件地图、来源台账、兼容矩阵和 ADR。
- 标准、协议、开源许可和版本证据的可追溯记录。
- CMake 3.25+ 工程、Windows/Ubuntu 构建门禁定义、C/C++ 公共头编译检查。
- 依赖与许可证边界；正式发布因版权主体未定而阻塞。

`0.1` 必须交付：

- 公共 C ABI 与共享库/静态库构建定义。
- VISA 资源字符串解析和规范化，标准子集与项目扩展明确分层。
- 类型化、带代际的句柄表；资源管理器、查找列表和会话生命周期。
- 同步 C API 外观之下的统一 operation/deadline/cancellation 模型。
- 隔离的 `WRVISA0::MOCK::INSTR` 模拟后端，用于端到端测试。
- 最小属性、状态码、进程内锁、读写、清除、刷新和终止能力。
- 单元、集成、并发取消、ABI、C/C++ 头文件和文档一致性测试。

`0.2` 必须交付：

- `TCPIP[board]::host::port::SOCKET` 的真实 DNS/地址解析、带 deadline 的连接、读写、终止符、read-ahead、超时、取消与立即关闭。
- ASRL 的本机串口发现、稳定接口编号、本机路径显式映射、9600-8N1 默认值、基础线路属性、读写、清除、刷新、超时、取消与立即关闭。
- 进程共享的 Asio I/O runtime；会话 strand 隔离；每个 operation 独立取消，禁止一个会话的超时误取消其他会话。
- TCP 本机服务器和 POSIX PTY 集成测试；Linux 原生门禁与 Windows 构建门禁。未在真实 Windows 运行时必须标为 `NOT_TESTED`，不得由交叉编译推断通过。
- 固定 Asio 版本、提交、下载校验值、许可证文本和第三方声明；允许用本地已审计源码目录完成离线构建。

本轮必须在 `0.2` 停止。不得实现 TCPIP `INSTR`、VXI-11、HiSLIP、USB、GPIB、厂商 VISA 适配器、异步 job API 或动态插件加载。

## 3. 标准与兼容基线

权威 VISA 基线为 IVI Foundation VPP-4.3 Revision 7.2.1（2024-01-04）。研究 HTML 只提供线索，不替代规范原文。

首批公共 API 基线：

- 资源管理：`viOpenDefaultRM`、`viGetDefaultRM`、`viFindRsrc`、`viFindNext`、`viOpen`、`viParseRsrc`、`viParseRsrcEx`、`viClose`。
- I/O：`viRead`、`viWrite`、`viReadSTB`、`viClear`、`viFlush`。
- 属性与缓冲：`viSetAttribute`、`viGetAttribute`、`viSetBuf`。
- 诊断和控制：`viStatusDesc`、`viLock`、`viUnlock`、`viAssertTrigger`、`viTerminate`。

初始标准资源语法：

- `ASRL[board][::INSTR]`
- `GPIB[board]::primary[::secondary][::INSTR]`
- `GPIB[board]::INTFC`
- `TCPIP[board]::host[::LAN device name][::INSTR]`
- `TCPIP[board]::host::port::SOCKET`
- `USB[board]::0xVVVV::0xPPPP::serial[::interface][::INSTR]`

USB RAW 只能标记为事实扩展（`DE_FACTO_EXTENSION`），不得描述为 VPP 标准。`WRVISA0::MOCK::INSTR` 是测试专用项目扩展（`PROJECT_EXTENSION`），不得被通用 `?*INSTR` 默认发现；测试只有显式使用 `WRVISA_MOCK_FIND_EXPRESSION` 才枚举它。

查找表达式遵循 VPP-4.3 定义的语法，而不是直接暴露 ECMAScript、PCRE 或 shell glob。`0.1` 实现资源表达式部分；`0.2` 将本机发现的 ASRL 资源加入普通发现集合。属性过滤表达式在兼容矩阵中明确列为未实现。

## 4. ABI、版本与可见性

公共 ABI 只包含固定宽度 VISA 类型、常量和 `extern "C"` 函数。禁止在 ABI 中暴露 STL、C++ 异常、RTTI 对象、编译器私有布局或所有权不明的指针。

符号默认隐藏，只导出公共 API。平台调用约定、导入/导出宏、结构体尺寸和保留字段必须显式定义。公共头必须分别由 C11 与 C++20 翻译单元编译。`0.x` 允许源代码兼容迁移；首个稳定版后以二进制兼容为主要边界。

插件 ABI 必须版本化并包含 `struct_size`、ABI 主/次版本、能力位与宿主回调表。插件只在受控加载点载入一次，运行期不卸载；`0.1` 仅冻结契约，不实现动态加载。

## 5. 运行时与并发不变量

- 每个句柄编码对象类型、槽位和代际；代际耗尽的槽位永久退役，避免 ABA 回绕。
- 关闭先使句柄不可再获取，再取消操作并清理对象。重复关闭和错误类型返回 `VI_ERROR_INV_OBJECT`。
- 同步 API 建立在统一 operation 模型上；operation 只有一个最终状态，正常完成、超时、取消或关闭只能有一个胜者。
- 超时使用单调时钟的绝对 deadline，禁止因重试重置预算。
- `viTerminate(..., VI_NULL)` 和 `viClose` 能唤醒阻塞 I/O；取消后不再写入用户缓冲区。
- TCP/串口操作经共享 I/O runtime 调度，并在会话 strand 上串行修改传输状态；读、写各自排队，不以每会话线程数换取并发。
- 超时或取消读取到的部分字节必须回收到会话 read-ahead，当前失败调用返回零字节，后续读取仍可取得这些数据。
- 锁协调不能依赖 I/O 粗粒度全局锁。`0.1` 提供进程内排他/共享锁语义；同主机跨进程锁是后续兼容门禁，当前必须如实标记。
- 诊断上下文不得依赖一个无隔离的全局“最后错误”。

## 6. 真实传输与后续技术边界

`0.2` 已按能力接口采用 standalone Asio 实现 TCP/串口，并把依赖作为私有、固定版本的头文件依赖。后续候选边界为 libusb（USB）；VXI-11 使用项目自有最小 ONC RPC/XDR；TLS 经可替换 provider 接口。任何新增依赖都需固定版本、校验来源、记录许可证并由 ADR 决定。

必须为未来的异步 API、批处理、设备发现、后端插件、跨进程锁、系统/厂商 VISA 共存与多协议留出兼容空间，但不得以未验证占位行为伪装成已支持。

## 7. 文档与 AI 维护契约

采用“中心文档 + 复杂目录局部 README”混合方式，避免一个巨型易冲突文件，也避免每个目录重复维护：

- `AGENTS.md`：最短接续入口。
- `docs/status/current.md`：唯一当前状态快照。
- 本文件：唯一需求权威源。
- `docs/architecture/overview.md`：结构和不变量。
- `docs/architecture/code-map.md`：每个第一方代码、构建和测试文件职责。
- `docs/decisions/`：不可变 ADR 历史。
- `docs/progress/`：阶段验收报告。
- `docs/research/sources.md`：来源、版本、文件、许可和采用情况。
- `docs/compatibility/`：标准与平台兼容性。

任何阶段结束时必须同步状态、文件地图、兼容矩阵与阶段报告。`tools/validate_docs.py` 是一致性门禁。

## 8. 许可与发布

项目计划采用 MIT，但版权主体为 `[TBD_COPYRIGHT_HOLDER]`，所以当前不生成正式 `LICENSE`、不打发布包、不对外发布。第三方代码默认不复制；参考实现只用于理解思想，必须记录仓库、版本或提交、具体文件、许可证以及“仅参考/采用/复制修改”的关系。

强 copyleft 代码可以作为独立工具或协议理解材料，但不得链接、复制或派生进入计划采用 MIT 的库。LGPL 依赖必须以可替换动态边界和合规发布流程接入。`0.2` 使用 Boost Software License 1.0 的 standalone Asio 私有头文件依赖；版本、校验值与许可证必须随仓库记录。

## 9. 完成定义

只有在实现、自动测试、兼容说明、文件地图和阶段报告一致，且 Linux 与真实 Windows 门禁均实际运行时，才能标记 `0.2` 完成。仅有交叉编译的平台必须写成 `NOT_TESTED`，不得推断通过；这种情况下可以标记“实现完成、平台验收待完成”，但不能宣称整个阶段已验收。
