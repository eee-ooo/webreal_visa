# ADR-0010：GPIB 控制器与会话边界

状态：接受（2026-08-13）

## 背景

`0.6` 进入本机 GPIB。标准资源名能够表达 board、主地址、可选次地址以及 `INSTR`/`INTFC`，但不能说明底层控制器来自 linux-gpib、NI-488.2、Prologix 或其他厂商驱动。各控制器对 EOI/EOS、IFC/REN、serial poll、SRQ、并行轮询、多会话和取消的支持强度也不同。当前环境没有可用于验收的 GPIB 控制器或总线仪器。

## 决策

- `ResourceDescriptor` 保留 GPIB board、主地址、可选次地址和资源类，不把本机驱动名称编码进标准资源字符串。
- 第一方 `GpibTransport` 只使用项目类型、字节视图和 `Operation`，并显式报告 send-end、device clear、trigger 与 serial poll 能力；核心和公共 C ABI 不接触厂商句柄或头文件。
- `GpibProvider` 负责发现规范资源并按地址打开 transport。RM 保存创建时的发现快照；显式打开使用当前 provider 注册表，并保留可诊断失败。
- 第一切片只用测试目标中的模拟 provider 验证公共 `vi*` 闭环。linux-gpib、NI-488.2 与 Prologix 均须在后续独立切片中经过依赖、许可、错误映射和生命周期评审后接入。
- `INTFC` 与地址化 `INSTR` 保持不同资源身份；第一切片只开放 `INSTR` I/O，`INTFC` 打开在专用控制器会话语义确定前明确返回不支持。

## 后果

协议会话可以复用现有 deadline、取消、终止符、read-ahead 与失败不提交规则，同时避免把任一控制器的限制冒充成通用 GPIB 能力。没有注册 provider 时，合法 GPIB 名称仍可解析，但打开明确不支持。模拟结果只能证明本项目公共层与受控控制器契约闭环，不能证明真实总线电气、驱动或仪器互操作。

## 被否决方案

- 直接在 API 层调用 linux-gpib 或 NI-488.2：会把平台头、厂商句柄和错误模型扩散到核心。
- 把 Prologix 命令集当作通用 GPIB：它缺少部分总线控制能力，不能代表 linux-gpib/NI-488.2。
- 为测试增加看似标准的 GPIB 资源语法：会污染资源兼容边界；测试 provider 应注册合法规范资源。
