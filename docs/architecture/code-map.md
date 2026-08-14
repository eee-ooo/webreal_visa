# 第一方文件地图

本文件回答“这个文件为什么存在、改动会影响哪里”。覆盖所有第一方代码、构建、CI、工具和测试文件；文档之间的职责由 `AGENTS.md` 与需求文档定义。新增、移动或删除下列范围文件时必须同步本表。

## 构建与 CI

| 文件 | 职责 |
|---|---|
| `CMakeLists.txt` | 定义 C/C++20 工程、固定或本地 Asio 来源、可选 libusb 1.0.30 动态依赖、Prologix 生产源、对象层、共享/静态库、安装规则和全部 CTest 门禁。 |
| `cmake/FindLibUSB.cmake` | 查找 libusb 头和动态库，从 `LIBUSB_API_VERSION` 校验最低 1.0.30，并提供 `LibUSB::LibUSB` 导入目标。 |
| `cmake/webreal_visa.map` | ELF 共享库的 0.1–0.6 版本化导出白名单，防止 STL/内部 C++ 符号泄漏为非预期 ABI。 |
| `cmake/webreal_visa_abi.json` | 冻结 27 个 0.1–0.6 公共符号首次所属版本节点，作为独立于链接脚本的 ABI 历史基线。 |
| `cmake/webreal_visaConfig.cmake.in` | 安装后 CMake 包入口，恢复线程与启用构建所需的 libusb 动态依赖，并导入命名空间目标。 |
| `cmake/uninstall.cmake.in` | 依据 CMake 安装清单生成显式 `uninstall` 目标。 |
| `.github/workflows/build.yml` | 定义 Ubuntu 24.04 与固定 MSVC 2022 基线的 Windows 2022 Debug/Release、含 Prologix TCP/PTY 的 Release 轻量重复压力、安装后静态/共享消费，以及 GCC/MSVC Sanitizer 持续门禁；Linux 从校验归档构建动态 libusb 1.0.30，Windows 显式定位 ASan runtime。支持 push、pull request 和手工触发，远端首次运行前仍不能记为 CI 通过。 |

## 独立消费示例

| 文件 | 职责 |
|---|---|
| `examples/CMakeLists.txt` | 以独立 C 工程通过 `find_package` 分别消费已安装的静态与共享目标；Windows 将导入目标 DLL 复制到共享消费程序旁。 |
| `examples/mock_query.c` | 打开模拟资源并执行 `*IDN?`，验证安装包的头、链接和最小运行闭环。 |
| `examples/tcp_query.c` | 接收主机与端口，使用纯 C ABI 演示 raw TCP Socket 查询。 |

## 公共 ABI

| 文件 | 职责 |
|---|---|
| `include/visa.h` | VISA 兼容基础类型、标准数值、导出/调用约定和首批公共函数；是长期 ABI 边界。 |
| `include/webreal_visa_ext.h` | 0.6 项目版本、模拟资源、ASRL 路径、TCPIP 服务端口、资源 alias，以及带尺寸/版本/保留字段的 USB RAW/control 与 Prologix controller 配置扩展；使用 `wrvisa*`，不污染 `vi*` 命名空间。 |
| `include/webreal_visa_plugin.h` | 尺寸协商、版本化的插件/宿主 C ABI 契约；0.1 不包含加载器。 |

## API 外观

| 文件 | 职责 |
|---|---|
| `src/api/visa_api.cpp` | 实现全部首批 `vi*` 与项目扩展入口、可选输出、alias/RAW/Prologix 配置解析、版本化 USB control 校验、ABI 异常屏障、句柄/状态映射，并确定性选择模拟、TCP Socket、ASRL、VXI-11、HiSLIP 或 RM/注册表 GPIB/USB provider。 |

## 核心对象与访问控制

| 文件 | 职责 |
|---|---|
| `src/core/backend_session.h` | 后端会话的 I/O、清除、状态、触发、USB control 和远端锁能力接口及默认不支持行为，使 API/核心不依赖具体传输实现。 |
| `src/core/handle_table.h` | 声明对象类型、对象基类和类型化代际句柄表。 |
| `src/core/handle_table.cpp` | 句柄编码、短锁查表、删除、代际递增和耗尽退役实现。 |
| `src/core/objects.h` | 声明 RM、FindList、Session 的所有权、GPIB/USB 发现快照、RM 范围 GPIB provider/alias/传输覆盖/USB RAW 配置、属性、远端锁挂钩和 operation 注册接口。 |
| `src/core/objects.cpp` | 实现 GPIB/USB 发现快照、RM board provider 映射、大小写无关 alias 解析、RAW 配置快照、父子级联关闭、后端立即关闭、I/O/控制委派、取消等待、协议远端锁协调和资源清理。 |
| `src/core/lock_manager.h` | 声明按规范化资源隔离的进程内锁协调器及会话锁深度查询。 |
| `src/core/lock_manager.cpp` | 实现排他/共享锁、访问键、嵌套计数、超时、会话释放和远端锁边界所需深度判断。 |

## 资源层

| 文件 | 职责 |
|---|---|
| `src/resource/resource_parser.h` | 定义资源类别、TCPIP INSTR 协议选择及包含 GPIB 地址、TCPIP 主机/端口/设备名和 USB 身份的传输无关规范化描述。 |
| `src/resource/resource_parser.cpp` | 解析并规范化 ASRL/GPIB/TCPIP/USB 初始标准子集、模拟扩展与括号 IPv6 Socket，保留 GPIB 主/次地址，并按设备名确定性路由 VXI-11/HiSLIP。 |
| `src/resource/find_expression.h` | 声明可编译、可针对规范化资源及其静态属性匹配的 VPP 资源表达式对象。 |
| `src/resource/find_expression.cpp` | 自行解析资源正则及静态全局属性的逻辑/比较表达式；不暴露宿主正则方言，也不为查找隐式打开设备。 |

## 运行时与后端

| 文件 | 职责 |
|---|---|
| `src/runtime/operation.h` | 声明单调 deadline、原子一次完成状态和线程安全的后端取消 hook。 |
| `src/runtime/operation.cpp` | 实现成功、超时与取消竞争的单一原子提交点，并原子移交、只触发一次取消 hook。 |
| `src/runtime/io_runtime.h` | 声明进程共享、引用计数持有的有限线程 Asio I/O runtime。 |
| `src/runtime/io_runtime.cpp` | 创建并以进程生命周期强引用持有 2–4 个工作线程和 work guard，避免 worker 回调释放最后引用时自我 join。 |
| `src/backends/asio/async_stream.h` | 复用的异步字节流引擎：strand、读写队列、逐 operation 取消、绝对定时器与 read-ahead。 |
| `src/backends/asio/request_channel.h` | 声明消息协议共用的有界 request/response 通道、读帧回调、取消排空和会话关闭接口。 |
| `src/backends/asio/request_channel.cpp` | 实现共享 runtime 上的 TCP 连接、strand 请求队列、绝对 deadline、协议边界读取与取消后响应排空。 |
| `src/backends/gpib/gpib_provider.h` | 声明 GPIB provider、RAII 注册令牌、发现和按规范资源身份打开 transport 的内部契约。 |
| `src/backends/gpib/gpib_provider.cpp` | 实现线程安全 provider 快照、发现故障隔离、规范化去重、当前 provider 打开路由和最早可诊断错误保留；当前不注册生产 provider。 |
| `src/backends/gpib/prologix_provider.h` | 声明 Prologix 串口/TCP 配置值与创建 RM board provider 的内部入口，不把第三方类型暴露给通用 GPIB 层。 |
| `src/backends/gpib/prologix_provider.cpp` | 实现 Asio 串口/TCP 连接、Prologix 初始化/版本校验、数据转义、有界 EOT 排空、clear/trigger/spoll、端点身份共享、完整事务仲裁，以及超时/取消/协议错误后的失效重连。 |
| `src/backends/gpib/gpib_transport.h` | 定义不暴露 linux-gpib、NI-488.2 或 Prologix 类型的 GPIB 读写/EOI/send-end、clear、trigger、serial poll、传输预取缓存丢弃、取消和关闭契约。 |
| `src/backends/gpib/gpib_session.h` | 声明按半双工串行化的 GPIB 后端会话、能力快照、read-ahead 和 EOI 状态。 |
| `src/backends/gpib/gpib_session.cpp` | 实现 GPIB 读写、终止符/EOI/read-ahead、send-end、clear、会话与 transport 两层预取缓存的 flush、状态字、触发、超时/取消与失败不提交语义。 |
| `src/backends/hislip/hislip_protocol.h` | 声明 HiSLIP 1.x 消息类型、16 字节头、帧对象与有界编解码接口。 |
| `src/backends/hislip/hislip_protocol.cpp` | 实现网络字节序 HiSLIP 帧编码、头校验和协商长度上限检查。 |
| `src/backends/hislip/hislip_session.h` | 声明 HiSLIP 同步/异步双通道会话、协商状态、消息 ID、read-ahead 和控制能力。 |
| `src/backends/hislip/hislip_session.cpp` | 实现 HiSLIP 1.x 同步模式初始化、Data/DataEnd、状态字、清除、触发、远端锁和取消恢复。 |
| `src/backends/mock/mock_session.h` | 声明隔离的内存模拟后端。 |
| `src/backends/mock/mock_session.cpp` | 实现可阻塞读取、取消唤醒、回显、`*IDN?`、队列清除与状态字。 |
| `src/backends/tcp/tcp_session.h` | 声明 raw TCP Socket 后端及 TCP 专有属性边界。 |
| `src/backends/tcp/tcp_session.cpp` | 实现异步解析/连接 deadline、Socket 选项、流式读写与连接错误映射。 |
| `src/backends/vxi11/vxi11_protocol.h` | 声明 XDR 数据、ONC RPC v2 envelope、record marking、portmapper 与 VXI-11 常量。 |
| `src/backends/vxi11/vxi11_protocol.cpp` | 实现有界 XDR/RPC 编解码、record 构造与响应校验。 |
| `src/backends/vxi11/vxi11_session.h` | 声明 VXI-11 core/abort 双通道会话、link、事务 ID、分块和 read-ahead 状态。 |
| `src/backends/vxi11/vxi11_session.cpp` | 实现 portmapper、create/destroy link、分块读写、状态字、清除、触发、远端排他锁、abort 与响应排空。 |
| `src/backends/serial/serial_session.h` | 声明 ASRL 后端、串口配置状态和平台清除能力。 |
| `src/backends/serial/serial_session.cpp` | 实现本机串口打开、9600-8N1 默认配置、基础线路属性和清除/刷新，并复用公共平台串口路径转换。 |
| `src/backends/usb/libusb_provider.h` | 声明内建 libusb provider 注册入口，以及不依赖 libusb 头的 USBTMC/RAW 接口描述符校验、错误映射和可用性测试边界。 |
| `src/backends/usb/libusb_provider.cpp` | 实现可选 libusb 1.0.30 runtime、USBTMC/USB488 与 RAW 枚举/精确端点匹配、INSTR/RAW 共享 handle/claim、异步双向 bulk/control/interrupt transfer、安全多 transfer 取消、端点 gate 和热拔出连接失效；禁用构建提供明确 stub。 |
| `src/backends/usb/usb_provider.h` | 声明进程内 USB provider 注册/发现/打开契约，以及同一物理接口共享 claim 生命周期与拔出失效的仲裁器。 |
| `src/backends/usb/usb_provider.cpp` | 注册可用的内建 libusb provider，并实现 provider 快照与故障隔离、资源规范化去重、INSTR/RAW 打开路由、失败诊断延后和线程安全的接口 lease acquire/release/invalidate。 |
| `src/backends/usb/usb_transport.h` | 定义不暴露 libusb 类型的 USB interface/alternate setting/端点信息、bulk/control/interrupt、halt 清除、取消与关闭契约，供 USBTMC、USB RAW 和模拟 transport 复用。 |
| `src/backends/usb/usb_raw_session.h` | 声明由显式 alternate setting、读写传输类型和端点驱动的 USB RAW 后端会话。 |
| `src/backends/usb/usb_raw_session.cpp` | 实现 RAW bulk/interrupt 读写、终止符/read-ahead、端点零 control、halt 清除、取消成功竞争和失败不提交用户缓冲。 |
| `src/backends/usb/usbtmc_protocol.h` | 声明 USBTMC DEV_DEP/USB488 TRIGGER 消息、能力、class split transaction、状态字及有界编解码接口。 |
| `src/backends/usb/usbtmc_protocol.cpp` | 实现 USBTMC 小端消息头、反码、长度/对齐、能力位、clear/abort 状态和 USB488 状态标签的严格校验。 |
| `src/backends/usb/usbtmc_session.h` | 声明基于可替换 USB transport 的 USBTMC/USB488 会话、bTag、状态标签、能力缓存、read-ahead 与恢复状态。 |
| `src/backends/usb/usbtmc_session.cpp` | 实现 USBTMC 消息 I/O、短包/read-ahead、class clear/abort split transaction、取消/超时恢复，以及 USB488 状态字/触发/interrupt-IN。 |
| `src/platform/serial_discovery.h` | 声明当前平台串口发现与路径规范化接口。 |
| `src/platform/serial_discovery.cpp` | 实现 Linux/macOS `/dev` 发现、Windows COM 枚举和 COM10+ 本机路径转换。 |
| `src/platform/serial_path.h` | 提供 ASRL 与 Prologix 共用的轻量平台路径转换，包括 Windows COM10+ 命名。 |

## 测试

| 文件 | 职责 |
|---|---|
| `tests/test_support.h` | 无第三方依赖的失败即停测试断言。 |
| `tests/resource_tests.cpp` | 覆盖标准/扩展资源解析、规范化、VPP 资源正则及静态属性逻辑/比较表达式正反例。 |
| `tests/core_tests.cpp` | 覆盖过期/错误类型句柄、一次完成竞争和锁管理器。 |
| `tests/api_tests.cpp` | 覆盖 RM/查找/alias/可选输出/打开/属性/模拟读写/锁/状态/关闭端到端闭环，以及公共 API 的参数、句柄、模式、属性/缓冲、锁和项目扩展错误契约。 |
| `tests/concurrency_tests.cpp` | 覆盖超时、`viTerminate`、会话关闭和 RM 级联关闭对阻塞读取的竞争。 |
| `tests/tcp_tests.cpp` | 以本机 TCP 服务端覆盖连接/属性、终止符、read-ahead、部分超时回收、排队读写独立 deadline、批量取消、阻塞写取消和关闭。 |
| `tests/protocol_codec_tests.cpp` | 覆盖 XDR/RPC 固定向量与 reply 校验、HiSLIP 精确帧头，以及截断、超限 opaque、错误 XID/状态和畸形长度拒绝。 |
| `tests/gpib_tests.cpp` | 以仅存在于测试目标的 GPIB provider/transport，经公共 `vi*` 覆盖发现快照、故障隔离、地址路由、EOI/终止符/read-ahead、能力门禁、取消、超时和会话复用。 |
| `tests/prologix_tests.cpp` | 以生产 TCP 连接和 loopback 控制器，经公共 API 覆盖配置校验、初始化、共享仲裁、主次地址、转义、EOI/read-ahead、clear/trigger/spoll、队列 deadline、超限/取消失效和重连。 |
| `tests/prologix_serial_tests.cpp` | 在 Linux POSIX PTY 上经生产串口连接覆盖显式路径、9600-8N1 打开、初始化、转义写入和 EOT 读取。 |
| `tests/usb_tests.cpp` | 以脚本化 USB transport 覆盖 USBTMC/USB488 固定向量、合法对齐与畸形输入、class clear/abort、状态标签/interrupt-IN、取消/超时恢复、回退 clear、复用与不可恢复关闭。 |
| `tests/usb_raw_tests.cpp` | 以脚本化 transport 覆盖 RAW bulk/interrupt 读写、终止符/read-ahead、control IN/OUT、端点 halt、配置不匹配及不支持端点。 |
| `tests/usb_api_tests.cpp` | 以仅存在于测试目标的 USB provider/设备模拟器，经公共 `vi*` 覆盖发现快照、provider 故障隔离、INSTR/RAW 共享 claim、版本化配置/control 错误契约、USBTMC/USB488/RAW I/O、取消后复用、拔出和重连代际隔离。 |
| `tests/libusb_provider_tests.cpp` | 在启用和禁用构建中覆盖 USBTMC/USB488 与 RAW interface/alternate setting/端点校验、libusb 错误到 VISA 状态映射、内建发现结果规范化及不可见设备的可诊断打开结果。 |
| `tests/libusb_adapter_tests.cpp` | 以可控 libusb C API 模拟设备驱动生产适配器，经公共 `vi*` 覆盖 INSTR/RAW 枚举、共享 claim、异步 USBTMC/USB488、RAW bulk/interrupt/control、取消 callback、恢复复用、热拔出和最后 release/close。 |
| `tests/hislip_tests.cpp` | 以 loopback 双通道模拟器和公共 `vi*` 覆盖初始化、分帧 I/O、状态字、触发、共享/排他锁、清除、取消/超时恢复、复用、不支持模式和异步 fatal error 关闭。 |
| `tests/vxi11_tests.cpp` | 以 loopback portmapper/core/abort 模拟器和公共 `vi*` 覆盖分块 I/O、状态字、触发、远端锁、清除、取消/超时恢复、复用、缺失服务及畸形响应后的部分数据回收。 |
| `tests/serial_tests.cpp` | 在 POSIX PTY 上覆盖 ASRL 发现/显式映射、属性、双向 I/O、超时/取消复用、清除和刷新。 |
| `tests/c_header_smoke.c` | 用 C11 验证头文件、核心类型尺寸和最小链接调用。 |
| `tests/cpp_header_smoke.cpp` | 用 C++20 验证公共结构标准布局、常量与最小链接调用。 |

## 工程工具

| 文件 | 职责 |
|---|---|
| `tools/validate_docs.py` | 检查必需文档、相对链接、ADR 编号/标题、文件地图覆盖和版权发布阻塞。 |
| `tools/validate_abi_history.py` | 将版本脚本节点、公共头声明、扩展头版本与 CMake 工程版本同独立 JSON 历史清单精确比较，防止旧符号被删除或迁移节点。 |
| `tools/validate_exports.py` | 从 ELF 版本脚本读取公共 ABI 白名单，解析 Windows PE 导出表或调用 `readelf` 检查 ELF 动态导出集合。 |
