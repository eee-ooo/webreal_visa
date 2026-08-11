# 开源实现源码复核记录

复核日期：2026-08-11。方法：对上游记录的固定提交或当时 HEAD 做浅克隆，在 `/tmp` 只读检查指定文件与仓库许可证；没有把上游源码复制到本仓库。

## PyVISA

- 仓库/提交：`pyvisa/pyvisa@e3faa8e1d2ddeb754aad223d4a6d7b68f8cc687c`
- 文件：`pyvisa/rname.py`、`LICENSE`
- 许可证：MIT

有价值的思想是按“接口类型 + 资源类”注册解析器，并由解析结果统一生成规范名。这支持本项目把资源语法与传输打开分开。

不采用其查找表达式实现：该文件虽然明确写出 VISA 与 Python 正则语法不同，但基础过滤仍把 `?` 替换后交给 Python `re`，属性过滤还使用 `eval`。本项目因此实现独立 VPP 表达式解析/匹配器，并在 `0.1` 明确拒绝尚未安全实现的属性过滤。

## pyvisa-py

- 仓库/提交：`pyvisa/pyvisa-py@7ed714c7f081404db3b860f9a145873bd38e0d67`
- 文件：`pyvisa_py/sessions.py`、`LICENSE`
- 许可证：MIT

该实现按 `(interface type, resource class)` 注册会话子类，并把公共属性与资源特有属性分层。这验证了后端注册和公共会话属性不应进入各个协议分支。`webreal_visa` 采用自己的 C++ `BackendSession` 能力接口与 `SessionObject` 属性层，没有翻译或复制 Python 代码。

## OpenVisa

- 仓库/提交：`lilongww/OpenVisa@0a7bdac9c490819440416268c876a37c1b4896c8`
- 文件：`src/OpenVisa/Private/IOBase.h`、`src/OpenVisa/Object.cpp`、`LICENSE`
- 许可证：LGPL-3.0-or-later

`IOBase` 展示了统一 I/O 抽象连接多种传输的可行性；但 `Object.cpp` 同时承担地址解析和协议类型分派，且是自创 C++ API，不是稳定 VISA C ABI。项目保留“能力接口”思想，同时将资源解析、句柄/会话、operation 和后端拆层，避免未来协议继续集中到 API 对象。

许可证结论纠正了早期调研中可能产生的 MIT 误判。OpenVisa 只能作思想/行为对照；不得复制源码进入目标 MIT 项目。若未来选择 LGPL 组件，只能在单独 ADR 和动态可替换合规边界下处理。

## standalone Asio

- 仓库/提交：`chriskohlhoff/asio@8806a6803cde7054c3049d3666d3ec36786568c5`（1.38.2）
- 文件/API：`asio/include/asio.hpp`、TCP resolver/socket、serial port、strand、steady timer、associated cancellation slot
- 许可证：Boost Software License 1.0

`0.2` 采用 Asio 作为私有 header-only 构建依赖，不复制其实现到第一方源码，也不暴露 Asio 类型到公共 ABI。采用其公开异步 API 和逐 operation cancellation 能力；资源语义、VISA 状态映射、队列、read-ahead、deadline 竞争与串口编号均由本项目独立实现。

技术判断是使用一个进程共享、有限工作线程的 `io_context`，再为每个会话建立 strand。这样不会随设备数线性创建线程。每个 operation 绑定自己的 cancellation signal，普通超时或 `viTerminate` 不通过关闭整个 Socket/串口实现；真正的 `viClose` 才关闭传输。

许可证允许与目标 MIT 项目组合，但必须保留 Asio 版权和许可文本；因此仓库和安装规则包含独立的第三方声明。精确固定方式见来源台账与 ADR-0006。

## 对 0.1 与 0.2 的具体影响

- 资源解析产出与传输无关的 `ResourceDescriptor`，解析成功不等于后端可打开。
- 后端由小型 `BackendSession` 能力接口隔离；模拟实现不伪装成物理资源。
- 查找表达式由本项目解析，不依赖 ECMAScript/PCRE/Python 方言，也不执行用户表达式。
- 公共 C ABI 不暴露任何参考项目的对象模型。
- TCP 与 ASRL 复用第一方流引擎，但协议特有属性和平台操作仍由各自后端隔离，避免以后将 VXI-11/HiSLIP 强塞进 raw stream。
- 所有实现均为依据规范和上述架构比较独立编写，无复制/修改上游代码。
