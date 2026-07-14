# CDDA 多人 fork 文档入口

`doc/multiplayer/` 集中保存 fork 专用的架构、构建、状态和交接记录。上游通用文档继续留在原有 `doc/` 结构中，避免把多人 fork 的长期决策散落到无关目录。

## 阅读顺序

开始新的多人开发任务时，按以下顺序读取：

1. [`AGENTS.md`](../../AGENTS.md)：操作约束、当前阶段门禁、工具链和交接规则。
2. [`STATUS.md`](STATUS.md)：最近一次工作快照、验证结果、阻塞项和下一步。
3. [`MULTIPLAYER_REFACTOR_PLAN.md`](MULTIPLAYER_REFACTOR_PLAN.md)：完整产品范围、目标架构、阶段路线和测试策略。
4. [`adr/README.md`](adr/README.md)：与当前任务相关的已接受或待验证决策。
5. [`MULTIPLAYER_BUILD_BASELINE.md`](MULTIPLAYER_BUILD_BASELINE.md)：涉及构建、CI、依赖或发布产物时必读。
6. [`DO_TURN_PHASE_AUDIT.md`](DO_TURN_PHASE_AUDIT.md)：涉及 turn loop、scheduler、world phase 或
   活动玩家 getter 时必读。

## 文档职责

| 文档 | 负责记录 | 不负责记录 |
| --- | --- | --- |
| `AGENTS.md` | 代理操作规则、阶段门禁、工程约束、常用命令 | 详细设计论证、逐次开发日志 |
| `STATUS.md` | 当前进度、证据、失败、未验证项、下一动作 | 永久架构决策、长期产品范围 |
| 重构计划 | 产品范围、总体架构、阶段、分层验证策略和退出标准 | 临时构建输出、单次调试过程 |
| ADR | 单项决策、替代方案、后果、go/no-go 门禁 | 每日进度、未经验证的实现宣称 |
| 构建基线 | 固定版本、构建命令、产物契约、已验证平台 | 多人玩法设计和协议细节 |
| turn phase 审计 | `do_turn()` 阶段边界、基线 profiling 和 world-phase getter 迁移清单 | 临时性能结论、已完成多人 scheduler 的宣称 |

## 更新规则

- 代码调查推翻计划假设时，先更新或新增 ADR，并同步重构计划，再扩展实现。
- 构建依赖、runner、ABI、产物名或验证状态变化时，同步更新构建基线。
- 每次完成可交接的工作单元时更新 `STATUS.md`，写明所选 Tier、选择原因、命令、实际结果和按策略未运行的
  平台，不只写“已测试”。
- `STATUS.md` 保持单一当前快照。重要历史通过 Git 和 ADR 保留，不堆积流水账。
- 未完成的 CI、未运行的平台和允许失败的 spike 必须准确分类：按 Tier 策略未运行/取消不自动构成 blocker，
  但不能冒充对应平台证据；真正要求而未通过的 gate 才列为阻塞。
- 文档链接使用仓库相对路径；移动多人文档时必须用 `rg` 检查并修复全部引用。

## 交接最小信息

后续开发者应能只读上述文档就回答：当前在哪个 phase、哪些门禁已有证据、有哪些已接受决策、工作区中正在验证什么、下一条应运行的命令是什么。缺少其中任一项时，先补文档再继续扩大改动范围。
