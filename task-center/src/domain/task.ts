import YAML from "yaml";
import type {
  CodexRuntimeEvidence,
  RawTaskSource,
  RuntimeDisplayState,
  TaskLoadIssue,
  TaskPriority,
  TaskRecord,
  TaskStatus,
} from "./types";

const statuses = new Set<TaskStatus>(["todo", "doing", "long_term", "done", "cancelled"]);
const priorities = new Set<TaskPriority>(["high", "medium", "low"]);
const blockedTerms = ["阻塞", "等待", "待决定", "需要确认", "blocked"];

const text = (value: unknown): string | undefined =>
  typeof value === "string" && value.trim() ? value.trim() : undefined;

const list = (value: unknown): string[] =>
  Array.isArray(value) ? value.filter((item): item is string => typeof item === "string") : [];

export function normalizeStatus(value: unknown): { status: TaskStatus; raw: string } {
  const raw = text(value) ?? "unknown";
  return { status: statuses.has(raw as TaskStatus) ? (raw as TaskStatus) : "unknown", raw };
}

export function parseTask(source: RawTaskSource): { task?: TaskRecord; issue?: TaskLoadIssue } {
  if (source.error) {
    return { issue: { fileToken: source.fileToken, code: "io_error", message: "元数据读取失败，已隔离该文件" } };
  }
  let data: Record<string, unknown>;
  try {
    const parsed = YAML.parse(source.frontmatter);
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) throw new Error("not an object");
    data = parsed as Record<string, unknown>;
  } catch {
    return { issue: { fileToken: source.fileToken, code: "bad_yaml", message: "元数据格式损坏" } };
  }

  const privacy = text(data.privacy) ?? "pending_classification";
  const codexAccess = text(data.codex_access) ?? "explicit_only";
  if (privacy !== "general" || codexAccess === "forbidden" || codexAccess === "explicit_only") {
    return { issue: { fileToken: source.fileToken, code: "restricted", message: "已按隐私或访问规则隐藏" } };
  }

  const id = text(data.task_id) ?? text(data.id);
  if (!id) return { issue: { fileToken: source.fileToken, code: "missing_id", message: "缺少任务编号" } };
  const normalized = normalizeStatus(data.task_status);
  const priorityRaw = text(data.priority) ?? "unknown";
  const deadline = text(data.deadline);
  if (deadline && Number.isNaN(Date.parse(deadline))) {
    return { issue: { fileToken: source.fileToken, code: "bad_date", message: "截止日期无法解析" } };
  }

  return {
    task: {
      id,
      title: text(data.title) ?? "无标题任务",
      status: normalized.status,
      rawStatus: normalized.raw,
      workflowStatus: text(data.workflow_status),
      priority: priorities.has(priorityRaw as TaskPriority) ? (priorityRaw as TaskPriority) : "unknown",
      deadline,
      updatedAt: text(data.updated_at),
      domain: text(data.domain) ?? "未归类",
      category: text(data.category),
      projectId: text(data.project_id),
      assignee: text(data.assignee),
      nextAction: text(data.next_action),
      privacy,
      codexAccess,
      sourceRefs: list(data.source_refs),
      relatedIds: list(data.related_ids),
      fileToken: source.fileToken,
    },
  };
}

export function deriveAttention(task: TaskRecord, now = new Date()): string[] {
  const hints: string[] = [];
  if (task.deadline) {
    const due = new Date(task.deadline);
    const deltaDays = (due.getTime() - now.getTime()) / 86_400_000;
    if (deltaDays < 0 && task.status !== "done" && task.status !== "cancelled") hints.push("已逾期");
    else if (deltaDays >= 0 && deltaDays <= 3 && task.status !== "done" && task.status !== "cancelled") hints.push("即将到期");
  }
  const explainableText = `${task.workflowStatus ?? ""} ${task.nextAction ?? ""}`.toLowerCase();
  if (blockedTerms.some((term) => explainableText.includes(term.toLowerCase()))) hints.push("需要关注");
  if (task.status === "unknown") hints.push("状态待核验");
  return [...new Set(hints)];
}

export function runtimeDisplayState(evidence: CodexRuntimeEvidence): RuntimeDisplayState {
  if (!evidence.providerAvailable) return "provider_unavailable";
  if (evidence.stale) return "stale";
  if (!evidence.providerOwnsInstance) return "external_unknown";
  if (evidence.status === "active") return "proven_active";
  if (evidence.status === "notLoaded") return "known_not_loaded";
  return "external_unknown";
}

export function codexThreadIds(task: TaskRecord): string[] {
  return task.sourceRefs
    .filter((ref) => ref.startsWith("codex-thread:"))
    .map((ref) => ref.slice("codex-thread:".length).split("#")[0])
    .filter(Boolean);
}
