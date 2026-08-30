import type { CodexRuntimeEvidence, ProjectMapping, RawTaskSource, TaskEvent } from "../domain/types";

const yaml = (lines: string[]) => lines.join("\n");

export const fixtureSources: RawTaskSource[] = [
  {
    fileToken: "tsk_demo_governance.md",
    frontmatter: yaml([
      "task_id: tsk_demo_governance",
      "title: 统一个人 AI 规则与能力边界",
      "task_status: doing",
      "workflow_status: 等待下一轮真实回归证据",
      "priority: high",
      "deadline: 2026-08-24",
      "domain: 个人AI系统",
      "category: 系统治理",
      "project_id: prj_ai_lab",
      "assignee: AI治理总管",
      "next_action: 核验自然任务中的路由表现",
      "tags: [\"AI治理\", \"回归测试\"]",
      "parent_id:",
      "blocked_by_ids: [\"tsk_demo_backup\"]",
      "privacy: general",
      "codex_access: proposal_only",
      "source_refs:",
      "  - codex-thread:019f-demo-running",
    ]),
  },
  {
    fileToken: "tsk_demo_backup.md",
    frontmatter: yaml([
      "task_id: tsk_demo_backup",
      "title: 完成独立备份与恢复演练",
      "task_status: long_term",
      "workflow_status: 等待备份介质",
      "priority: high",
      "domain: 个人AI系统",
      "category: 备份与恢复",
      "project_id: prj_knowledge",
      "assignee: 本人",
      "next_action: 介质可用后开始",
      "tags: [\"备份\"]",
      "parent_id:",
      "blocked_by_ids: []",
      "privacy: general",
      "codex_access: proposal_only",
      "source_refs: []",
    ]),
  },
  {
    fileToken: "tsk_demo_keyboard.md",
    frontmatter: yaml([
      "task_id: tsk_demo_keyboard",
      "title: 完成 Mac 键盘决策级研究",
      "task_status: todo",
      "priority: medium",
      "deadline: 2026-09-15",
      "domain: 日常生活",
      "category: 电脑外设",
      "assignee: 本人",
      "tags: [\"采购研究\"]",
      "parent_id: tsk_demo_governance",
      "blocked_by_ids: []",
      "privacy: general",
      "codex_access: proposal_only",
      "source_refs:",
      "  - codex-thread:019f-demo-external",
    ]),
  },
  {
    fileToken: "tsk_demo_done.md",
    frontmatter: yaml([
      "task_id: tsk_demo_done",
      "title: 建立安全同步",
      "task_status: done",
      "priority: high",
      "domain: 个人AI系统",
      "project_id: prj_knowledge",
      "assignee: 本人",
      "tags: [\"知识库\"]",
      "parent_id:",
      "blocked_by_ids: []",
      "privacy: general",
      "codex_access: proposal_only",
      "source_refs: []",
    ]),
  },
];

export const fixtureProjects: ProjectMapping[] = [
  { id: "prj_ai_lab", name: "小烈刀 AI 协作实验室", workdirs: ["/workspace/ai-lab"] },
  { id: "prj_knowledge", name: "个人 AI 知识库", workdirs: ["/workspace/knowledge"] },
];

export const fixtureEvents: TaskEvent[] = [
  {
    id: "tevt_demo_1",
    taskId: "tsk_demo_governance",
    eventType: "created",
    occurredAt: "2026-08-20T09:00:00+08:00",
    newTaskStatus: "todo",
  },
  {
    id: "tevt_demo_2",
    taskId: "tsk_demo_governance",
    eventType: "started",
    occurredAt: "2026-08-21T10:30:00+08:00",
    previousTaskStatus: "todo",
    newTaskStatus: "doing",
  },
  {
    id: "tevt_demo_3",
    taskId: "tsk_demo_governance",
    eventType: "comment_added",
    occurredAt: "2026-08-22T11:00:00+08:00",
    message: "这是一条合成评论。",
    author: "本人",
  },
];

export const runtimeFixtures: CodexRuntimeEvidence[] = [
  { threadId: "running", providerOwnsInstance: true, status: "active", providerAvailable: true, stale: false },
  { threadId: "not-loaded", providerOwnsInstance: true, status: "notLoaded", providerAvailable: true, stale: false },
  { threadId: "external", providerOwnsInstance: false, providerAvailable: true, stale: false },
  { threadId: "failed", providerOwnsInstance: false, providerAvailable: false, stale: false },
];
