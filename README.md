# webreal_visa

`webreal_visa` 是一个面向 Windows 与 Ubuntu 的 VISA 兼容实现。项目以 C ABI 作为长期兼容边界，以 C++20 实现内部核心；当前开发版本为 `0.6.0`。0.4 已支持隔离的模拟后端、raw TCP Socket、ASRL 串口、VXI-11、HiSLIP 1.x 同步模式、VPP 静态属性查找、RM 范围资源 alias 和 ABI 历史门禁。0.5 的五个无硬件 USB 切片已完成 USB `INSTR`/`RAW` 资源模型、USBTMC/USB488、可替换传输/provider、可选 libusb 1.0.30 生产适配器、RAW bulk/interrupt/control、异步取消、热拔出映射与共享接口仲裁。0.6 的第一纵向切片已加入 GPIB 地址身份、可替换 provider/transport 和测试专用控制器公共 `vi*` 闭环；第二切片完成 linux-gpib 4.3.7 许可/API 评审，并因其 GPL 边界拒绝在 MIT 核心库中链接或 `dlopen`。NI-488.2 与 Prologix 仍未接入。真实 USB/GPIB 硬件均为 `NOT_TESTED`，HiSLIP overlap/2.0/TLS 和厂商 VISA 也尚未实现。

当前状态、已验证能力与准确限制见 [`docs/status/current.md`](docs/status/current.md)，历史网络协议证据见 [`0.3 Linux 记录`](docs/progress/2026-08-11-stage-0.3-linux.md) 与 [`0.3 Windows 记录`](docs/progress/2026-08-12-stage-0.3-windows.md)，`0.4` 见 [`无硬件加固记录`](docs/progress/2026-08-12-stage-0.4-linux.md)。没有历史上下文的 AI 或开发者应先阅读 [`AGENTS.md`](AGENTS.md)。

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

纯 C 工具链优先链接共享目标；静态库内部由 C++ 实现，因此最终链接步骤仍需平台 C++ 运行时（示例通过 CMake 的 C++ linker 完成），但调用源码和 ABI 保持为 C。

## English summary

`webreal_visa` 0.6 is under development. Its first GPIB slice adds canonical board/primary/secondary addressing, replaceable provider/transport contracts, EOI-aware I/O, optional clear/trigger/serial-poll capabilities, and a public-API loop through a test-only controller. The second slice reviewed linux-gpib 4.3.7 and rejected in-process linking or `dlopen` because its GPL boundary conflicts with this project's current MIT dependency policy. No NI-488.2 or Prologix production provider is included yet, and real GPIB hardware remains `NOT_TESTED`. The 0.5 USB baseline remains covered through controlled transport and libusb API simulators; real USB hardware is also `NOT_TESTED`.

The project is not affiliated with, certified by, or endorsed by the IVI Foundation, NI, Keysight, or any other VISA vendor.

## 许可状态

项目目标许可为 MIT，但版权主体尚未确定。仓库因此暂不提供正式 `LICENSE` 文件，也不得对外发布。Asio 与 libusb 的第三方声明和许可证已单独随仓库保留，见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。具体边界见 [`docs/project/licensing.md`](docs/project/licensing.md)。

“VISA”用于描述接口兼容目标；本项目与 IVI Foundation、NI、Keysight 或其他 VISA 厂商不存在隶属、认证或背书关系。
