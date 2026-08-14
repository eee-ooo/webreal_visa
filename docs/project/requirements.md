# webreal_visa 项目需求（中文权威版）

状态：阶段 0 基线，`0.5` 五个无硬件 USB 切片（含生产 libusb 适配器与 USB RAW）已经完成并建立 Git 基线。用户于 2026-08-13 授权按推荐进入 `0.6` GPIB 阶段；第一切片交付资源/控制器契约和模拟公共 API 闭环，第二切片因 GPL 边界拒绝 linux-gpib 进程内 Provider。2026-08-14 的第三切片依据 Prologix 官方串口/TCP 命令协议实现显式配置的生产 Provider，并以 Linux TCP loopback 与 POSIX PTY 验证无硬件路径；真实 Prologix/GPIB 仪器、Windows 0.4–0.6 原生重跑、真实 USB、远端 CI 首次运行与 macOS 仍保持 `NOT_TESTED`。本文件由父目录主提示词整理导入；进入仓库后，本文件是需求权威源。英文内容仅作辅助，冲突时以中文为准。

## 1. 项目目标

实现一个可独立构建、可测试、面向 Windows 与 Ubuntu 的 VISA 兼容库。公共边界采用稳定 C ABI，内部采用 C++20。`0.1` 建立可证明的核心闭环；`0.2` 增加真实 raw TCP Socket 和 ASRL；`0.3` 增加 VXI-11 与 HiSLIP 1.x 同步模式，并在 Linux 与 Windows原生 loopback 环境验证网络/协议纵向切片；`0.4` 在不增加硬件协议的前提下补齐资源查找、别名和 ABI 回归门禁。

项目名与 CMake 命名空间为 `webreal_visa`。项目扩展使用 `wrvisa_` 函数前缀、`WRVISA_` 宏前缀；插件符号使用 `wrvisa_plugin_` 前缀，禁止占用 `vi*` 标准命名空间表达非标准能力。

## 2. 当前授权范围：阶段 0 至 0.6 GPIB

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

`0.3` Linux 侧必须交付：

- `TCPIP[board]::host[::LAN device name][::INSTR]` 的确定性路由：`hislip...` 使用 HiSLIP，省略设备名或以 `vxi`、`gpib`、`inst` 开头时使用 VXI-11；未知设备名明确返回不支持，不以连接失败静默切换。
- 自有最小 ONC RPC v2/XDR/record-marking/portmapper 客户端，以及 VXI-11 core/abort 通道、分块读写、状态字、清除、触发、远端排他锁、超时、取消和取消后复用。
- HiSLIP 1.x 同步模式的同步数据与异步控制双通道、初始化、消息 ID、最大消息长度、Data/DataEnd、状态字、清除、触发、远端共享/排他锁、超时、取消和协议恢复。
- 对帧头、长度和 XDR/RPC 边界做分配前上限校验；取消后排空当前协议响应或执行标准清除握手，不能把残留响应交给下一个 operation。
- 只在测试路径实现 loopback VXI-11/HiSLIP 协议模拟器，覆盖协议向量、分帧、锁、取消、超时、故障注入和会话复用；生产库不得包含仪器端服务器。
- Linux Debug/Release、ASan/UBSan、压力、安装消费和 ELF ABI 门禁；模拟器通过不代表真实仪器互操作通过。

`0.3` Windows 原生无硬件网络/协议验证必须交付：

- MSVC x64 Debug/Release 原生构建与运行，同一套 raw TCP、VXI-11、HiSLIP loopback、公共 API、并发、协议 codec、C/C++ 头和文档测试。
- MSVC AddressSanitizer Debug/Release、Release 轻量持续压力、PE 导出白名单、安装后的静态与共享 C 消费目标。
- Windows ASRL 代码参加编译，但在没有可控虚拟串口或真实串口时，Windows ASRL runtime 仍为 `NOT_TESTED`；不得由编译或 Linux PTY 结果推断通过。
- 真实 VXI-11/HiSLIP 仪器、不同厂商异常行为和真实串口电气行为继续为 `NOT_TESTED`；loopback 结果只证明受控客户端闭环。

`0.4` 必须交付：

- `viFindRsrc` 支持 VPP-4.3 `regularExpr ['{' attrExpr '}']` 的确定性子集：`&&`、`||`、`!`、括号、数值六种比较与字符串相等/不等；只允许无需打开设备即可得到的 `VI_ATTR_INTF_TYPE`、`VI_ATTR_INTF_NUM`、`VI_ATTR_RSRC_CLASS`、`VI_ATTR_RSRC_NAME` 和 ASRL 默认 `VI_ATTR_ASRL_BAUD`。未知属性、类型不匹配、局部属性或畸形表达式返回 `VI_ERROR_INV_EXPR`。
- 修正标准可选输出行为：`viFindRsrc` 的 find-list 与 count 可为 `VI_NULL`；`viParseRsrcEx` 的资源类、展开名和 alias 输出可为 `VI_NULL`，且不得因此泄漏查找句柄。
- 增加 RM 范围、进程内、非持久化的 `wrvisaSetResourceAlias` 项目扩展；alias 禁止冒号和资源名歧义，大小写不敏感且可覆盖。`viOpen`、`viParseRsrc`、`viParseRsrcEx` 必须一致解析，展开名仍返回 canonical resource，alias 输出确定性返回已配置名字。
- 增加不可由当前导出脚本自身改写而漂移的 0.1–0.4 ABI 历史清单，检查符号所属版本节点；继续保持 C/C++ 头、共享/静态安装消费、ELF/PE 精确导出和 Sanitizer 门禁。
- 复用并增强 raw TCP、VXI-11、HiSLIP 的断线、畸形帧、超时、取消、响应排空和恢复复用故障测试；受控模拟器结论仍不得外推为真实仪器互操作。

`0.5` 必须交付：

- 扩展资源描述符，区分标准 USB `INSTR` 与事实扩展 USB `RAW`，保留 VID、PID、序列号、接口号及 VISA board；解析、规范化、发现与打开使用同一身份字段。
- 定义不依赖 libusb 类型的 USB 传输契约，覆盖接口 claim/release、bulk/control/interrupt transfer、逐 operation 取消、断开和关闭；USBTMC/USB488 状态机不得直接依赖 libusb。
- 实现 USBTMC bulk-OUT/bulk-IN 头、bTag、反码、传输长度、EOM、终止符、四字节填充和分包/短包校验；实现 clear/abort、状态字和触发所需的 USBTMC/USB488 控制路径。
- 实现通用 USB RAW 的 bulk/interrupt/control 能力和显式端点/接口配置扩展；厂商初始化逻辑只能通过 USB 专用、版本化能力契约接入，不得占用标准 `vi*` 命名空间。
- libusb 采用可选、可替换的系统动态依赖边界。缺少开发包、运行库、可用驱动或权限时，基础库和模拟测试仍可构建，显式打开应返回可诊断的未配置/未找到/权限状态，不能伪造成功。
- 无硬件门禁必须覆盖协议固定向量、畸形长度、bTag 错配、短包、终止符/read-ahead、取消、超时、拔出、接口共享和恢复复用；硬件不存在时准确标记 `NOT_TESTED`，不得把模拟器结果外推为硬件验证。
- 五个纵向切片依次交付资源/传输/USBTMC 基础、provider/公共 API、class clear/abort 与 USB488 控制、可选 libusb 枚举/打开/异步传输，以及版本化 USB RAW bulk/interrupt/control 的无硬件验证。五个代码切片均已交付；真实硬件验证未完成前 `0.5` 保持进行中。

`0.6` 第一纵向切片必须交付：

- 扩展传输无关资源描述符，分别保存 GPIB board、主地址、可选次地址和 `INSTR`/`INTFC` 身份；解析、规范化、发现和打开复用同一组字段。
- 定义不依赖 linux-gpib、NI-488.2 或 Prologix 类型的 GPIB provider/transport 契约，明确 EOI、device clear、trigger、serial poll、取消、断开与关闭能力。
- 只在测试目标注册模拟 provider，经公共 `viFindRsrc`、`viOpen`、`viRead`、`viWrite` 打通可取消、可超时的最小纵向闭环；模拟资源不得作为真实控制器或硬件通过证据。
- 不新增公共 C ABI；26 个既有导出及其 0.1–0.5 所属节点保持不变。后续只有在标准资源字符串无法表达必要的控制器映射时，才评审版本化 `wrvisa*` 配置扩展。

`0.6` 第二纵向切片（linux-gpib 许可闸门）必须交付：

- 固定官方稳定发布、归档校验值、用户态许可证和所查 API 文件，禁止以非官方镜像替代许可结论。
- 评审 board/主次地址、EOI、异步读写、线程局部状态、离散 timeout、`ibwait`/`ibstop` 取消、clear/trigger/serial poll、错误映射与 descriptor 关闭语义。
- 先通过项目强 copyleft 依赖政策；只有许可边界满足时才允许增加可注入 C API 模拟器和生产 Provider。许可不满足时必须停在通用 `GpibProvider`/`GpibTransport` 契约，不添加不可发布或伪装可用的实现。
- 当前评审确认 linux-gpib 4.3.7 用户态库、公开头和实现均在 GPL 边界且没有链接例外，因此直接链接、延迟链接和 `dlopen` 均不采用；本切片不增加生产代码、公共 ABI 或构建依赖。
- 未来只有取得兼容链接例外、替代许可或商业授权，或另行批准具有独立许可/分发边界的进程外方案，才可通过新 ADR 重开实现。

`0.6` 第三纵向切片（Prologix）必须交付：

- 只依据官方 GPIB-USB 6.0 与 GPIB-Ethernet 手册实现命令协议，不链接、复制或动态加载 linux-gpib；固定来源 URL、下载校验值和采用关系。
- 以版本化 RM 范围 `wrvisaSetPrologixController` 显式映射 GPIB board 到串口路径或 TCP host/port；不得从 ASRL 编号、环境变量或扫描结果猜测控制器。
- 对同一原样 endpoint 身份共享一个进程内 controller，在完整地址选择、状态切换、写入或响应排空事务外仲裁；冲突配置返回资源忙，endpoint 别名不作物理去重并必须在部署中统一。
- 连接或恢复时先发送 `++savecfg 0`，固定 controller/auto/EOS/EOT/read timeout 状态并验证 `++ver`；写数据转义 CR/LF/ESC/`+`，读取在释放事务前排空到 EOT 并受最大响应大小限制。
- 超时、取消、协议越界和连接错误使通道失效；下一事务必须重连并完整初始化。不得在未知控制器状态上继续，也不得虚构 GPIB 发现资源或实现未定义的 `INTFC` 会话。
- 公共契约必须明确单字节 EOT 与仪器数据无法无歧义共存，不能宣称任意二进制透明；Linux TCP loopback 和 POSIX PTY 覆盖共享仲裁、队列 deadline、取消恢复与串口打开，真实控制器/仪器和 Windows runtime 保持 `NOT_TESTED`。

本轮必须保持在 `0.6` GPIB 范围内。不得把 GPL linux-gpib 通过动态加载伪装成无依赖实现，也不得顺带接入 NI-488.2、HiSLIP overlap、HiSLIP 2/TLS、通用动态插件加载、异步 job API、持久化系统配置或生产级网络发现。

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

查找表达式遵循 VPP-4.3 定义的语法，而不是直接暴露 ECMAScript、PCRE 或 shell glob。`0.1` 实现资源正则表达式部分；`0.2` 将本机发现的 ASRL 资源加入普通发现集合；`0.4` 实现上述静态全局属性过滤子集。未列入 0.4 白名单的属性不得通过隐式打开设备求值。

## 4. ABI、版本与可见性

公共 ABI 只包含固定宽度 VISA 类型、常量和 `extern "C"` 函数。禁止在 ABI 中暴露 STL、C++ 异常、RTTI 对象、编译器私有布局或所有权不明的指针。

符号默认隐藏，只导出公共 API。平台调用约定、导入/导出宏、结构体尺寸和保留字段必须显式定义。公共头必须分别由 C11 与 C++20 翻译单元编译。`0.x` 允许源代码兼容迁移，但 0.1 起已经发布到版本节点的函数不得被静默删除或迁移节点；`0.4` 起以机器可读历史清单持续检查。首个稳定版后以二进制兼容为主要边界。

插件 ABI 必须版本化并包含 `struct_size`、ABI 主/次版本、能力位与宿主回调表。插件只在受控加载点载入一次，运行期不卸载；`0.1` 仅冻结契约，不实现动态加载。

## 5. 运行时与并发不变量

- 每个句柄编码对象类型、槽位和代际；代际耗尽的槽位永久退役，避免 ABA 回绕。
- 关闭先使句柄不可再获取，再取消操作并清理对象。重复关闭和错误类型返回 `VI_ERROR_INV_OBJECT`。
- 同步 API 建立在统一 operation 模型上；operation 只有一个最终状态，正常完成、超时、取消或关闭只能有一个胜者。
- 超时使用单调时钟的绝对 deadline，禁止因重试重置预算。
- `viTerminate(..., VI_NULL)` 和 `viClose` 能唤醒阻塞 I/O；取消后不再写入用户缓冲区。
- TCP、串口和消息协议通道经共享 I/O runtime 调度，并在会话 strand 上串行修改传输状态；不以每会话线程数换取并发。
- 超时或取消读取到的部分字节必须回收到会话 read-ahead，当前失败调用返回零字节，后续读取仍可取得这些数据。
- VXI-11/HiSLIP 的超时或取消必须在协议边界完成响应排空或异步清除恢复，确保后续请求不会错配旧响应。
- 锁协调不能依赖 I/O 粗粒度全局锁。先取得进程内锁，再请求远端协议锁；远端失败必须回滚本地状态。VXI-11 远端只提供排他锁，VISA 共享锁仍只在当前进程协调；HiSLIP 远端支持共享与排他锁。
- 诊断上下文不得依赖一个无隔离的全局“最后错误”。

## 6. 真实传输与后续技术边界

`0.2` 已按能力接口采用 standalone Asio 实现 TCP/串口；`0.3` 复用同一共享 runtime 和第一方有界 request channel，实现项目自有最小 ONC RPC/XDR/VXI-11 与 HiSLIP 编解码和状态机，没有引入新的第三方库。`0.5` 已按 ADR-0009 采用可选 libusb 1.0.30 动态边界实现生产 USB 适配器；TLS 未来经可替换 provider 接口。任何新增依赖都需固定版本、校验来源、记录许可证并由 ADR 决定。

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

每完成一个以 `0.1` 为步长的开发版本迭代（例如 `0.5.x` 计划代码范围收口并进入 `0.6.x` 前），必须在阶段文档同步且当前环境适用的构建与测试门禁通过后，创建一次对应的 Git 提交，用于固定该版本的可追溯基线。因环境或设备缺失而无法执行的硬件、平台验证必须准确保留为 `NOT_TESTED`；这些缺口不阻止版本基线提交，但该提交不得被描述为对应平台或硬件已经验收通过。

## 8. 许可与发布

项目计划采用 MIT，但版权主体为 `[TBD_COPYRIGHT_HOLDER]`，所以当前不生成正式 `LICENSE`、不打发布包、不对外发布。第三方代码默认不复制；参考实现只用于理解思想，必须记录仓库、版本或提交、具体文件、许可证以及“仅参考/采用/复制修改”的关系。

强 copyleft 代码可以作为独立工具或协议理解材料，但不得链接、复制或派生进入计划采用 MIT 的库。LGPL 依赖必须以可替换动态边界和合规发布流程接入。`0.2`/`0.3` 使用 Boost Software License 1.0 的 standalone Asio 私有头文件依赖；版本、校验值与许可证必须随仓库记录。`0.3` 协议实现为基于公开规范编写的第一方代码。

## 9. 完成定义

只有在实现、自动测试、兼容说明、文件地图和阶段报告一致时，才能标记相应平台范围完成。`0.4` 只有在 Linux Debug/Release、Sanitizer、属性表达式正反例、alias 三入口一致性、可选输出、ABI 历史、精确导出与安装消费通过后，才可标记“Linux 无硬件兼容性加固完成”。`0.5` 的无硬件切片必须同时证明依赖启用/禁用构建、USBTMC/USB488 与 RAW、异步 callback 取消、热拔出、压力、安装消费、许可材料和 ABI 不漂移；这些证据仍不能替代真实 USB 硬件。`0.6` 第一切片必须证明通用 GPIB 契约，第三切片还必须证明 Prologix 配置 ABI、端点级仲裁、EEPROM 保护、命令转义、完整响应排空、大小上限、排队 deadline、取消/错误后的重连初始化，以及 TCP/PTY 无硬件路径；这些证据仍不能替代真实 Prologix 控制器或总线仪器。既有 Windows 0.3 网络/协议结果继续有效，但 0.4–0.6 新增能力与门禁在 Windows 原生重新运行前必须写成 `NOT_TESTED`。Windows ASRL/runtime、真实仪器互操作、远端 CI 与 macOS 在取得对应证据前仍不得推断通过。
