# webreal_visa

`webreal_visa` 是一个面向 Windows 与 Ubuntu 的 VISA 兼容实现。项目以 C ABI 作为长期兼容边界，以 C++20 实现内部核心；当前版本为 `0.4.0`，支持隔离的模拟后端、raw TCP Socket、ASRL 串口、VXI-11 与 HiSLIP 1.x 同步模式，并在 Linux 增加 VPP 静态属性查找、RM 范围资源 alias 和 ABI 历史门禁。USB、本机 GPIB、HiSLIP overlap/2.0/TLS 和厂商 VISA尚未实现。

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

安装后可使用 `find_package(webreal_visa 0.4 CONFIG REQUIRED)`，目标为 `webreal_visa::visa` 和 `webreal_visa::visa_static`。[`examples/mock_query.c`](examples/mock_query.c) 演示模拟设备，[`examples/tcp_query.c`](examples/tcp_query.c) 演示 raw TCP Socket；二者属于独立 C 消费工程。VXI-11 与 HiSLIP 使用标准 TCPIP `INSTR` 资源；非默认服务端口可通过 `wrvisaSetTcpipServicePort` 配置，RM 范围非持久化 alias 可通过 `wrvisaSetResourceAlias` 配置。

纯 C 工具链优先链接共享目标；静态库内部由 C++ 实现，因此最终链接步骤仍需平台 C++ 运行时（示例通过 CMake 的 C++ linker 完成），但调用源码和 ABI 保持为 C。

## English summary

`webreal_visa` 0.4 adds a bounded VPP static-attribute search subset, RM-scoped process-local resource aliases, optional VISA outputs, and a machine-checked 0.1–0.4 ABI history to the existing C-compatible mock, raw TCP, ASRL, VXI-11, and HiSLIP implementation. The 0.4 additions are Linux-verified; their Windows native rerun, Windows ASRL runtime, real-instrument interoperability, USB, native GPIB, HiSLIP overlap, and HiSLIP 2/TLS remain unverified or unsupported.

The project is not affiliated with, certified by, or endorsed by the IVI Foundation, NI, Keysight, or any other VISA vendor.

## 许可状态

项目目标许可为 MIT，但版权主体尚未确定。仓库因此暂不提供正式 `LICENSE` 文件，也不得对外发布。Asio 的第三方声明和许可证已单独随仓库保留，见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。具体边界见 [`docs/project/licensing.md`](docs/project/licensing.md)。

“VISA”用于描述接口兼容目标；本项目与 IVI Foundation、NI、Keysight 或其他 VISA 厂商不存在隶属、认证或背书关系。
