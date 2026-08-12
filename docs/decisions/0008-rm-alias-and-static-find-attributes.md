# ADR-0008：RM 范围 alias 与静态查找属性

状态：已接受  
日期：2026-08-12

## 背景

VPP-4.3 允许资源 alias，并定义 `regularExpr ['{' attrExpr '}']` 查找语法。既有实现只接受资源正则部分，`viParseRsrcEx` 总是返回空 alias；同时当前工程没有持久化配置服务，也不能为了查找属性而打开真实设备。

## 决策

- 以 `wrvisaSetResourceAlias` 提供 Resource Manager 会话范围、进程内、非持久化 alias。alias 大小写不敏感，禁止冒号、canonical resource 歧义与 alias 链；重复设置覆盖原映射。
- `viOpen`、`viParseRsrc` 与 `viParseRsrcEx` 通过同一 RM 解析路径识别 alias。展开名始终为 canonical resource；直接传 canonical resource 时，如存在 alias，也确定性返回该 alias。
- `viFindRsrc` 实现 VPP 属性表达式的逻辑与比较语法，但只对白名单中的静态全局属性求值。未知、局部或类型不匹配属性在编译表达式时失败，不通过 I/O 猜测。
- `viFindRsrc` 继续默认返回 canonical resource，不把 alias 当作第二个物理资源枚举，避免同一资源重复计数。

## 后果

这提供了可测试、跨平台且不依赖注册表或系统配置守护进程的兼容纵向切片。它不是系统级 VISA 配置数据库；alias 不跨 RM、不跨进程、不持久化。未来若引入持久化配置，必须新增 ADR 并保持这三个标准入口的一致解析语义。
