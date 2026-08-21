# 字段权威与展示映射矩阵（P0）

P1 的统一规则：正式单任务 Markdown 负责当前状态，追加式 JSONL 负责历史，所连接的官方 Codex App Server 负责该实例运行态，本机项目映射配置负责目录对应关系。P1 对这些来源全部只读。

| 字段/能力 | 唯一权威位置 | P1 读取方式 | 展示映射 | P1 写入 | 冲突策略 | P2 回读验证与失败恢复 |
|---|---|---|---|---|---|---|
| 任务编号 | Markdown `task_id`，兼容 `id` | 只读 frontmatter | 稳定键 | 禁止 | 两者不同则隔离文件 | 期望哈希 + 唯一编号检查；不覆盖 |
| 标题 | Markdown `title` | 只读 frontmatter | 卡片/列表标题 | 禁止 | 缺失则显示“无标题”，不使用正文猜测 | 原子写后重读同字段；失败保留原文件 |
| 当前状态 | Markdown `task_status` | 只读 frontmatter | `todo→待处理`、`doing→进行中`、`long_term→长期`、`done→已完成`、`cancelled→已取消`、其他→未知 | 禁止 | JSONL 只作历史，不反盖当前值 | 先校验期望哈希，写 Markdown，再追加事件；任一步失败进入恢复记录 |
| 工作流说明 | Markdown `workflow_status` | 只读 frontmatter | 次要说明 | 禁止 | 不从事件推导覆盖 | 同上 |
| 优先级 | Markdown `priority` | 只读 frontmatter | `high/medium/low/unknown`；不硬映射 Dashi 数字等级 | 禁止 | 未知值原样标记待审核 | 写后回读枚举；失败回滚临时文件 |
| 截止日期 | Markdown `deadline` | ISO 日期/时间；无效值隔离字段 | 逾期/即将到期派生提示 | 禁止 | 以 Markdown 为准，使用本机时区显示 | 解析、时区、边界测试；原子替换 |
| 下一步 | Markdown `next_action` | 只读 frontmatter | 详情字段 | 禁止 | 不从正文或 AI 生成覆盖 | 写后逐字段重读 |
| 归属 | Markdown `domain/category/owner_scope/assignee/project_id` | 只读 frontmatter | 项目、领域、负责人筛选 | 禁止 | 项目映射与 `project_id` 不一致时并列提示 | 配置和任务分别带版本；不自动改任务 |
| 隐私/访问 | Markdown `privacy/codex_access` | 先读最小 frontmatter，再过滤 | `general` 可显示；`forbidden`、`explicit_only`、待分类默认不进入 P1 | 禁止 | 最严格规则优先 | 写能力不改变访问规则；拒绝写入受限记录 |
| 来源/关联 | Markdown `source_refs/related_ids` | 只读数组 | 对话绑定和关联计数 | 禁止 | 无法识别的引用保留原值，不猜测 | 仅经审核的字段扩展可写 |
| 正文 | 同一 Markdown frontmatter 后内容 | 仅用户打开详情时按路径令牌读取 | Markdown 纯文本展示 | 禁止 | 路径必须来自已扫描任务且仍在允许根目录 | 写前预览、期望哈希、临时文件、原子替换、回读 |
| 活动时间线 | `事件/YYYY-MM.jsonl` | 打开详情时按任务编号筛选 | 事件类型、时间、状态变化 | 禁止 | 坏行单独报告；不改变 Markdown | 成功写 Markdown 后追加事件；追加失败必须标记不完整并提供恢复 |
| Codex 对话编号 | Markdown `source_refs` 中 `codex-thread:` | 只读提取 | 绑定列表 | 禁止 | 不读取对话正文补标题 | 无写入计划 |
| Codex 当前运行态 | 所连接 App Server 的 `thread.status`/通知 | Provider 抽象；P1 默认固定样例/不可用降级 | 已证明运行、已知但未加载、外部实例未知、接口不可用 | 禁止 | 不用会话文件活跃度覆盖官方语义 | 无写入计划 |
| 工作目录映射 | 本机显式配置 | P1 内置默认映射；未来用户配置文件 | 项目切换 | 禁止正式任务；配置将来可写 | 配置缺失时显示未映射 | 配置独立版本、原子写、可恢复备份 |
| 搜索索引/已读/统计 | 可重建缓存 | P1 以内存派生，不落盘 | 搜索与计数 | 不写任务 | 删除缓存后可重建 | 若将来落盘，必须排除正文、隐私字段和密钥 |
| 评论、附件、重复、类型化依赖、开始日期、工作树绑定 | 正式结构目前未稳定定义 | P1 不读取为业务字段 | 显示“需要审核是否扩展正式结构” | 禁止 | 不塞入 SQLite/缓存伪造字段 | 先审核正式结构，再设计写入 |

## Dashi 状态只作参考

`backlog/todo/in_progress/in_review/blocked/done/canceled` 不等于正式状态。P1 看板只按正式状态分组；“需要关注/阻塞”只能从正式字段中的可靠文字或逾期规则派生，并标为提示，不创建新的状态。

