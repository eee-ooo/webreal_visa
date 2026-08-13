# 当前工程状态

更新时间：2026-08-13

当前开发版本：`0.5.0`。用户于 2026-08-13 授权进入 USB 阶段；USB 资源/协议、provider/公共 API、class clear/abort 与 USB488、生产 libusb 适配，以及版本化 USB RAW 五个无硬件切片已完成，证据见 [`切片 1`](../progress/2026-08-13-stage-0.5-usb-slice-1.md)、[`切片 2`](../progress/2026-08-13-stage-0.5-usb-slice-2.md)、[`切片 3`](../progress/2026-08-13-stage-0.5-usb-slice-3.md)、[`切片 4`](../progress/2026-08-13-stage-0.5-usb-slice-4.md) 和 [`切片 5`](../progress/2026-08-13-stage-0.5-usb-slice-5.md)。0.5 的无硬件代码范围已收口；真实 USB 硬件验证仍无法进行，因此阶段保持进行中。0.4 的 Linux 无硬件兼容性加固是既有基线；既有 0.3 Windows 原生网络/协议结果继续有效，但 0.4/0.5 新增能力尚未在 Windows 原生重跑。USB 边界见 [`ADR-0009`](../decisions/0009-usb-transport-and-libusb-boundary.md)。

## 0.5 进行中

- USB 资源描述符现在保留 VISA board、VID、PID、序列号与 USB interface，并把标准 `INSTR` 和事实扩展 `RAW` 分开建模。
- `UsbTransport` 将协议状态机与 libusb/平台句柄隔离；USBTMC 实现 DEV_DEP 消息、读取请求、bTag、EOM、终止符、read-ahead、合法短包对齐和失败部分数据回收。
- `UsbProvider` 注册表把规范化发现和打开路由接入 RM/公共 `vi*`；每个 RM 保存创建时快照，`UsbInterfaceArbiter` 为同一连接代次共享 claim 并在最后关闭或拔出时只 release 一次。
- RM 范围 `wrvisaSetUsbRawConfig` 以 32 字节、`struct_size` + ABI 1.0 + 零保留字段结构冻结 RAW alternate setting 与读写端点；未配置的 RAW 明确返回 `VI_ERROR_INTF_NUM_NCONFIG`，已打开会话不受后续配置修改影响。
- `UsbRawBackendSession` 已将配置后的 bulk/interrupt 端点接入标准 `viRead`/`viWrite`、终止符/read-ahead、halt clear、取消和拔出语义；`wrvisaUsbControlTransfer` 以独立 `wrvisa*` 扩展承载端点零 IN/OUT，不污染标准 `vi*` 命名空间。
- USBTMC class clear 与 bulk-IN/bulk-OUT abort 使用规范 split transaction；取消/超时后先恢复传输边界，必要时回退完整 clear，双重恢复失败则关闭会话，避免旧响应污染复用。
- USB488 已实现 GET_CAPABILITIES、TRIGGER、READ_STATUS_BYTE 的 control/interrupt-IN 两种返回路径、SRQ/迟到标签过滤和 interrupt FIFO busy 映射。
- 可选 libusb 1.0.30 生产适配器已接入：`WRVISA_LIBUSB=AUTO/ON/OFF` 控制探测，合格动态库存在时分别发现 USBTMC/USB488 `INSTR` 与 interface 级 `RAW`，读取规范身份、按 VID/PID/serial/interface/alternate setting/端点打开，并让同一物理接口的 INSTR/RAW 会话共享一次 claim；无依赖构建仍保留完整协议测试和明确不支持结果。
- libusb transfer 全部使用异步 submit/callback；绝对 operation deadline 下推为 transfer timeout，取消后等待 completion callback 才释放 transfer/缓冲。bulk-OUT、bulk-IN、control、interrupt-IN/OUT 按端点方向串行，并由连接级 active 集合安全跟踪并发 transfer。
- 第一个物理 handle 启动进程级 libusb 事件线程，最后一个关闭后停止；热插拔回调只标记到达/离开和连接代次，不做描述符或同步 I/O。拔出使活动 transfer 映射为 `VI_ERROR_CONN_LOST`，旧会话不会因同身份设备重连而复活。
- 测试专用 USB 设备经公共 API 覆盖发现、八线程并发打开、USBTMC I/O/控制、取消后同会话复用、拔出和重连；模拟设备逻辑未进入生产库。
- Linux GCC 13.3 在 libusb 启用时 Debug/Release 全量各 19/19，ASan/UBSan/LSan 19/19；显式关闭 libusb 的构建为 18/18。
- Release `usb_tests`、`usb_raw_tests` 与 `usb_api_tests` 各连续 1000 轮；`libusb_provider_tests` 连续 1000 轮、包含 INSTR/RAW 的完整适配器公共 API 闭环连续 100 轮通过。额外 5000 轮 `usb_api_tests` 捕获到测试以“operation 已登记”近似“transport 已阻塞”的调度窗口；改为模拟设备显式确认 bulk-IN 已挂起后 5000/5000 通过。前一切片捕获的“无数据立即空 EOM”测试设备时序模型也保持已修正。
- 生产适配器已对官方 libusb 1.0.30 动态库真实编译/链接；适配器 API 测试覆盖 INSTR/RAW 枚举、打开、共享 claim、USBTMC/USB488、RAW bulk/interrupt/control、异步取消恢复、拔出和最后关闭，但设备端仍是受控模拟，不能外推为硬件通过。
- Release 全新安装后的纯 C 静态/共享消费 2/2；CMake 以 `$<LINK_ONLY:LibUSB::LibUSB>` 恢复动态依赖而不暴露其头文件，公共导出严格为既有 24 项加 0.5 两个 `wrvisa*` RAW 扩展，共 26 项。
- MinGW-w64 GCC 13 Release、`WRVISA_LIBUSB=OFF` 已交叉编译全部库和测试目标，PE 精确导出为 26 项；该结果只证明 Windows 编译/ABI 形状，不替代 MSVC、Windows libusb 或运行时测试。
- 本机枚举没有发现可验证的 USBTMC 目标；真实设备、驱动/权限组合和 Windows libusb runtime 均为 `NOT_TESTED`，0.5 不能标记完成。

## 现在可用

- 原有 21 个 `vi*` C ABI、模拟后端、资源解析/查找、代际句柄、统一 operation、进程内锁和 CMake 安装消费能力。
- `TCPIP[board]::host::port::SOCKET`：DNS/IPv4/括号 IPv6、连接超时、读写、终止符、read-ahead、逐 operation 取消和 TCP 基础属性。
- ASRL：Linux 设备发现/显式路径映射、9600-8N1 与基础线路属性、双向 I/O、清除、刷新、超时和取消复用。
- VXI-11 TCPIP `INSTR`：第一方 XDR/ONC RPC v2/record marking/portmapper，core+abort 通道、分块读写、状态字、清除、触发、远端排他锁、超时/取消恢复。
- HiSLIP 1.x TCPIP `INSTR`：同步模式、同步/异步双通道、最大消息长度、Data/DataEnd、状态字、清除、触发、远端共享/排他锁、超时/取消恢复。
- RM 范围项目扩展 `wrvisaSetTcpipServicePort`，用于给测试模拟器或自定义网关设置非默认 VXI-11 portmapper/HiSLIP 端口；生产默认仍为 111/4880。
- VPP `{attrExpr}` 静态属性查找子集：逻辑组合、数值比较、字符串等值，并对未知/局部/类型错误属性明确返回 `VI_ERROR_INV_EXPR`。
- RM 范围 `wrvisaSetResourceAlias`：进程内、非持久化、大小写无关；打开和两个 Parse 入口共享解析语义。Find/Parse 标准可选输出已补齐。
- RM 范围 `wrvisaSetUsbRawConfig` 与 RAW 会话扩展：显式 bulk/interrupt 端点、标准同步读写、端点零 control、取消与共享 claim；Linux 无硬件模拟闭环已验证，真实设备未验证。
- 0.1–0.5 ABI 历史清单固定 26 个公共符号所属版本节点，并与 ELF/PE 精确导出门禁分离，避免白名单与产物同时漂移。
- 进程共享的固定 Asio 1.38.2 I/O runtime、每通道 strand、有界请求队列、绝对 deadline 和协议响应排空/清除恢复；无每会话线程。

## Linux 已验证

- 0.4 GCC 13.3 Debug/Release 全量各 14/14；GCC ASan/UBSan/LSan（`detect_leaks=1`）14/14。属性表达式正反例、alias 三入口一致性、可选输出和 ABI 历史均包含在内。
- 0.4 Release 全新安装后静态/共享纯 C 消费 2/2；ELF 恰好导出 21 个 `vi*` 与三个 `wrvisa*`，共 24 项，0.1–0.4 版本节点历史一致。
- 0.4 `resource_tests` 与 `api_tests` 各连续 100 轮通过；CI 同款资源/API/并发/raw TCP/codec/HiSLIP/VXI-11/ASRL 八个用例各连续 25 轮通过。
- 以下为 0.3 阶段历史证据，保留用于说明传输回归强度：
- 阶段验收时 GCC 13.3 Debug/Release CTest 均为 12/12；加入跨平台 ABI 导出门禁后，2026-08-12 从当前未提交工作区重新配置、构建并复验 Release 为 13/13。覆盖资源、核心、API、并发、raw TCP、ASRL、协议向量、VXI-11 与 HiSLIP loopback 模拟器、C/C++ 头、文档和导出契约。
- 协议测试通过公共 `vi*` API 覆盖分帧/分块 I/O、终止符/read-ahead、状态字、触发、远端锁、清除、取消、超时、恢复后复用和畸形/不支持响应。
- 阶段验收 GCC ASan/UBSan/LSan 为 12/12；加入 ABI 门禁后再次以 `detect_leaks=1` 运行 ASan/UBSan 为 13/13。并发、raw TCP、ASRL、VXI-11 和 HiSLIP 在阶段验收 Release 下各连续 500 轮，API 错误契约和协议 codec 各连续 1000 轮，共 4500 次测试进程调用无首次失败；收口后 CI 同款 7 个高风险用例各连续 25 轮亦通过。
- gcov/gcovr 对第一方 `src/` 的当前基线为行 73.8%、函数 89.4%、分支 62.1%；新增公共 API 参数/句柄/属性/锁边界和 XDR/RPC/HiSLIP 截断、畸形长度测试后，行覆盖从 71.6% 提升，分支覆盖从 59.1% 提升。覆盖率是缺口导航，不作为兼容完成声明。
- Release 全新安装到临时前缀后，独立纯 C 源码消费工程的静态 `mock_query` 与共享 `mock_query_shared` 均构建并运行，2/2 通过；安装后的 ELF 动态导出仍严格只有 21 个 `vi*`、`wrvisaSetSerialPath` 和 `wrvisaSetTcpipServicePort`，0.1/0.2/0.3 版本节点继承正确。

## Windows 已验证（2026-08-12，无硬件）

- VS 2022 Community（MSVC 19.44.35207 / x64 / SDK 10.0.26100），锁定 Asio 1.38.2 源码（SHA-256 一致）。Debug、Release 与 MSVC `/fsanitize=address`（Debug/Release 各一）四套 CTest 均为 11/11（`serial_tests` 为 UNIX-only 不构建）；ASan 未报告地址类内存错误，本轮没有执行与 Linux LSan 等价的独立泄漏门禁。
- Release 稳定性：`concurrency_tests`、`tcp_tests`、`vxi11_tests`、`hislip_tests` 各连续 500 轮，`api_tests`、`protocol_codec_tests` 各连续 1000 轮，共 4000 次测试进程调用无首次失败。
- Release 安装到临时前缀，独立 C 消费工程 `find_package(webreal_visa 0.3 CONFIG REQUIRED)` 构建成功；`mock_query` 1/1、`tcp_query`（本机回显服务器）1/1 运行通过。
- `dumpbin /exports` 确认 DLL 导出与 Linux `webreal_visa.map` 完全一致：21 个 `vi*` + `wrvisaSetSerialPath` + `wrvisaSetTcpipServicePort`。
- 发现并修复两个 Windows 专属问题，生产实现只改一行 `CMakeLists.txt`：① `tcp_tests` 的平台时序假设 —— Windows 上 `asio::async_connect` 对已关闭端口约 2s 才上报 refusal（同步 connect 即时），1000ms 打开超时先触发导致返回 `VI_ERROR_TMO`；测试在 `_WIN32` 下放宽打开超时至 5000ms 并保持 `VI_ERROR_RSRC_NFOUND` 断言。② 共享库导入库未安装 —— `install()` 缺 `ARCHIVE DESTINATION`，Windows 安装树没有 `webreal_visa.lib`，`find_package` 消费共享目标会链接失败；已补上（非 Windows 无副作用）。详情见 [`0.3 Windows 阶段报告`](../progress/2026-08-12-stage-0.3-windows.md)。

## 当前限制与发布阻塞

- 0.4 新增 alias API、属性查找、可选输出、24 项 PE 导出和安装消费尚未在 Windows 原生重跑，状态为 `NOT_TESTED`；旧 0.3 Windows DLL 的 23 项结果不能替代本轮验证。
- Windows 原生网络/协议无硬件门禁已完成；Windows ASRL runtime、真实串口电气行为和真实仪器互操作仍为 `NOT_TESTED`。任何交叉构建只能证明可编译，不能替代 runtime 结果。
- VXI-11/HiSLIP 只与仓库内受控 loopback 模拟器互操作；第三方真实仪器、不同厂商错误行为和网络异常组合为 `NOT_TESTED`。
- HiSLIP 当前只实现 1.x 同步模式；overlap、HiSLIP 2、TLS/加密和生产 DNS-SD/mDNS 发现未实现。初始化发送的 `WR` vendor ID 是临时项目值，尚未按 IVI VPP-9 注册，不能宣称正式互操作认证。
- VXI-11 远端协议只有排他锁；VISA 共享锁仍只协调当前进程。跨进程锁、完整属性过滤、持久化系统 alias/完整资源类型和稳定版二进制兼容承诺未实现。
- USB 0.5 的五个无硬件代码切片已经完成，但不得被描述为真实 USB 硬件验证；真实 USBTMC/USB488/RAW 的枚举、驱动 detach、权限、claim、端点和厂商初始化组合仍为 `NOT_TESTED`。本机 GPIB、厂商 VISA、动态插件加载和异步 job API 未实现；ASRL 的 mark/space parity、DTR/DSR 流控和完整 VISA 串口属性仍不完整。
- macOS 构建/运行未验证。ThreadSanitizer 在当前容器因运行时内存映射不兼容而无法启动，不得记为通过。
- 当前 Linux 环境没有 Clang、Valgrind、clang-tidy/cppcheck，且无 sudo 非交互安装权限；本轮相应矩阵未执行，不得记为通过。GCC Sanitizer 不能替代 ThreadSanitizer 或不同编译器验证。
- 版权主体仍为 `[TBD_COPYRIGHT_HOLDER]`；正式项目 `LICENSE` 和对外发布被阻塞。Asio 与 libusb 第三方许可证及声明已随仓库保留。

## 下一步

0.5 不再新增无硬件 USB 功能，下一步是验证收口：环境允许时在 Windows 原生重跑 0.4/0.5 新增 API、libusb 模拟适配器、MSVC ASan、安装消费和 26 项 PE 导出；硬件到位后再按独立矩阵记录 Linux/Windows 的驱动、权限、枚举、claim、USBTMC/USB488 与 RAW 实际结果。保持 GPIB、HiSLIP overlap/2.0/TLS 与通用动态插件加载在本轮范围之外。
