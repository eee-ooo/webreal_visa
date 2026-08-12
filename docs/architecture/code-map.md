# 第一方文件地图

本文件回答“这个文件为什么存在、改动会影响哪里”。覆盖所有第一方代码、构建、CI、工具和测试文件；文档之间的职责由 `AGENTS.md` 与需求文档定义。新增、移动或删除下列范围文件时必须同步本表。

## 构建与 CI

| 文件 | 职责 |
|---|---|
| `CMakeLists.txt` | 定义 C/C++20 工程、固定或本地 Asio 来源、对象层、共享/静态库、安装规则和全部 CTest 门禁。 |
| `cmake/webreal_visa.map` | ELF 共享库的 0.1–0.4 版本化导出白名单，防止 STL/内部 C++ 符号泄漏为非预期 ABI。 |
| `cmake/webreal_visa_abi.json` | 冻结 0.1–0.4 公共符号首次所属版本节点与当前工程版本，作为独立于链接脚本的 ABI 历史基线。 |
| `cmake/webreal_visaConfig.cmake.in` | 安装后 CMake 包入口，恢复线程依赖并导入命名空间目标。 |
| `cmake/uninstall.cmake.in` | 依据 CMake 安装清单生成显式 `uninstall` 目标。 |
| `.github/workflows/build.yml` | 定义 Ubuntu 24.04 与 Windows 2025 的 Debug/Release、Release 轻量重复压力、安装后静态/共享消费，以及 GCC/MSVC Sanitizer 持续门禁。 |

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
| `include/webreal_visa_ext.h` | 项目版本、模拟资源、ASRL 路径、TCPIP 服务端口覆盖与 RM 范围资源 alias 扩展；使用 `wrvisa*`，不污染 `vi*` 命名空间。 |
| `include/webreal_visa_plugin.h` | 尺寸协商、版本化的插件/宿主 C ABI 契约；0.1 不包含加载器。 |

## API 外观

| 文件 | 职责 |
|---|---|
| `src/api/visa_api.cpp` | 实现全部首批 `vi*` 与项目扩展入口、可选输出、alias 统一解析、ABI 异常屏障、句柄/状态映射，并确定性选择模拟、TCP Socket、ASRL、VXI-11 或 HiSLIP 后端。 |

## 核心对象与访问控制

| 文件 | 职责 |
|---|---|
| `src/core/backend_session.h` | 后端会话的 I/O、清除、状态、触发和远端锁能力接口及默认不支持行为，使 API/核心不依赖具体传输实现。 |
| `src/core/handle_table.h` | 声明对象类型、对象基类和类型化代际句柄表。 |
| `src/core/handle_table.cpp` | 句柄编码、短锁查表、删除、代际递增和耗尽退役实现。 |
| `src/core/objects.h` | 声明 RM、FindList、Session 的所有权、RM 范围 alias/传输覆盖、属性、远端锁挂钩和 operation 注册接口。 |
| `src/core/objects.cpp` | 实现大小写无关 alias 解析、父子级联关闭、后端立即关闭、I/O/控制委派、取消等待、协议远端锁协调和资源清理。 |
| `src/core/lock_manager.h` | 声明按规范化资源隔离的进程内锁协调器及会话锁深度查询。 |
| `src/core/lock_manager.cpp` | 实现排他/共享锁、访问键、嵌套计数、超时、会话释放和远端锁边界所需深度判断。 |

## 资源层

| 文件 | 职责 |
|---|---|
| `src/resource/resource_parser.h` | 定义资源类别、TCPIP INSTR 协议选择及包含主机、端口、设备名的传输无关规范化描述。 |
| `src/resource/resource_parser.cpp` | 解析并规范化 ASRL/GPIB/TCPIP/USB 初始标准子集、模拟扩展与括号 IPv6 Socket，并按设备名确定性路由 VXI-11/HiSLIP。 |
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
| `src/backends/serial/serial_session.cpp` | 实现本机串口打开、9600-8N1 默认配置、基础线路属性和清除/刷新。 |
| `src/platform/serial_discovery.h` | 声明当前平台串口发现与路径规范化接口。 |
| `src/platform/serial_discovery.cpp` | 实现 Linux/macOS `/dev` 发现、Windows COM 枚举和 COM10+ 本机路径转换。 |

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
