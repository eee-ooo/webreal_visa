# 技术与来源台账

## 权威规范

| 来源 | 固定信息 | 用途 | 采用关系 |
|---|---|---|---|
| IVI Foundation, VPP-4.3 | Revision 7.2.1, 2024-01-04, `vpp43_2024-01-04.pdf` | API 语义、资源语法、4.3.3 alias/可选 Parse 输出、4.4 资源正则与属性表达式、可选 Find 输出、状态与锁要求 | 0.4 alias/查找兼容切片的规范依据；不复制实现代码 |
| IVI Foundation, VPP-4.3.2 | Revision 7.2, 2022-05-19, `vpp432_2022-05-19.pdf` | C 文本绑定、`visa.h`/`visatype.h` 参考内容、常量数值 | ABI 数值依据；按固定宽度平台类型实现 |
| IVI Foundation, IVI-6.1 HiSLIP | Revision 2.0, 2020-04-23, `IVI-6.1_HiSLIP-2.0-2020-04-23.pdf` | 1.x 消息格式、双通道、初始化、I/O、状态、锁、清除与取消；2.0 内容只用于划定未实现边界 | `0.3` 同步模式协议依据；不复制实现代码 |
| VXIbus Consortium, VXI-11 | Revision 1.0, 1995，官方规范页列出 VXI-11.1/.2/.3 | Network Instrument Protocol、core/abort、远端操作与 RPC program/procedure | `0.3` 协议依据；官方归档链接在 2026-08-11 访问时返回 404，保留页面元数据和既有规范基线，不伪造取得状态 |
| IETF RFC 4506 | 2006-05, XDR Standard | XDR 4 字节对齐、整数、opaque/string 编码 | 第一方编解码依据 |
| IETF RFC 5531 | 2009-05, RPC Version 2 | ONC RPC call/reply envelope 与 record marking | 第一方 RPC 客户端依据 |
| IETF RFC 1833 | 1995-08, Binding Protocols for ONC RPC | portmapper/rpcbind v2 查询 | VXI-11 服务端口解析依据 |
| IVI Foundation, VPP-9 | Revision 4.35, 2024-08-08 | VISA 供应商缩写注册规则与公开列表 | 确认 HiSLIP vendor ID 需注册；当前 `WR` 仅为临时项目值，不宣称已注册 |
| USB-IF, USB Test and Measurement Class Specification + USB488 Subclass | Revision 1.0, 2003-04-14, `USBTMC_1_006a.zip` | USBTMC 接口、DEV_DEP bulk 消息、短包/对齐、abort/clear、能力请求，以及 USB488 trigger/status/interrupt-IN | `0.5` USBTMC/USB488 协议依据；不复制实现代码 |
| IVI Specifications 下载页 | 2026-08-11 查阅 | 确认当前公开修订与权威下载位置 | 元数据依据 |

## 本地调研材料

| 文件 | 性质 | 处理方式 |
|---|---|---|
| 父目录 `开源VISA项目调研.html` | 静态源码调研报告 | 只作项目线索，结论需回到上游或规范验证 |
| 父目录 `visa标准接口调研/VISA 调研1：标准与相关库.html` | 静态标准与库调研报告 | 只作线索，不作为规范原文 |
| 父目录 `VISA_PROJECT_MASTER_PROMPT.md` | 需求形成过程 | 已整理导入 `docs/project/requirements.md`；仓库内文件为权威版本 |

## 采用的第三方依赖

| 项目 | 固定来源 | 许可证 | 采用关系 |
|---|---|---|---|
| standalone Asio 1.38.2 | `chriskohlhoff/asio@8806a6803cde7054c3049d3666d3ec36786568c5`；归档 SHA-256 `ca7f6c14f2bf91e61c7e81fb693f2f8fc86f93e85520d5fc7fd035d0f666bb35`；主要使用 `asio/include/asio.hpp`、TCP、serial_port、strand、timer 与 cancellation API | Boost Software License 1.0 | `0.2` 起的私有 header-only 构建依赖；未修改，不出现在公共头和安装导出依赖中；许可证与声明随安装包提供 |
| libusb 1.0.30 | `libusb/libusb` 标签 `v1.0.30`、提交缩写 `87a5563`；官方 `libusb-1.0.30.tar.bz2` SHA-256 `fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf` | LGPL-2.1-or-later | `0.5` 可选、可替换的动态生产 USB 依赖；未修改、不进入公共 C 头或静态归档，许可证与声明随安装包提供 |

## 0.5 libusb API 约束

| 项目 | 固定版本 | 许可证 | 当前关系 |
|---|---|---|---|
| libusb | 官方稳定标签 `v1.0.30`，发布页提交缩写 `87a5563`；官方归档 SHA-256 `fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf` | LGPL-2.1-or-later | 已采用为 `WRVISA_LIBUSB=AUTO/ON` 的动态生产适配边界；枚举、打开、claim、异步 transfer 和 hotplug 已实现并由受控 API 模拟器验证，真实硬件仍为 `NOT_TESTED` |

libusb 官方异步 API 明确要求取消后等待 completion callback，未完成 transfer 不得释放；macOS/iOS 取消一个 transfer 可能取消同端点全部 transfer。生产适配器据此等待 callback 后释放，并以端点 gate 串行化同类请求。官方事件处理文档建议可移植应用使用专用事件线程；热插拔文档限制 DEVICE_LEFT callback 内的安全 API，因此本实现的 callback 不调用任何 libusb I/O 或描述符函数，只更新本地连接状态。

Asio 版本和构建决策见 [`ADR-0006`](../decisions/0006-asio-real-transport-runtime.md)，许可分发边界见 [`licensing.md`](../project/licensing.md)。

## 开源实现参考边界

`0.1`–`0.4` 没有复制或修改下列参考项目代码，也不链接其库。VXI-11/HiSLIP 的协议层依据上节公开规范独立编写；0.4 属性表达式解析器同样为依据 VPP-4.3 编写的第一方代码。没有引入 `libtirpc`、`liblxi` 或 `libhislip`。2026-08-11 的既有源码复核详情见 [`implementation-review.md`](implementation-review.md)。采用的 Asio 与 libusb 依赖已在上节单独声明，不归入“仅参考”列表。

| 项目 | 上游 | 许可证 | 当前关系 |
|---|---|---|---|
| PyVISA | `pyvisa/pyvisa`，`e3faa8e1d2ddeb754aad223d4a6d7b68f8cc687c`，`pyvisa/rname.py`、`LICENSE` | MIT | 资源注册/规范化思想参考；未复制 |
| pyvisa-py | `pyvisa/pyvisa-py`，`7ed714c7f081404db3b860f9a145873bd38e0d67`，`pyvisa_py/sessions.py`、`LICENSE` | MIT | 后端会话注册和公共属性思想参考；未复制 |
| OpenVisa | `lilongww/OpenVisa`，`0a7bdac9c490819440416268c876a37c1b4896c8`，`src/OpenVisa/Private/IOBase.h`、`src/OpenVisa/Object.cpp`、`LICENSE` | LGPL-3.0-or-later | 只作架构对照；禁止复制进目标 MIT 源码，未链接 |
| liblxi | `lxi-tools/liblxi` | BSD-3-Clause（须按提交复核） | 协议实现线索，未纳入 |
| libhislip | `lxi-tools/libhislip` | BSD-3-Clause（须按提交复核） | 协议实现线索，未纳入 |
| linux-gpib | `coolshou/linux-gpib` 等分发源 | GPL 系列 | 仅协议/工具研究，不进入库 |

## 证据 URL

- https://www.ivifoundation.org/specifications/default.html
- https://www.ivifoundation.org/downloads/VISA/vpp43_2024-01-04.pdf
- https://www.ivifoundation.org/downloads/VISA/vpp432_2022-05-19.pdf
- https://www.ivifoundation.org/downloads/Protocol%20Specifications/IVI-6.1_HiSLIP-2.0-2020-04-23.pdf
- https://www.vxibus.org/specifications.html
- https://www.rfc-editor.org/rfc/rfc4506.html
- https://www.rfc-editor.org/rfc/rfc5531.html
- https://www.rfc-editor.org/rfc/rfc1833.html
- https://www.ivifoundation.org/downloads/VPP/vpp9_4.35_2024-08-08.pdf
- https://github.com/pyvisa/pyvisa-py
- https://github.com/lilongww/OpenVisa
- https://github.com/lxi-tools/liblxi
- https://github.com/lxi-tools/libhislip
- https://think-async.com/Asio/asio-1.38.2/doc/
- https://think-async.com/Asio/License
- https://github.com/chriskohlhoff/asio/tree/8806a6803cde7054c3049d3666d3ec36786568c5
- https://github.com/libusb/libusb
- https://github.com/libusb/libusb/releases/tag/v1.0.30
- https://libusb.sourceforge.io/api-1.0/group__libusb__asyncio.html
- https://libusb.sourceforge.io/api-1.0/libusb_mtasync.html
- https://libusb.sourceforge.io/api-1.0/group__libusb__hotplug.html
- https://www.usb.org/documents
- https://usb.org/sites/default/files/USBTMC_1_006a.zip

CI 使用 `actions/checkout` 的固定提交 `de0fac2e4500dabe0009e67214ff5f5447ce83dd`（v6.0.2，MIT）；它只在 GitHub 托管构建环境执行，不进入库或发行物。
