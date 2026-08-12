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
    ├── Serial discovery / RM-scoped alias & transport overrides
    └── Backend session interface
          ├── Mock backend
          ├── Asio stream engine ── per-session strand / read & write queues
          │     ├── raw TCP Socket backend
          │     └── ASRL serial backend
          └── bounded request channel ── record/frame read + response draining
                ├── ONC RPC/XDR ── VXI-11 core + abort channels
                └── HiSLIP ── synchronous data + asynchronous control channels
                         │
                         ▼
                  process-shared IoRuntime
```

公共层只依赖固定布局的 C 类型。API 层依赖核心层；核心层通过抽象会话使用后端，后端不得反向依赖公共函数。资源解析独立于传输，以便未知但语法正确的标准资源能被解析，同时由 `viOpen` 如实报告后端不可用。

standalone Asio 只作为实现目标的私有头文件依赖，不进入安装后的公共 include 或 CMake 接口。共享 I/O runtime 使用有限工作线程，并由进程生命周期强引用持有，避免设备数增加时产生“每会话一组线程”的扩展陷阱，也避免最后一个会话引用从 worker 回调释放时自我 join；会话 strand 串行化流对象状态，而读队列与写队列仍可独立推进。

## 生命周期

句柄由 32 位值编码：高 4 位对象类型、中 12 位代际、低 16 位槽位加一。槽位删除时代际递增；到达上限后槽位退役而不回绕。查表返回共享所有权，使并发调用可安全完成或被取消；关闭首先从表中移除句柄，再取消所有 operation，并立即关闭后端传输。

资源管理器跟踪子句柄、ASRL 接口号到本机设备路径的映射、按主机/协议限定的 TCPIP 服务端口覆盖，以及大小写无关的一对一资源 alias。自动发现产生确定性排序；`wrvisaSetSerialPath` 可在打开会话前显式覆盖或补充映射，`wrvisaSetTcpipServicePort` 允许测试模拟器和自定义网关使用非特权服务端口，`wrvisaSetResourceAlias` 为当前 RM 配置非持久化名字。`viOpen`、`viParseRsrc` 和 `viParseRsrcEx` 共享同一 alias 解析路径，已经打开的会话不受后续配置变化影响。关闭 RM 会使其子查找列表和会话失效并取消阻塞操作。

`viFindRsrc` 先以 VPP 资源正则匹配 canonical name，再对规范化描述符求值可选 `{attrExpr}`。0.4 白名单只含接口类型/编号、资源类/名称和 ASRL 默认波特率；未知、局部或类型错误属性在表达式编译时失败，查找过程不做网络或串口 I/O。find-list 和 count 可省略；省略 find-list 时不注册临时句柄。

## Operation 与流式 I/O

每次阻塞或可取消调用建立 operation。operation 使用原子最终状态，正常、超时、`viTerminate` 和关闭竞争同一个完成点。等待基于 `steady_clock` 绝对 deadline；operation 的取消 hook 只发出对应 Asio cancellation slot，不以关闭整个流代替普通取消。

后端先写入内部缓冲，只有赢得成功完成的一方才提交用户缓冲区。终止符之后的多读数据保存在 read-ahead；若读取在收到部分字节后超时或被取消，这些字节也回到 read-ahead，失败调用返回零字节，避免产生“既报失败又消费数据”的模糊状态。排队操作同样能被逐项取消。

VXI-11 和 HiSLIP 使用有界 request channel，不假设一次 TCP read 等于一条协议消息。VXI-11 在 XDR 解码前验证 RPC record marking 和最大记录长度，HiSLIP 在分配 payload 前验证 16 字节帧头与协商上限。普通响应按请求顺序排队；取消 VXI-11 请求时通过 abort 通道通知设备并排空该 RPC 响应，取消 HiSLIP 请求时通过异步通道执行 device-clear/interrupt 恢复。恢复完成前不启动下一个请求，避免旧响应错配。

## 锁

锁管理器按规范化资源名隔离，支持同进程排他锁、共享访问键和同类型嵌套计数；I/O 仅在操作开始时查询权限，不持有全局锁执行。第一次本地加锁成功后才请求远端锁，最后一次解锁再释放远端锁，远端失败会回滚本地状态。HiSLIP 可映射远端共享与排他锁；VXI-11 协议只有排他 `device_lock`，所以 VXI-11 的 VISA 共享锁仍只协调当前进程。跨进程同主机协调和混合类型嵌套的完整 VISA 行为仍未实现。

## 扩展点

插件 ABI 单独位于 `webreal_visa_plugin.h`，使用尺寸与版本协商；`0.4` 仍只冻结契约，不加载插件。新增协议必须实现 `BackendSession` 能力接口；可复用字节流语义的协议可使用流引擎，但消息协议不能被强行伪装成 raw stream。VXI-11、HiSLIP、USB 与 GPIB 保持独立后端边界；HiSLIP overlap、HiSLIP 2/TLS 和生产发现也不会由同步模式实现隐式开启。
