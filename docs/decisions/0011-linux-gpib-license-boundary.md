# ADR-0011：拒绝在核心库中链接 linux-gpib

状态：接受（2026-08-14）

## 背景

`0.6` 第二切片评审 linux-gpib 作为 Linux 本机 GPIB 生产 Provider 的候选。官方 4.3.7 用户态源码包含 `ibdev`、`ibrda`/`ibwrta`、`ibwait`、`ibstop`、`ibclr`、`ibtrg`、`ibrsp` 与线程局部状态接口，技术上可以映射到现有 `GpibTransport`。但是官方项目明确将包声明为 GNU GPL，4.3.7 用户态归档的 `COPYING` 是 GPL v2，公开头和实现文件也带 GPL-2.0 或 GPL v2-or-later 声明，没有发现链接例外。

本项目目标许可证为 MIT，既有依赖政策禁止把 GPL 等强 copyleft 实现链接、复制或派生进核心库。该政策本身不是一般性法律结论，而是当前工程在正式法律审核前采用的保守发布边界。

## 决策

- `webreal_visa` 生产库、进程内插件和安装导出目标不得编译包含 linux-gpib 头、直接或延迟链接 `libgpib`，也不得通过 `dlopen`/`dlsym` 绕过同一进程链接边界。
- 不增加 `WRVISA_LINUX_GPIB` 构建选项，不注册 linux-gpib 生产 Provider，不复制其公开头、错误表或实现代码。现有通用 `GpibProvider`/`GpibTransport` 是本切片的停止点。
- linux-gpib 4.3.7 只作为许可与 API 研究证据；归档、头文件和 GPL 源码不提交、不安装，也不进入 `THIRD_PARTY_NOTICES.md` 的已采用依赖列表。
- 只有上游提供与项目目标兼容的链接例外、替代许可或商业授权后，才可新建 ADR 重新评审进程内 Provider。
- 若未来需要以独立进程隔离 GPL 组件，必须先单独决定 IPC 协议、进程生命周期、取消/崩溃恢复、独立源码许可证和分发方式；不得把该方案视为本 ADR 已授权的实现。

## 后果

`webreal_visa` 仍不能通过 linux-gpib 访问物理 GPIB。合法 GPIB 资源在没有其他生产 Provider 时继续明确返回 `VI_ERROR_NSUP_OPER`，测试 Provider 结果也不能外推为硬件支持。

技术评审仍固定了未来适配所需语义：次地址传给 `ibdev` 时需要 `0x60 + secondary`，异步缓冲必须存活到 `ibwait(CMPL)` 或 `ibstop` 完成 join，`ibstop` 成功取消以 `ERR`/`EABO` 表示，结果应读取 `AsyncIbsta`/`AsyncIberr`/`AsyncIbcntl`，离散 `ibtmo` 不能替代本项目的绝对 deadline。这些结论只用于评估，不构成对 GPL API 的采用。

## 被否决方案

- 直接动态链接或运行时 `dlopen` `libgpib`：仍进入本项目明确禁止的进程内强 copyleft 边界。
- 复制最小函数声明和常量后调用共享库：不能改变链接关系，还会复制 GPL 头文件表达并增加 ABI 漂移风险。
- 绕过用户态库直接复制 ioctl/内核协议：会复制或派生 GPL 接口实现、绑定不稳定内核细节，并失去上游描述符和取消语义。
- 在没有生产驱动的情况下注册虚构 linux-gpib 资源：会把模拟闭环错误描述为物理控制器支持。
