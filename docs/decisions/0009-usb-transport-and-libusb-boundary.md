# ADR-0009：USB 协议与可替换 libusb 边界

状态：Accepted（2026-08-13）

## 背景

`0.5` 进入 USBTMC/USB488 与 USB RAW。USBTMC 是带消息头、bTag、EOM、类控制请求和端点状态的协议，不能作为普通字节流处理。libusb 提供跨平台枚举、claim、control/bulk/interrupt transfer、异步取消和热插拔，但它是 LGPL-2.1-or-later 依赖，而且当前开发环境只有运行库、没有开发头或 pkg-config 元数据。

早期主路线曾把 USB 闭环称为 `0.4`；仓库已经把 `0.4` 用于 alias、查找属性和 ABI 加固，因此 USB 顺延为 `0.5`，后续 GPIB 版本相应顺延，不改写既有发布历史。

## 决策

- 第一方 `UsbTransport` 契约只使用项目类型和字节视图，不在公共 ABI、核心对象或 USBTMC 状态机中暴露 libusb 类型。
- USBTMC 编解码和会话状态机独立实现并以模拟 transport 测试；USB RAW 复用 transport 能力，但不绕过统一 operation、deadline、取消和会话生命周期。
- 真实适配器以 libusb 1.0.30 为当前固定候选。优先消费系统提供的动态库；缺少 libusb 开发条件时不构建真实适配器，其他后端和无硬件 USB 测试仍须通过。
- libusb transfer 使用异步接口。取消只有在 completion callback 到达后才算传输生命周期结束；适配器不得提前释放 transfer 或用户缓冲。由于 macOS 取消单个 transfer 可能连带取消同端点其他 transfer，同一 endpoint 先串行调度，避免把逐 operation 取消语义错误外推为端点内并行。
- USB 设备/接口仲裁对象负责 claim/release 和连接代次；多个 VISA 会话不得各自无协调地重复 claim 同一接口。热插拔通知只更新发现/连接状态，不在回调中执行阻塞描述符或同步 I/O。

## 后果

协议和大部分故障测试不依赖真实 USB 设备或 libusb 安装，真实适配器也可替换为平台原生或厂商实现。首个切片不会宣称 USB 硬件可用；只有 libusb 枚举、打开、异步 transfer、取消与至少一个硬件结果完成后，才提升相应平台状态。

引入 libusb 后，共享库保持动态链接；静态 `webreal_visa` 消费必须显式链接可替换的 libusb 动态库，发行材料保留 LGPL 文本、归属和重新链接条件。正式发行仍受项目版权主体未定阻塞。

## 被否决方案

- 在 USBTMC 会话中直接调用 libusb：会把协议状态机、平台事件循环和许可边界耦合。
- 使用同步 libusb transfer 加每请求线程：难以满足逐 operation 取消，并使并发资源成本随请求增长。
- 将 USBTMC 当作 raw bulk 字节流：会丢失 bTag、EOM、abort/clear、短包和响应边界语义。
- 因开发机缺少头文件而复制最小 libusb ABI 声明：容易产生结构布局和版本不匹配，且绕开依赖审计。
