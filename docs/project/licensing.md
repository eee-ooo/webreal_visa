# 许可与第三方代码政策

项目目标许可证：MIT。版权主体：`[TBD_COPYRIGHT_HOLDER]`。

由于版权主体未确认，当前仓库不含正式项目 `LICENSE`，不得创建公开发行物。确认主体后，可以在不改变源码 ABI 的情况下添加项目 `LICENSE`、版权头和发布材料；这不会阻塞内部开发。第三方许可证文件不是项目许可证，必须保留且不能因上述阻塞而省略。

## 当前第三方依赖

当前使用 standalone Asio，并可选使用 libusb，具体边界如下：

| 项目 | 固定版本 | 获取与校验 | 许可证 | 使用方式 |
|---|---|---|---|---|
| standalone Asio | 1.38.2，提交 `8806a6803cde7054c3049d3666d3ec36786568c5` | GitHub codeload 提交归档，SHA-256 `ca7f6c14f2bf91e61c7e81fb693f2f8fc86f93e85520d5fc7fd035d0f666bb35`；或 `WRVISA_ASIO_SOURCE_DIR` 指向已审计源码 | Boost Software License 1.0 | 私有、未修改的 header-only 构建依赖；不进入公共头或安装导出接口 |
| libusb | 1.0.30，标签 `v1.0.30`，提交 `87a5563` | 官方 `libusb-1.0.30.tar.bz2`，SHA-256 `fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf`；构建消费系统或用户提供的 1.0.30+ 动态库 | LGPL-2.1-or-later | 可选、可替换、未修改的生产 USB 动态依赖；不进入公共 C 头，静态/共享目标均不静态合入 libusb |

许可证原文保存在 [`third_party/asio/LICENSE_1_0.txt`](../../third_party/asio/LICENSE_1_0.txt) 和 [`third_party/libusb/COPYING`](../../third_party/libusb/COPYING)，发行归属说明保存在 [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md)，均由安装规则随包安装。项目未复制 Asio 或 libusb 实现源码到仓库；默认构建由 CMake 依据固定 URL 与校验值获取 Asio，并仅探测系统/用户提供的 libusb 动态库。CI 使用官方校验归档临时构建 libusb，不把其产物提交或并入库。

除上述依赖外，当前可构建产物只使用 C/C++ 标准库和操作系统 API。`0.3` 的 XDR、ONC RPC、VXI-11 和 HiSLIP 编解码/状态机，`0.4` 的属性表达式与 alias，`0.5` 的 USBTMC/USB488、USB RAW、provider 和适配逻辑，以及 `0.6` 第一切片的 GPIB provider/transport/session 均依据公开规范或既有项目抽象独立实现，没有复制第三方实现。linux-gpib 当前仅列为 GPL 研究来源，没有链接、复制或派生进入生产库。

`WRVISA_LIBUSB=AUTO` 在缺少 1.0.30 开发包时关闭生产 USB 适配器，`ON` 则配置失败，`OFF` 明确构建无 libusb 版本。启用时共享库动态链接 libusb；静态 `webreal_visa` 归档不包含 libusb 对象，安装导出的静态和共享 CMake 目标以 `LINK_ONLY` 要求消费者链接可替换动态库，同时不传播 libusb include 到编译接口。发行者仍须提供 LGPL 文本、归属、可替换动态库和适用的重新链接条件；本说明不替代正式法律审核。

## 引入政策

未来依赖必须经过以下检查：

1. 固定上游项目、版本/提交、下载地址和校验值。
2. 记录许可证、链接方式、是否修改、归属和 NOTICE 要求。
3. 评估与 MIT 发行目标、静态/动态分发及商业使用的兼容性。
4. GPL 等强 copyleft 实现只可作为独立工具或协议研究资料，不进入库的链接或派生边界。
5. LGPL 依赖采用可替换动态边界，并在发行流程中提供许可证与重新链接所需条件。

本文件是工程政策，不是法律意见；正式发布前需要由版权主体完成法律复核。
