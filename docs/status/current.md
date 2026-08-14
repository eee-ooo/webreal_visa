# 当前工程状态

更新时间：2026-08-14

当前开发版本：`0.6.0`。GPIB 第一切片已交付通用资源/provider/transport 与模拟公共 API 闭环；第二切片因 GPL 边界拒绝 linux-gpib 进程内集成。2026-08-14 第三切片完成显式配置的 Prologix 串口/TCP 生产 Provider，设计见 [`ADR-0012`](../decisions/0012-prologix-controller-boundary.md)，实现证据见 [`Prologix 切片记录`](../progress/2026-08-14-stage-0.6-prologix-slice.md)。当前生产代码已通过 Linux TCP loopback 与 POSIX PTY 受控端点验证，随后完成当前 27 项 ABI 的完整 Linux CMake/CTest/Sanitizer/安装消费收口，证据见 [`0.6 Linux CMake 收口记录`](../progress/2026-08-14-stage-0.6-linux-cmake-validation.md)；同日又在 Windows 11 原生完成 0.4–0.6 无硬件复验：Debug/Release 与 MSVC ASan Debug/Release 四套全量 CTest 均为 19/19，Release 高风险 13 用例各连续 25 轮通过，安装消费 2/2、`tcp_query` 本机回显 1/1，PE 导出 27 项且 ABI 6 节点门禁通过，证据见 [`0.6 Windows 原生记录`](../progress/2026-08-14-stage-0.6-windows.md)。真实 Prologix/GPIB 仪器、Windows libusb/ASRL runtime、真实 USB 与 macOS 继续保持 `NOT_TESTED`。

## 0.6 进行中

- 通用 `GpibProvider`/`GpibTransport`/`GpibBackendSession` 保持厂商无关，承载 board/主次地址、EOI/send-end、clear、trigger、serial poll、deadline、取消和 read-ahead；测试 provider 继续证明发现与能力门禁。
- linux-gpib 4.3.7 的技术映射仍保留，但用户态库、公开头和实现处于 GPL 边界且无链接例外；ADR-0011 禁止直接链接、延迟链接或 `dlopen`，Prologix 路径不依赖它。
- 新增 RM 范围 `wrvisaSetPrologixController` 和 40 字节 ABI v1 配置，将一个 GPIB board 显式映射到串口路径或 TCP host/port；不猜测 ASRL 编号、不扫描 GPIB 地址、不虚构发现资源。
- 21 个既有 `vi*` 与新增后的 6 个 `wrvisa*` 共 27 个导出；`wrvisaSetPrologixController` 固定在新的 `WRVISA_0.6` 节点，旧符号所属节点不变。
- 进程级池按连接类型、原样 endpoint 和 TCP port 共享 controller；同一身份的多个 RM/board/session 在完整地址/状态/I/O/排空事务外仲裁，冲突配置返回 `VI_ERROR_RSRC_BUSY`。别名不去重，部署必须统一 endpoint 拼写。
- 每次连接先发 `++savecfg 0`，再固定 controller mode、关闭 auto/EOS、配置 EOT/read timeout 并以 `++ver` 校验，避免运行时地址/EOI 切换磨损 EEPROM。
- 写入转义 CR/LF/ESC/`+`；读取先把整条响应有界排空到 EOT，再交给会话终止符/read-ahead。clear、trigger 和 serial poll 映射到官方命令。
- 排队等待服从 operation deadline；活动事务超时、取消、超限、协议或连接错误会关闭通道，下一事务重连并完整初始化，不复用未知状态。
- TCP 生产路径经 loopback 覆盖共享、主次地址、转义、EOI、控制命令、队列 timeout、超限/取消恢复；串口生产路径经 POSIX PTY 覆盖实际打开、初始化、写入和 EOT 读取。
- `GPIB[board]::INTFC`、控制器发现、endpoint 别名去重、跨进程仲裁和任意二进制响应未实现；单字节 EOT 若出现在仪器响应中无法消歧。Windows Prologix TCP 受控端点已随 `prologix_tests` 原生运行；真实硬件、Windows libusb/ASRL/Prologix 串口 runtime 仍为 `NOT_TESTED`。

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
- GPIB 除测试 provider 外，最终用户可通过 `wrvisaSetPrologixController` 显式配置 Prologix 串口/TCP 控制器，再以标准 `GPIB...::INSTR` 和既有 `vi*` 操作；适用性受 EOT、端点身份和 `NOT_TESTED` 边界约束。
- 0.1–0.6 ABI 历史清单固定 27 个公共符号所属版本节点，并与 ELF/PE 精确导出门禁分离，避免白名单与产物同时漂移。
- 进程共享的固定 Asio 1.38.2 I/O runtime、每通道 strand、有界请求队列、绝对 deadline 和协议响应排空/清除恢复；无每会话线程。

## Linux 已验证

- 0.6 第一切片在 GCC 13.3、libusb 启用时 Debug/Release 全量各 20/20，GCC ASan/UBSan/LSan（`detect_leaks=1`）20/20；显式关闭 libusb 的 Debug 为 19/19。`gpib_tests` 覆盖测试 provider 的公共 API 闭环、取消/超时和复用，并在 Release 连续 1000/1000 轮通过。
- 当前提交 `19b4242` 已在 Ubuntu 24.04.4 x86_64、GCC 13.3、CMake 3.28.3 和动态 libusb 1.0.30 下完成 0.6 Linux 收口：libusb 启用时 Debug/Release 各 22/22，显式关闭 libusb 的 Debug 为 21/21，GCC ASan/UBSan/LSan（`detect_leaks=1`）为 22/22；Release 的 `gpib_tests`、`prologix_tests`、`prologix_serial_tests` 各连续 1000/1000 轮通过。全新安装后的静态/共享纯 C 消费 2/2，安装产物动态链接 libusb，ELF 精确导出 27 项且 ABI 历史为 6 个节点。详见 [`0.6 Linux CMake 收口记录`](../progress/2026-08-14-stage-0.6-linux-cmake-validation.md)。
- 0.6 第三切片初始收口曾在 GCC 13.3 以 `-fPIC`、CMake 等价高告警和 `-Werror` 手工编译全部生产源；TCP loopback、POSIX PTY、通用 GPIB 与 C/C++ 头回归通过，通用 GPIB 与两个 Prologix 测试在 ASan/UBSan/LSan 下通过。ELF ABI、文档与 ABI 历史门禁确认 27 项/89 文件/6 节点；MinGW-w64 GCC 13 编译生产源并链接测试，PE 导出为 27 项。该手工证据现由上述完整 CMake 门禁补强，但交叉产物仍不能替代 Windows 原生 runtime。
- 0.6 第一切片的历史 Release 证据：全新安装后独立纯 C 静态/共享消费 2/2，当时 ELF ABI 历史与精确导出均为 26 项；MinGW-w64 GCC 13、`WRVISA_LIBUSB=OFF` 当时编译全部库和测试目标，PE 亦为 26 项。该证据早于第 27 个 Prologix 配置导出，且交叉产物未运行，不能替代当前安装消费或 Windows 原生证据。
- 0.4 GCC 13.3 Debug/Release 全量各 14/14；GCC ASan/UBSan/LSan（`detect_leaks=1`）14/14。属性表达式正反例、alias 三入口一致性、可选输出和 ABI 历史均包含在内。
- 0.4 Release 全新安装后静态/共享纯 C 消费 2/2；ELF 恰好导出 21 个 `vi*` 与三个 `wrvisa*`，共 24 项，0.1–0.4 版本节点历史一致。
- 0.4 `resource_tests` 与 `api_tests` 各连续 100 轮通过；CI 同款资源/API/并发/raw TCP/codec/HiSLIP/VXI-11/ASRL 八个用例各连续 25 轮通过。
- 以下为 0.3 阶段历史证据，保留用于说明传输回归强度：
- 阶段验收时 GCC 13.3 Debug/Release CTest 均为 12/12；加入跨平台 ABI 导出门禁后，2026-08-12 从当前未提交工作区重新配置、构建并复验 Release 为 13/13。覆盖资源、核心、API、并发、raw TCP、ASRL、协议向量、VXI-11 与 HiSLIP loopback 模拟器、C/C++ 头、文档和导出契约。
- 协议测试通过公共 `vi*` API 覆盖分帧/分块 I/O、终止符/read-ahead、状态字、触发、远端锁、清除、取消、超时、恢复后复用和畸形/不支持响应。
- 阶段验收 GCC ASan/UBSan/LSan 为 12/12；加入 ABI 门禁后再次以 `detect_leaks=1` 运行 ASan/UBSan 为 13/13。并发、raw TCP、ASRL、VXI-11 和 HiSLIP 在阶段验收 Release 下各连续 500 轮，API 错误契约和协议 codec 各连续 1000 轮，共 4500 次测试进程调用无首次失败；收口后 CI 同款 7 个高风险用例各连续 25 轮亦通过。
- gcov/gcovr 对第一方 `src/` 的当前基线为行 73.8%、函数 89.4%、分支 62.1%；新增公共 API 参数/句柄/属性/锁边界和 XDR/RPC/HiSLIP 截断、畸形长度测试后，行覆盖从 71.6% 提升，分支覆盖从 59.1% 提升。覆盖率是缺口导航，不作为兼容完成声明。
- Release 全新安装到临时前缀后，独立纯 C 源码消费工程的静态 `mock_query` 与共享 `mock_query_shared` 均构建并运行，2/2 通过；安装后的 ELF 动态导出仍严格只有 21 个 `vi*`、`wrvisaSetSerialPath` 和 `wrvisaSetTcpipServicePort`，0.1/0.2/0.3 版本节点继承正确。

## Windows 已验证（2026-08-14，无硬件）

- 基线 `19b4242`，VS 2022 Community（MSVC 19.44.35228 / x64 / SDK 10.0.26100）、CMake 3.28.1、Python 3.14.5，锁定 Asio 1.38.2 本地源码；本机无 libusb，`WRVISA_LIBUSB=AUTO` 关闭生产 USB 适配器（与 CI Windows 一致），`libusb_adapter_tests` 不构建，`serial_tests`/`prologix_serial_tests` 为 UNIX-only。
- Debug、Release 与 MSVC `/fsanitize=address`（Debug/Release 各一）四套 CTest 均为 19/19：16 个代码测试（资源/核心/API/并发/raw TCP/codec/GPIB/Prologix/USB 系列/HiSLIP/VXI-11/C/C++ 头）+ 文档/ABI 三个门禁；ASan 未报告地址类内存错误，本轮没有执行与 Linux LSan 等价的独立泄漏门禁。
- Release 稳定性：13 个高风险用例（CI 同款列表加入 `prologix_tests`）各连续 25 轮共 325 次测试进程调用无首次失败，总用时 89.63 秒。
- Release 安装到仓库外前缀：独立纯 C 静态 `mock_query` 与共享 `mock_query_shared` 消费 2/2；`tcp_query TCPIP0::127.0.0.1::45123::SOCKET` 连接本机回显服务器输出 `ACME Windows TCP INSTR`，1/1。
- `dumpbin /exports` 确认 DLL 恰好导出 27 项（21 个 `vi*` + 6 个 `wrvisa*`，含 `WRVISA_0.6` 节点 `wrvisaSetPrologixController`），与 `abi_exports`/`abi_history` 门禁一致。
- 本轮环境注意（非产品缺陷）：MSBuild `/m` 并行在本机静默失败需串行构建；默认 Anaconda Python 3.6.4 过老导致门禁脚本语法错误，改指 Python 3.14.5；安装前缀放构建树内会被 `validate_docs.py` 扫描到而报 broken link；MSVC ASan runtime DLL 需加入 PATH。详见 [`0.6 Windows 原生记录`](../progress/2026-08-14-stage-0.6-windows.md)。

## Windows 0.3 历史（2026-08-12，无硬件）

- VS 2022 Community（MSVC 19.44.35207 / x64 / SDK 10.0.26100），锁定 Asio 1.38.2 源码（SHA-256 一致）。Debug、Release 与 MSVC `/fsanitize=address`（Debug/Release 各一）四套 CTest 均为 11/11（`serial_tests` 为 UNIX-only 不构建）；ASan 未报告地址类内存错误，本轮没有执行与 Linux LSan 等价的独立泄漏门禁。
- Release 稳定性：`concurrency_tests`、`tcp_tests`、`vxi11_tests`、`hislip_tests` 各连续 500 轮，`api_tests`、`protocol_codec_tests` 各连续 1000 轮，共 4000 次测试进程调用无首次失败。
- Release 安装到临时前缀，独立 C 消费工程 `find_package(webreal_visa 0.3 CONFIG REQUIRED)` 构建成功；`mock_query` 1/1、`tcp_query`（本机回显服务器）1/1 运行通过。
- `dumpbin /exports` 确认 DLL 导出与 Linux `webreal_visa.map` 完全一致：21 个 `vi*` + `wrvisaSetSerialPath` + `wrvisaSetTcpipServicePort`。
- 发现并修复两个 Windows 专属问题，生产实现只改一行 `CMakeLists.txt`：① `tcp_tests` 的平台时序假设 —— Windows 上 `asio::async_connect` 对已关闭端口约 2s 才上报 refusal（同步 connect 即时），1000ms 打开超时先触发导致返回 `VI_ERROR_TMO`；测试在 `_WIN32` 下放宽打开超时至 5000ms 并保持 `VI_ERROR_RSRC_NFOUND` 断言。② 共享库导入库未安装 —— `install()` 缺 `ARCHIVE DESTINATION`，Windows 安装树没有 `webreal_visa.lib`，`find_package` 消费共享目标会链接失败；已补上（非 Windows 无副作用）。详情见 [`0.3 Windows 阶段报告`](../progress/2026-08-12-stage-0.3-windows.md)。

## 当前限制与发布阻塞

- 0.4–0.6 新增 alias/API、属性查找、USB 模拟系列、GPIB/Prologix 与当前 27 项 PE 导出和安装消费已于 2026-08-14 在 Windows 11 原生无硬件复验通过（四套 19/19）；Windows libusb runtime（本机无 libusb）、Windows ASRL runtime（`serial_tests` UNIX-only）与真实 USB/GPIB 硬件仍为 `NOT_TESTED`。
- Windows 原生网络/协议无硬件门禁已完成；Windows ASRL runtime、真实串口电气行为和真实仪器互操作仍为 `NOT_TESTED`。任何交叉构建只能证明可编译，不能替代 runtime 结果。
- VXI-11/HiSLIP 只与仓库内受控 loopback 模拟器互操作；第三方真实仪器、不同厂商错误行为和网络异常组合为 `NOT_TESTED`。
- HiSLIP 当前只实现 1.x 同步模式；overlap、HiSLIP 2、TLS/加密和生产 DNS-SD/mDNS 发现未实现。初始化发送的 `WR` vendor ID 是临时项目值，尚未按 IVI VPP-9 注册，不能宣称正式互操作认证。
- VXI-11 远端协议只有排他锁；VISA 共享锁仍只协调当前进程。跨进程锁、完整属性过滤、持久化系统 alias/完整资源类型和稳定版二进制兼容承诺未实现。
- USB 0.5 的五个无硬件代码切片已经完成，但不得被描述为真实 USB 硬件验证；真实 USBTMC/USB488/RAW 与 Windows libusb runtime 仍为 `NOT_TESTED`。GPIB 0.6 已有受限 Prologix 生产路径，Windows 原生门禁已覆盖通用 GPIB 和 Prologix TCP 受控端点；但真实控制器/仪器、Windows Prologix 串口 runtime、EOT 冲突响应、endpoint 别名与跨进程并发仍未验证或不支持；linux-gpib 继续被许可边界拒绝，NI-488.2、`INTFC` 会话、厂商 VISA、动态插件加载和异步 job API 未实现。ASRL 的 mark/space parity、DTR/DSR 流控和完整 VISA 串口属性仍不完整。
- macOS 构建/运行未验证。ThreadSanitizer 在当前容器因运行时内存映射不兼容而无法启动，不得记为通过。
- 当前 Linux 环境没有 Clang、Valgrind、clang-tidy/cppcheck，且无 sudo 非交互安装权限；本轮相应矩阵未执行，不得记为通过。GCC Sanitizer 不能替代 ThreadSanitizer 或不同编译器验证。
- 版权主体仍为 `[TBD_COPYRIGHT_HOLDER]`；正式项目 `LICENSE` 和对外发布被阻塞。Asio 与 libusb 第三方许可证及声明已随仓库保留。

## 下一步

Linux 与 Windows 的 0.6 当前无硬件软件范围均已通过完整 CMake/CTest、持续运行、安装消费与 ELF/PE/ABI 门禁；内存安全门禁分别为 Linux GCC ASan/UBSan/LSan 和 Windows MSVC ASan。下一阶段优先取得一台真实 Prologix 控制器和已知响应的 GPIB 仪器，验证串口/TCP、主次地址、EOI、clear/trigger/spoll、超时恢复及部署端点规则；Windows 侧待具备条件后补 libusb runtime 与真实串口 runtime。真实 USB 设备和远端 CI 证据仍按可用条件补齐。NI-488.2 保持为后续 Windows 专属独立切片，不与本轮混合。
