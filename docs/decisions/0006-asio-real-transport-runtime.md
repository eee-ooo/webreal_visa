# ADR-0006：Asio 真实传输与共享 I/O 运行时

状态：Accepted（2026-08-11）

## 背景

`0.2` 需要在 Windows、Linux 和 macOS 上实现 TCP Socket 与串口，同时保持同步 C API 下的超时、逐操作取消、关闭竞态和未来异步扩展。直接维护 BSD socket、termios 与 WinSock/Overlapped COM 三套事件循环会重复平台细节；每会话独占线程又会使大量会话的资源成本线性增长。

## 决策

采用 standalone Asio 1.38.2，固定到提交 `8806a6803cde7054c3049d3666d3ec36786568c5` 和带 SHA-256 的源码归档。Asio 以私有、仅头文件依赖接入，使用 Boost Software License 1.0，不出现在公共头或安装后的链接接口中。

真实流后端共享一个进程内 `io_context` 小型线程池；每个会话拥有独立 strand、读队列、写队列、接收暂存区和逐操作 cancellation slot。公共 `viRead`/`viWrite` 仍同步等待；内部异步操作与 `steady_clock` 绝对 deadline 竞争同一个 `Operation` 完成状态。取消只作用于发出取消时已存在的 operation，不能通过关闭整个传输误伤后续调用。

CMake 默认以 `FetchContent` 获取并校验依赖；离线或受控供应链环境通过 `WRVISA_ASIO_SOURCE_DIR` 指向已审核源码树。

## 后果

TCP、串口以及后续 HiSLIP/VXI-11 可以复用同一取消和执行模型，不需要改变 C ABI。线程数不会随会话数线性增长，会话间仍由 strand 隔离。构建首次需要取得固定 Asio 源码，发布材料必须携带第三方声明；依赖升级必须新建 ADR 并重跑 Windows/Linux/macOS 验证。

## 被否决方案

- 分别手写 POSIX poll、WinSock Overlapped 和 Win32 COM：平台代码和取消竞态面过大。
- 每个会话启动一个永久 I/O 线程：实现简单，但无法合理扩展到大量会话。
- 把 Asio 类型暴露到插件或公共 C ABI：会把第三方版本和 C++ ABI 固化为兼容负担。
