# ADR-0012：Prologix 控制器的显式配置与共享状态边界

状态：接受（2026-08-14）

## 背景

`0.6` 第三切片评审 Prologix GPIB-USB 6.0 与 GPIB-Ethernet 1.2 作为不依赖 linux-gpib 的生产控制器。两种设备使用相同的 `++` 命令接口，分别通过虚拟串口和 TCP 1234 访问。官方协议能够表达主/次地址、写 EOI、selected device clear、group execute trigger 与 serial poll，并要求发送数据中的 CR、LF、ESC 和 `+` 使用 ESC 前缀转义。

控制器设置不是按 GPIB 地址隔离的：`mode`、`addr`、`auto`、`eoi`、`eos`、`eot_*` 与 `read_tmo_ms` 对整个物理控制器生效，且 `savecfg` 默认会把变化写入 EEPROM。控制器读取端只以一个可配置字节标记 EOI，设备数据本身不做转义，因此任意 8 位响应与 EOI 标记之间不存在无歧义编码。

## 决策

- 增加 RM 范围的版本化 `wrvisaSetPrologixController` 配置。标准 `GPIB[board]::primary[::secondary]::INSTR` 名称保持不变；串口路径、TCP 主机、端口和协议限制不编码进 VISA 资源字符串，也不从 `ASRL[board]`、环境变量或设备扫描中猜测。
- 配置明确选择官方 Prologix serial 或 TCP 连接、EOI 标记字节、1–3000 ms 控制器读取空闲超时和有界最大响应大小。连接类型、原样 endpoint 字符串和 TCP port 共同构成进程内身份；同一身份只允许一组一致配置，冲突配置返回资源忙，不创建第二条独立状态通道。
- 每个已配置端点身份由进程级弱共享池持有一个 controller。所有 RM、board 映射和 GPIB 会话复用该 controller，并在地址选择、控制器状态变更、数据传输和响应排空的完整事务外层仲裁。不得仅在单个 VISA session 内加锁；DNS 别名、等价 IP 文本和串口符号链接不会自动归并，调用方必须对同一设备使用一致的 endpoint 拼写。
- 每次建立或重建连接，先发送 `++savecfg 0`，再固定 controller mode、关闭 read-after-write、禁用自动 EOS、设置 EOT 标记/读取超时并用 `++ver` 验证端点。运行中地址、EOI 和 timeout 改动不得写入 EEPROM。
- 写入对 CR、LF、ESC 和 `+` 做逐字节转义，再追加未转义 LF 作为控制器帧终止符。读取使用 `++read eoi`，在释放物理控制器事务前一直排空到 EOT 标记，并以配置的最大响应大小限制内存。
- EOT 标记字节不属于仪器响应。若仪器可能返回该字节，当前 Prologix Provider 不适合该工作负载；不得把它描述为任意二进制透明。调用方必须选择不会出现在预期响应中的标记，真实硬件验证前保持 `NOT_TESTED`。
- 排队等待遵守同一个 operation 绝对 deadline。活动事务超时、取消、协议越界、连接错误或没有收到 EOT 时，立即把连接标记为不可复用并关闭；下一事务重新连接和完整初始化。不得在未知控制器/流状态上继续发送命令。
- Provider 不扫描 GPIB 地址，也不虚构发现结果。配置只授权对应 board 的显式 `viOpen`；`viFindRsrc` 仍只返回能够被安全证明存在的资源。
- `INTFC`、parallel poll、pass control、跨进程仲裁、控制器网络发现和任意二进制透明读取不在本切片范围。

## 后果

Prologix Provider 可以在不采用 GPL 库的情况下为标准 GPIB `INSTR` 会话提供受限生产路径，并复用现有 `GpibBackendSession` 的 deadline、取消、终止符、read-ahead 和失败不提交规则。新增公共结构使用 size、ABI major/minor、零 flags 与零 reserved 建立扩展边界；其他厂商控制器不复用或曲解 Prologix 专属字段。

同一显式端点身份在单进程内不会因多个 RM 或会话产生状态竞态，但不同别名或不同进程仍可能绕开该身份池；串口独占行为也依赖操作系统。项目不宣称别名去重或跨进程安全；部署必须统一 endpoint 配置，需要更强保证时另行设计规范化身份、锁服务或独占代理。

## 被否决方案

- 把 `GPIB7` 暗中映射到 `ASRL7`：编号相同不代表物理关联，会造成不可诊断的错误设备访问。
- 每个 GPIB session 单独打开串口/TCP：控制器状态全局共享，会交叉切换地址、EOI 和读取模式，且串口通常不能重复打开。
- 启用 `++auto 1`：非查询写入会触发无响应读取并可能让仪器产生 Query Unterminated，且破坏 `viWrite`/`viRead` 的显式边界。
- 保持默认 `savecfg 1`：按地址和 send-end 切换配置会持续写 EEPROM。
- 收到 `viRead` 请求大小后停止读取并释放控制器：响应尾部仍在串口/TCP 缓冲中，会污染其他地址或会话。
- 把任意固定 EOT 字节称为二进制透明 EOI：协议没有对仪器到主机方向转义，无法成立。
