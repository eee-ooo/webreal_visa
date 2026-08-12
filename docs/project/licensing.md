# 许可与第三方代码政策

项目目标许可证：MIT。版权主体：`[TBD_COPYRIGHT_HOLDER]`。

由于版权主体未确认，当前仓库不含正式项目 `LICENSE`，不得创建公开发行物。确认主体后，可以在不改变源码 ABI 的情况下添加项目 `LICENSE`、版权头和发布材料；这不会阻塞内部开发。第三方许可证文件不是项目许可证，必须保留且不能因上述阻塞而省略。

## 当前第三方依赖

`0.2`–`0.4` 使用 standalone Asio，具体边界如下：

| 项目 | 固定版本 | 获取与校验 | 许可证 | 使用方式 |
|---|---|---|---|---|
| standalone Asio | 1.38.2，提交 `8806a6803cde7054c3049d3666d3ec36786568c5` | GitHub codeload 提交归档，SHA-256 `ca7f6c14f2bf91e61c7e81fb693f2f8fc86f93e85520d5fc7fd035d0f666bb35`；或 `WRVISA_ASIO_SOURCE_DIR` 指向已审计源码 | Boost Software License 1.0 | 私有、未修改的 header-only 构建依赖；不进入公共头或安装导出接口 |

许可证原文保存在 [`third_party/asio/LICENSE_1_0.txt`](../../third_party/asio/LICENSE_1_0.txt)，发行归属说明保存在 [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md)，二者由安装规则随包安装。项目未复制 Asio 实现源码到仓库；默认构建由 CMake 依据固定 URL 与校验值获取。

除上述依赖外，库代码只使用 C/C++ 标准库和操作系统 API。`0.3` 的 XDR、ONC RPC、VXI-11 和 HiSLIP 编解码/状态机，以及 `0.4` 的属性表达式与 alias 解析均依据公开规范独立实现，没有引入系统 SunRPC、libtirpc、liblxi 或 libhislip。调研项目没有源码被复制或修改，也未链接进入库。

## 引入政策

未来依赖必须经过以下检查：

1. 固定上游项目、版本/提交、下载地址和校验值。
2. 记录许可证、链接方式、是否修改、归属和 NOTICE 要求。
3. 评估与 MIT 发行目标、静态/动态分发及商业使用的兼容性。
4. GPL 等强 copyleft 实现只可作为独立工具或协议研究资料，不进入库的链接或派生边界。
5. LGPL 依赖采用可替换动态边界，并在发行流程中提供许可证与重新链接所需条件。

本文件是工程政策，不是法律意见；正式发布前需要由版权主体完成法律复核。
