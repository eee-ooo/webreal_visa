# webreal_visa

`webreal_visa` 是一个面向 Windows 与 Ubuntu 的 VISA 兼容实现。项目以 C ABI 作为长期兼容边界，以 C++20 实现内部核心；当前版本为 `0.2.0`，支持隔离的模拟后端、真实 raw TCP Socket，以及 ASRL 串口纵向切片。TCPIP `INSTR`、USB、GPIB、HiSLIP、VXI-11 和厂商 VISA 尚未实现。

当前状态、已验证能力与准确限制见 [`docs/status/current.md`](docs/status/current.md)，`0.2` 实施记录见 [`docs/progress/2026-08-11-stage-0.2.md`](docs/progress/2026-08-11-stage-0.2.md)。没有历史上下文的 AI 或开发者应先阅读 [`AGENTS.md`](AGENTS.md)。

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

安装后可使用 `find_package(webreal_visa 0.2 CONFIG REQUIRED)`，目标为 `webreal_visa::visa` 和 `webreal_visa::visa_static`。[`examples/mock_query.c`](examples/mock_query.c) 演示模拟设备，[`examples/tcp_query.c`](examples/tcp_query.c) 演示 raw TCP Socket；二者属于独立 C 消费工程。

纯 C 工具链优先链接共享目标；静态库内部由 C++ 实现，因此最终链接步骤仍需平台 C++ 运行时（示例通过 CMake 的 C++ linker 完成），但调用源码和 ABI 保持为 C。

## English summary

`webreal_visa` 0.2 provides a C-compatible VISA core with a C++20 implementation, an isolated in-memory mock resource, real raw TCP Socket sessions, and an ASRL serial slice. TCPIP INSTR, USB, GPIB, HiSLIP, and VXI-11 remain intentionally unsupported. See `docs/status/current.md` for the authoritative scope and platform verification state.

The project is not affiliated with, certified by, or endorsed by the IVI Foundation, NI, Keysight, or any other VISA vendor.

## 许可状态

项目目标许可为 MIT，但版权主体尚未确定。仓库因此暂不提供正式 `LICENSE` 文件，也不得对外发布。Asio 的第三方声明和许可证已单独随仓库保留，见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。具体边界见 [`docs/project/licensing.md`](docs/project/licensing.md)。

“VISA”用于描述接口兼容目标；本项目与 IVI Foundation、NI、Keysight 或其他 VISA 厂商不存在隶属、认证或背书关系。
