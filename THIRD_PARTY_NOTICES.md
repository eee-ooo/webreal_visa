# 第三方组件声明 / Third-Party Notices

## standalone Asio

- 上游：`chriskohlhoff/asio`
- 版本：Asio 1.38.2
- 固定提交：`8806a6803cde7054c3049d3666d3ec36786568c5`
- 获取归档 SHA-256：`ca7f6c14f2bf91e61c7e81fb693f2f8fc86f93e85520d5fc7fd035d0f666bb35`
- 许可证：Boost Software License 1.0
- 用途：私有、仅头文件的 TCP/串口异步 I/O 实现依赖；不进入公共 C ABI。
- 修改：未修改上游源码；默认由 CMake `FetchContent` 获取，也可通过 `WRVISA_ASIO_SOURCE_DIR` 使用预先审核的源码树。

许可证全文见 [`third_party/asio/LICENSE_1_0.txt`](third_party/asio/LICENSE_1_0.txt)。

## libusb

- 上游：`libusb/libusb`
- 版本：libusb 1.0.30，标签 `v1.0.30`，提交 `87a5563`
- 官方发布归档：`libusb-1.0.30.tar.bz2`
- 归档 SHA-256：`fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf`
- 许可证：GNU Lesser General Public License 2.1 or later
- 用途：可选、可替换的生产 USB 枚举、接口 claim、异步 bulk/control/interrupt I/O 与热插拔依赖；不进入公共 C ABI 或公共头。
- 链接：只使用系统或用户提供的动态库；未复制、修改或静态合入上游实现。

许可证全文见 [`third_party/libusb/COPYING`](third_party/libusb/COPYING)。
