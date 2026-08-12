# 当前工程状态

更新时间：2026-08-12

当前版本：`0.4.0`。0.4 的 Linux 无硬件兼容性加固已完成：静态属性查找、RM 范围 alias、标准可选输出与 ABI 历史门禁均已验证。既有 0.3 Windows 原生网络/协议无硬件结果继续有效，但 0.4 新增 API 尚未在 Windows 原生重跑，状态为 `NOT_TESTED`；Windows ASRL runtime 与真实仪器互操作同样未验证。证据见 [`0.4 Linux 阶段报告`](../progress/2026-08-12-stage-0.4-linux.md) 与 [`0.3 Windows 阶段报告`](../progress/2026-08-12-stage-0.3-windows.md)。

## 现在可用

- 原有 21 个 `vi*` C ABI、模拟后端、资源解析/查找、代际句柄、统一 operation、进程内锁和 CMake 安装消费能力。
- `TCPIP[board]::host::port::SOCKET`：DNS/IPv4/括号 IPv6、连接超时、读写、终止符、read-ahead、逐 operation 取消和 TCP 基础属性。
- ASRL：Linux 设备发现/显式路径映射、9600-8N1 与基础线路属性、双向 I/O、清除、刷新、超时和取消复用。
- VXI-11 TCPIP `INSTR`：第一方 XDR/ONC RPC v2/record marking/portmapper，core+abort 通道、分块读写、状态字、清除、触发、远端排他锁、超时/取消恢复。
- HiSLIP 1.x TCPIP `INSTR`：同步模式、同步/异步双通道、最大消息长度、Data/DataEnd、状态字、清除、触发、远端共享/排他锁、超时/取消恢复。
- RM 范围项目扩展 `wrvisaSetTcpipServicePort`，用于给测试模拟器或自定义网关设置非默认 VXI-11 portmapper/HiSLIP 端口；生产默认仍为 111/4880。
- VPP `{attrExpr}` 静态属性查找子集：逻辑组合、数值比较、字符串等值，并对未知/局部/类型错误属性明确返回 `VI_ERROR_INV_EXPR`。
- RM 范围 `wrvisaSetResourceAlias`：进程内、非持久化、大小写无关；打开和两个 Parse 入口共享解析语义。Find/Parse 标准可选输出已补齐。
- 0.1–0.4 ABI 历史清单固定 24 个公共符号所属版本节点，并与 ELF/PE 精确导出门禁分离，避免白名单与产物同时漂移。
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
- USB、本机 GPIB、厂商 VISA、动态插件加载和异步 job API 未实现；ASRL 的 mark/space parity、DTR/DSR 流控和完整 VISA 串口属性仍不完整。
- macOS 构建/运行未验证。ThreadSanitizer 在当前容器因运行时内存映射不兼容而无法启动，不得记为通过。
- 当前 Linux 环境没有 Clang、Valgrind、clang-tidy/cppcheck，且无 sudo 非交互安装权限；本轮相应矩阵未执行，不得记为通过。GCC Sanitizer 不能替代 ThreadSanitizer 或不同编译器验证。
- 版权主体仍为 `[TBD_COPYRIGHT_HOLDER]`；正式项目 `LICENSE` 和对外发布被阻塞。Asio 第三方许可证与声明已随仓库保留。

## 下一步

保持 0.4 范围冻结，不自动进入 USB/GPIB/HiSLIP overlap/TLS。环境允许时先在 Windows 原生重跑 0.4 Debug/Release/ASan、24 项导出和静态/共享消费；硬件到位后再补真实 VXI-11/HiSLIP 与 Windows ASRL。开始 0.5 前必须获得新的范围授权。
