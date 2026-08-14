# webreal_visa

`webreal_visa` 是一个面向 Windows 与 Ubuntu 的 VISA 兼容实现。项目以 C ABI 作为长期兼容边界，以 C++20 实现内部核心；当前开发版本为 `0.6.0`。0.4 已支持隔离的模拟后端、raw TCP Socket、ASRL 串口、VXI-11、HiSLIP 1.x 同步模式、VPP 静态属性查找、RM 范围资源 alias 和 ABI 历史门禁。0.5 的五个无硬件 USB 切片已完成 USB `INSTR`/`RAW`、USBTMC/USB488、可选 libusb 1.0.30 生产适配器和共享接口仲裁。0.6 已建立通用 GPIB provider/transport，因 GPL 边界拒绝 linux-gpib 进程内集成，并新增显式配置的 Prologix GPIB-USB/GPIB-Ethernet 生产 Provider；其 TCP loopback 与 Linux POSIX PTY 路径已验证，当前 27 项 ABI 的 Linux CMake/CTest/Sanitizer、持续运行、安装消费与 ELF 门禁已完成，并在 Windows 11 原生通过 0.4–0.6 无硬件复验（Debug/Release 与 MSVC ASan 四套 19/19、PE 导出 27 项、安装消费 2/2）。NI-488.2、真实 USB/Prologix/GPIB 硬件和 Windows libusb/ASRL runtime 均为 `NOT_TESTED`，HiSLIP overlap/2.0/TLS 和厂商 VISA 也尚未实现。

当前状态、已验证能力与准确限制见 [`docs/status/current.md`](docs/status/current.md)，当前 0.6 Linux 软件门禁见 [`0.6 Linux CMake 收口记录`](docs/progress/2026-08-14-stage-0.6-linux-cmake-validation.md)，Windows 原生无硬件复验见 [`0.6 Windows 原生记录`](docs/progress/2026-08-14-stage-0.6-windows.md)；历史网络协议证据见 [`0.3 Linux 记录`](docs/progress/2026-08-11-stage-0.3-linux.md) 与 [`0.3 Windows 记录`](docs/progress/2026-08-12-stage-0.3-windows.md)，`0.4` 见 [`无硬件加固记录`](docs/progress/2026-08-12-stage-0.4-linux.md)。没有历史上下文的 AI 或开发者应先阅读 [`AGENTS.md`](AGENTS.md)。

## 构建

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

需要 CMake 3.25+、支持 C++20 的编译器，以及 C11 编译器。Linux 建议 GCC 13+ 或 Clang 17+；Windows 支持 Visual Studio 2022 或更新工具集，CI 使用当前 `windows-2025` 镜像。

默认配置按固定提交和 SHA-256 获取 standalone Asio 1.38.2。离线或已审计构建可传入源码目录，避免网络访问：

```sh
cmake -S . -B build -DWRVISA_ASIO_SOURCE_DIR=/path/to/asio/asio
```

生产 USB 适配器使用可替换的 libusb 1.0.30+ 动态库。`WRVISA_LIBUSB=AUTO`（默认）在找不到合格开发包时关闭适配器而不影响其他后端；需要强制启用或明确禁用时使用：

```sh
cmake -S . -B build -DWRVISA_LIBUSB=ON -DLibUSB_ROOT=/path/to/libusb-prefix
cmake -S . -B build-no-usb -DWRVISA_LIBUSB=OFF
```

安装后可使用 `find_package(webreal_visa 0.6 CONFIG REQUIRED)`，目标为 `webreal_visa::visa` 和 `webreal_visa::visa_static`。[`examples/mock_query.c`](examples/mock_query.c) 演示模拟设备，[`examples/tcp_query.c`](examples/tcp_query.c) 演示 raw TCP Socket；二者属于独立 C 消费工程。VXI-11 与 HiSLIP 使用标准 TCPIP `INSTR` 资源；非默认服务端口可通过 `wrvisaSetTcpipServicePort` 配置，RM 范围非持久化 alias 可通过 `wrvisaSetResourceAlias` 配置。

USB RAW 使用 [`webreal_visa_ext.h`](include/webreal_visa_ext.h) 中的版本化扩展：先在 RM 上以 `wrvisaSetUsbRawConfig` 固定 alternate setting 和读写端点，再用标准 `viOpen`、`viRead`、`viWrite` 操作该 `RAW` 资源；端点零请求使用 `wrvisaUsbControlTransfer`。配置只影响后续打开的会话，未配置的 RAW 打开返回 `VI_ERROR_INTF_NUM_NCONFIG`。这套接口不替代设备厂商初始化文档，真实设备使用前仍需核对端点和 control request。

## Prologix GPIB

标准 GPIB 资源名不携带控制器地址，因此必须先在 RM 上显式映射 board，再 `viOpen`。以下示例把 `GPIB7` 映射到官方 Ethernet 控制器的 TCP 端口；串口型号把连接类型改为 `WRVISA_PROLOGIX_CONNECTION_SERIAL`、`tcp_port` 设为 0，并将 endpoint 传为 `/dev/ttyUSB0` 或 Windows `COMx`。

```c
ViSession rm = VI_NULL;
ViSession instrument = VI_NULL;
wrvisa_prologix_controller_config_v1 config = {0};

config.struct_size = sizeof(config);
config.abi_major = WRVISA_PROLOGIX_ABI_MAJOR;
config.abi_minor = WRVISA_PROLOGIX_ABI_MINOR;
config.connection_type = WRVISA_PROLOGIX_CONNECTION_TCP;
config.tcp_port = 1234;
config.eot_char = 0x04;
config.read_timeout_ms = 500;
config.maximum_response_size = 1024 * 1024;

viOpenDefaultRM(&rm);
wrvisaSetPrologixController(rm, 7, "192.0.2.10", &config);
viOpen(rm, "GPIB7::5::INSTR", VI_NO_LOCK, 2000, &instrument);
```

配置不会扫描 GPIB 地址，`viFindRsrc` 也不会虚构仪器；调用方必须知道目标主/次地址。需要特别遵守三条边界：

- `eot_char` 必须选取预期响应不会包含的字节。Prologix 在设备到主机方向没有消歧转义，因此该路径不支持包含 EOT 字节的任意二进制响应。
- 同一控制器在所有 RM 中必须使用完全一致的 endpoint 字符串、连接类型、port、EOT、读取 timeout 和最大响应大小。DNS/IP 别名与串口符号链接不会自动归并，混用会绕过进程内仲裁。
- `read_timeout_ms` 是控制器 GPIB 空闲 timeout，范围 1–3000 ms；`VI_ATTR_TMO_VALUE` 仍是每次 VISA operation 的总 deadline。超时、取消或协议错误后连接会被丢弃并在下次操作完整初始化。

实现会先发送 `++savecfg 0`，避免运行时地址/EOI 切换反复写 EEPROM；同一显式端点的多会话在完整物理事务外仲裁。Windows 原生 `prologix_tests` 已覆盖 TCP 受控端点；跨进程独占、`GPIB::INTFC`、任意二进制透明读取、真实硬件以及 Windows libusb/ASRL/Prologix 串口 runtime 尚未验证。

纯 C 工具链优先链接共享目标；静态库内部由 C++ 实现，因此最终链接步骤仍需平台 C++ 运行时（示例通过 CMake 的 C++ linker 完成），但调用源码和 ABI 保持为 C。

## English summary

`webreal_visa` 0.6 is under development. It provides canonical GPIB addressing and replaceable provider/transport contracts, rejects in-process linux-gpib integration because of its GPL boundary, and now includes an explicitly configured Prologix serial/TCP production provider. The production paths are covered against a TCP loopback controller and a Linux POSIX PTY, including endpoint-wide arbitration and reconnect-after-failure behavior. The current 27-symbol build has passed the complete CMake/CTest/Sanitizer, stress, installation-consumer, and ELF ABI gates on Linux, and the 0.4–0.6 no-hardware scope was re-verified natively on Windows 11 (four CTest suites at 19/19, 27 PE exports, install consumption 2/2). Real Prologix/GPIB hardware, Windows libusb/ASRL runtime, NI-488.2, arbitrary binary responses containing the configured EOT byte, and cross-process arbitration remain `NOT_TESTED` or unsupported.

The project is not affiliated with, certified by, or endorsed by the IVI Foundation, NI, Keysight, or any other VISA vendor.

## 许可状态

项目目标许可为 MIT，但版权主体尚未确定。仓库因此暂不提供正式 `LICENSE` 文件，也不得对外发布。Asio 与 libusb 的第三方声明和许可证已单独随仓库保留，见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。具体边界见 [`docs/project/licensing.md`](docs/project/licensing.md)。

“VISA”用于描述接口兼容目标；本项目与 IVI Foundation、NI、Keysight 或其他 VISA 厂商不存在隶属、认证或背书关系。
