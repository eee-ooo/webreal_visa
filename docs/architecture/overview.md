# 架构总览

## 分层

```text
C/C++ 调用方
    │  include/visa.h（稳定 C ABI）
    ▼
API 外观 ── 参数校验、句柄解析、状态映射、后端选择
    ▼
核心对象 ── RM / FindList / Session / 类型化代际句柄表
    ├── Resource parser + VPP regex/static-attribute matcher
    ├── Operation ── deadline / exactly-once completion / cancellation hook
    ├── Lock manager ── 进程内协调 + 协议远端锁挂钩
    ├── Serial discovery / GPIB+USB discovery snapshots / RM-scoped configuration
    └── Backend session interface
          ├── Mock backend
          ├── Asio stream engine ── per-session strand / read & write queues
          │     ├── raw TCP Socket backend
          │     └── ASRL serial backend
          └── bounded request channel ── record/frame read + response draining
                ├── ONC RPC/XDR ── VXI-11 core + abort channels
                └── HiSLIP ── synchronous data + asynchronous control channels
          ├── GPIB session
          │     ├── GpibProvider registry ── discovery snapshot / open routing
          │     └── GpibTransport ── EOI / clear / trigger / serial poll
          └── USB protocol sessions
                ├── USBTMC/USB488 ── framing / split recovery / status / trigger
                ├── USB RAW ── configured bulk/interrupt I/O + EP0 control
                ├── UsbProvider registry ── discovery snapshot / open routing
                ├── UsbInterfaceArbiter ── shared claim / release / invalidation
                └── UsbTransport ── replaceable bulk/control/interrupt adapter
                      └── LibusbProvider ── enumerate/open/hotplug
                            └── process-level libusb event thread

Asio stream/message backends ── process-shared IoRuntime
```

公共层只依赖固定布局的 C 类型。API 层依赖核心层；核心层通过抽象会话使用后端，后端不得反向依赖公共函数。资源解析独立于传输，以便未知但语法正确的标准资源能被解析，同时由 `viOpen` 如实报告后端不可用。

standalone Asio 只作为实现目标的私有头文件依赖，不进入安装后的公共 include 或 CMake 接口。共享 I/O runtime 使用有限工作线程，并由进程生命周期强引用持有，避免设备数增加时产生“每会话一组线程”的扩展陷阱，也避免最后一个会话引用从 worker 回调释放时自我 join；会话 strand 串行化流对象状态，而读队列与写队列仍可独立推进。libusb 是独立的可选动态依赖，不进入公共 C 头；安装导出只以 `LINK_ONLY` 恢复链接依赖，避免把 libusb include 传播到调用方编译接口。

## 生命周期

句柄由 32 位值编码：高 4 位对象类型、中 12 位代际、低 16 位槽位加一。槽位删除时代际递增；到达上限后槽位退役而不回绕。查表返回共享所有权，使并发调用可安全完成或被取消；关闭首先从表中移除句柄，再取消所有 operation，并立即关闭后端传输。

资源管理器跟踪子句柄、ASRL 接口号到本机设备路径的映射、创建时取得的不可变 GPIB/USB 发现快照、按规范资源名保存的 USB RAW 配置、按主机/协议限定的 TCPIP 服务端口覆盖，以及大小写无关的一对一资源 alias。自动发现规范化、去重并产生确定性排序；单个 GPIB/USB provider 发现失败不会隐藏其他 provider 或 ASRL 资源。`wrvisaSetSerialPath` 可在打开会话前显式覆盖或补充映射，`wrvisaSetTcpipServicePort` 允许测试模拟器和自定义网关使用非特权服务端口，`wrvisaSetResourceAlias` 为当前 RM 配置非持久化名字，`wrvisaSetUsbRawConfig` 为后续 RAW 打开固定 alternate setting 和读写端点。`viOpen`、`viParseRsrc` 和 `viParseRsrcEx` 共享同一 alias 解析路径；已打开会话持有自己的配置快照，不受 RM 后续修改影响。关闭 RM 会使其子查找列表和会话失效并取消阻塞操作。

`viFindRsrc` 先以 VPP 资源正则匹配 canonical name，再对规范化描述符求值可选 `{attrExpr}`。0.4 白名单只含接口类型/编号、资源类/名称和 ASRL 默认波特率；未知、局部或类型错误属性在表达式编译时失败，查找过程不做网络或串口 I/O。find-list 和 count 可省略；省略 find-list 时不注册临时句柄。

## Operation 与流式 I/O

每次阻塞或可取消调用建立 operation。operation 使用原子最终状态，正常、超时、`viTerminate` 和关闭竞争同一个完成点。等待基于 `steady_clock` 绝对 deadline；operation 的取消 hook 只发出对应 Asio cancellation slot，不以关闭整个流代替普通取消。

后端先写入内部缓冲，只有赢得成功完成的一方才提交用户缓冲区。终止符之后的多读数据保存在 read-ahead；若读取在收到部分字节后超时或被取消，这些字节也回到 read-ahead，失败调用返回零字节，避免产生“既报失败又消费数据”的模糊状态。排队操作同样能被逐项取消。

VXI-11 和 HiSLIP 使用有界 request channel，不假设一次 TCP read 等于一条协议消息。VXI-11 在 XDR 解码前验证 RPC record marking 和最大记录长度，HiSLIP 在分配 payload 前验证 16 字节帧头与协商上限。普通响应按请求顺序排队；取消 VXI-11 请求时通过 abort 通道通知设备并排空该 RPC 响应，取消 HiSLIP 请求时通过异步通道执行 device-clear/interrupt 恢复。恢复完成前不启动下一个请求，避免旧响应错配。

0.5 的 USBTMC 状态机与 `UsbTransport` 分层。协议层负责 bTag、反码、传输长度、EOM、终止符、bulk-OUT 四字节对齐，以及 bulk-IN 由短包终止的最多 `wMaxPacketSize - 1` 对齐字节；transport 负责 bulk/control/interrupt transfer、端点 halt 清除、取消与断开。USBTMC class clear 和 abort 作为 INITIATE/CHECK split transaction 执行，取消或超时的原始结果先固定，再在独立 500 ms 恢复预算内重建传输边界；abort 失败时回退完整 clear，两者都失败才使会话不可复用。USB488 在能力校验后发送 TRIGGER，并以 2–127 状态标签关联 READ_STATUS_BYTE control 与 interrupt-IN 响应；异步 SRQ 和迟到标签被消费但不会错配当前请求。

USB RAW 使用单独的 `UsbRawBackendSession`，不会将厂商请求加入标准 `vi*` 命名空间。公共版本化结构以 `struct_size`、ABI 主/次版本、零保留字段和零 flags 建立向前扩展边界；当前支持为读写方向分别选择 `none`、bulk 或 interrupt 端点。配置必须先写入 RM，未配置的 RAW 资源不打开；provider 返回的 alternate setting 和端点必须与配置完全一致。标准 `viRead`/`viWrite` 复用终止符、read-ahead、operation deadline 和取消提交规则，`viClear` 清除已配置端点的 halt，读缓冲 discard 清理 read-ahead；端点零 IN/OUT 由 `wrvisaUsbControlTransfer` 承载，长度受 USB setup packet 的 16 位 `wLength` 限制。

`UsbProvider` 以进程内注册表提供发现与打开；RM 只保存创建时快照，而显式 `viOpen` 每次路由到当前 provider。打开保持注册顺序；某个 provider 的可诊断失败会被暂存，后续 provider 仍可成功处理同一资源，全部失败时返回最早诊断，违反“成功状态但无 transport”契约则立即转为系统错误。`UsbInterfaceArbiter` 让同一身份的并发会话共享一次物理 claim，并在最后关闭或拔出失效时只 release 一次；连接代次防止旧会话在重连后复活。

内建 `LibusbProvider` 在启用 libusb 1.0.30 时枚举 active configuration：符合 class/subclass/protocol 且具有唯一 bulk-IN/bulk-OUT 的 interface 生成 USBTMC/USB488 `INSTR`，每个 interface 同时可生成独立 `RAW` 身份。INSTR 按协议端点严格校验；RAW 打开按 VID/PID/serial/interface、指定 alternate setting 及端点方向/类型精确匹配。相同物理接口的 INSTR 与 RAW VISA 会话共享 `LibusbConnection`、device ref、handle 和一次 claim；不同 alternate setting 不能在同一连接上并存。可用时自动 detach kernel driver，并在最后一个 lease/连接释放时按一次 release/close。打开创建与 provider 状态分别加锁，libusb 调用和事件线程 join 不在热插拔状态锁内执行。

第一个 libusb handle 启动专用事件线程，最后一个关闭后停止。bulk-OUT、bulk-IN、control 和 interrupt 各有方向适当的 gate，同类端点请求串行、不同端点可以推进；连接跟踪全部 active transfer，关闭或拔出可安全取消并等待各自 callback。transfer 使用 operation 剩余绝对预算，callback 固定最终状态并唤醒等待方。取消在 transfer 集合锁保护下调用 `libusb_cancel_transfer`，且必须等 callback 到达后才清除跟踪、释放 transfer 和缓冲。热插拔 callback 不调用描述符、打开或同步 I/O，只记录 device pointer 的 ARRIVED/LEFT 并标记弱连接；活动 transfer 由 libusb 完成为 NO_DEVICE，新操作直接返回连接丢失。脚本化 transport、仅测试 provider 和 libusb C API 模拟器分别验证协议、公共 API、RAW 和生产适配边界；真实 USB 硬件结果仍为 `NOT_TESTED`。

0.6 的 GPIB 第一切片把 `GpibProvider`、`GpibTransport` 与 `GpibBackendSession` 分层。资源描述符保存 board、主地址、可选次地址和 `INSTR`/`INTFC` 身份；RM 保存创建时发现快照，显式打开使用当前 provider 注册表并延后返回最早可诊断错误。transport 显式报告 send-end、device clear、trigger 与 serial poll 能力，并以 `end` 表示 EOI；会话按半双工串行化事务，复用 operation deadline、取消、终止符、read-ahead 和失败不提交规则。`INTFC` 暂不开放控制器会话。第二切片按 ADR-0011 拒绝把 GPL linux-gpib 直接链接、延迟链接或 `dlopen` 进核心库；生产库仍无 linux-gpib、NI-488.2 或 Prologix provider。当前公共 `vi*` 闭环仅由测试目标中的 provider 证明，真实总线仍为 `NOT_TESTED`。

## 锁

锁管理器按规范化资源名隔离，支持同进程排他锁、共享访问键和同类型嵌套计数；I/O 仅在操作开始时查询权限，不持有全局锁执行。第一次本地加锁成功后才请求远端锁，最后一次解锁再释放远端锁，远端失败会回滚本地状态。HiSLIP 可映射远端共享与排他锁；VXI-11 协议只有排他 `device_lock`，所以 VXI-11 的 VISA 共享锁仍只协调当前进程。跨进程同主机协调和混合类型嵌套的完整 VISA 行为仍未实现。

## 扩展点

插件 ABI 单独位于 `webreal_visa_plugin.h`，使用尺寸与版本协商；`0.6` 仍不实现通用动态加载。新增协议必须实现 `BackendSession` 能力接口；可复用字节流语义的协议可使用流引擎，但消息协议不能被强行伪装成 raw stream。VXI-11、HiSLIP、USB 与 GPIB 保持独立后端边界；HiSLIP overlap、HiSLIP 2/TLS 和生产发现也不会由同步模式实现隐式开启。
