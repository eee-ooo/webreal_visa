# webreal_visa AI 接续入口

本仓库由 AI 主导实现和维护。任何无历史上下文的维护者开始工作前，按以下顺序阅读：

1. `docs/status/current.md`：当前阶段、已完成项、阻塞项和下一步。
2. `docs/project/requirements.md`：中文权威需求与不可突破的边界。
3. `docs/architecture/overview.md`：运行时结构、依赖方向和并发模型。
4. `docs/architecture/code-map.md`：每个第一方代码、构建和测试文件的职责。
5. `docs/decisions/`：已冻结的技术决策；修改前新增 ADR，不覆盖历史。
6. `docs/compatibility/visa-compatibility.md`：VISA 兼容范围与差异。

基本纪律：中文文档是权威说明；不要进入 `current.md` 所列阶段之外的实现；不要把研究报告当作规范原文；新增、移动或删除第一方代码/构建/测试文件时同步更新 `code-map.md` 并运行 `tools/validate_docs.py`。
