# Domain Docs

采用 single-context 布局：根目录 GLOSSARY.md 和 docs/adr/。

## 探索前读取

读取根目录 GLOSSARY.md，以及 docs/adr/ 中与当前领域相关的 ADR。
遵循 AGENTS.md 中对源码图谱和最近区域地图的读取要求。

领域文档不存在时直接继续，无须报告缺失或预建占位文件。
domain-modeling 在术语或决策明确后按需创建文档。

## 术语与决策

Issue 标题、设计建议、假设和测试名称使用 GLOSSARY.md 定义的术语。
需要的概念未收录时，先核实项目现有用语；确有缺口时留给
domain-modeling 补充。

方案与现有 ADR 冲突时，明确指出对应 ADR 和重新讨论的理由。
