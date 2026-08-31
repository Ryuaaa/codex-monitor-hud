export type TaskStatus = "todo" | "doing" | "long_term" | "done" | "cancelled" | "unknown";
export type TaskPriority = "high" | "medium" | "low" | "unknown";

export interface RawTaskSource {
  fileToken: string;
  frontmatter: string;
  error?: string;
}

export interface TaskRecord {
  id: string;
  title: string;
  status: TaskStatus;
  rawStatus: string;
  recordStatus: "current" | "archived" | "unknown";
  workflowStatus?: string;
  priority: TaskPriority;
  deadline?: string;
  updatedAt?: string;
  domain: string;
  category?: string;
  projectId?: string;
  assignee?: string;
  nextAction?: string;
  privacy: string;
  codexAccess: string;
  sourceRefs: string[];
  tags: string[];
  parentId?: string;
  blockedByIds: string[];
  relatedIds: string[];
  fileToken: string;
}

export interface TaskLoadIssue {
  fileToken: string;
  code: "bad_yaml" | "missing_id" | "restricted" | "bad_date" | "io_error";
  message: string;
}

export interface TaskEvent {
  id: string;
  taskId: string;
  eventType: string;
  occurredAt: string;
  previousTaskStatus?: string | null;
  newTaskStatus?: string | null;
  message?: string;
  author?: string;
}

export interface ProjectMapping {
  id: string;
  name: string;
  workdirs: string[];
}

export interface PriorityEditPreview {
  fileToken: string;
  taskId: string;
  beforePriority: string;
  afterPriority: string;
  expectedHash: string;
}

export interface PriorityEditRequest {
  fileToken: string;
  newPriority: "high" | "medium" | "low";
  expectedHash: string;
  confirmed: boolean;
}

export interface PriorityEditReceipt {
  fileToken: string;
  taskId: string;
  previousPriority: string;
  newPriority: "high" | "medium" | "low";
  fileHash: string;
  eventId: string;
  eventFile: string;
  verified: boolean;
}

export interface TaskWriteFailure {
  code: string;
  message: string;
}

export type WritableTaskField =
  | "title"
  | "task_status"
  | "priority"
  | "deadline"
  | "assignee"
  | "tags"
  | "parent_id"
  | "blocked_by_ids"
  | "related_ids"
  | "record_status";

export interface TaskFieldEditPreview {
  fileToken: string;
  taskId: string;
  field: WritableTaskField;
  beforeValue: unknown;
  afterValue: unknown;
  expectedHash: string;
}

export interface TaskFieldEditRequest {
  fileToken: string;
  field: WritableTaskField;
  newValue: unknown;
  expectedHash: string;
  confirmed: boolean;
}

export interface TaskFieldEditReceipt {
  fileToken: string;
  taskId: string;
  field: WritableTaskField;
  previousValue: unknown;
  newValue: unknown;
  fileHash: string;
  eventId: string;
  eventFile: string;
  verified: boolean;
}

export interface NewTaskDraft {
  title: string;
  domain: string;
  taskStatus: "todo" | "doing" | "long_term" | "done" | "cancelled";
  priority: "high" | "medium" | "low";
  assignee: string;
  deadline: string;
  tags: string[];
  parentId: string;
  blockedByIds: string[];
  relatedIds: string[];
}

export interface CreateTaskPreview {
  draft: NewTaskDraft;
  taskId: string;
  fileToken: string;
  createdAt: string;
  occurredAt: string;
  expectedHash: string;
}

export interface CreateTaskReceipt {
  taskId: string;
  fileToken: string;
  fileHash: string;
  eventId: string;
  eventFile: string;
  verified: boolean;
}

export type TaskNoteKind = "comment" | "activity";

export interface TaskNotePreview {
  fileToken: string;
  taskId: string;
  kind: TaskNoteKind;
  text: string;
  author: string;
  occurredAt: string;
  expectedTaskHash: string;
  expectedEventHash: string;
}

export interface TaskNoteRequest extends TaskNotePreview {
  confirmed: boolean;
}

export interface TaskNoteReceipt {
  taskId: string;
  eventId: string;
  eventFile: string;
  verified: boolean;
}

export interface TaskCenterUpdateInfo {
  currentVersion: string;
  available: boolean;
  version?: string;
}

export interface SavedTaskFilter {
  id: string;
  name: string;
  projectId: string;
  status: TaskStatus | "all";
  tag: string;
  showArchived: boolean;
  view: "board" | "list";
}

export interface SavedTaskFilterDraft {
  id?: string;
  name: string;
  projectId: string;
  status: TaskStatus | "all";
  tag: string;
  showArchived: boolean;
  view: "board" | "list";
}

export type RuntimeDisplayState =
  | "proven_active"
  | "known_not_loaded"
  | "external_unknown"
  | "provider_unavailable"
  | "stale";

export interface CodexRuntimeEvidence {
  threadId: string;
  providerOwnsInstance: boolean;
  status?: "active" | "idle" | "systemError" | "notLoaded";
  activeFlags?: string[];
  observedAt?: string;
  providerAvailable: boolean;
  stale: boolean;
}

export interface CodexTurnSummary {
  id: string;
  status: string;
  startedAt?: number;
  completedAt?: number;
  durationMs?: number;
}

export interface CodexThreadSummary {
  threadId: string;
  name?: string;
  sourceKind: string;
  sourceLabel: string;
  reportedStatus: string;
  createdAt?: number;
  updatedAt?: number;
  workspaceName?: string;
  isPinned: boolean;
  archived: boolean;
}

export type CodexThreadSourceGroup = "all" | "interactive" | "automation" | "subagents";

export interface CodexThreadListRequest {
  cursor?: string;
  archived: boolean;
  sourceGroup: CodexThreadSourceGroup;
  searchTerm?: string;
}

export interface CodexThreadListPage {
  threads: CodexThreadSummary[];
  nextCursor?: string;
  observedAt: number;
}

export interface CodexOpenReceipt {
  mode: "deepLink" | "openedAndCopied" | "openedOnly" | string;
  message: string;
}

export interface CodexThreadPage {
  threadId: string;
  name?: string;
  sourceKind: string;
  sourceLabel: string;
  reportedStatus: string;
  createdAt?: number;
  updatedAt?: number;
  historyMode?: string;
  turns: CodexTurnSummary[];
  nextCursor?: string;
  historyState: "paged" | "unsupported" | "error";
  historyMessage?: string;
  observedAt: number;
}

export interface CodexHistoryFailure {
  code: string;
  message: string;
}

export type CodexTurnState =
  | "starting"
  | "running"
  | "waitingApproval"
  | "interrupting"
  | "completed"
  | "failed"
  | "interrupted"
  | "timedOut";

export type CodexApprovalDecision = "accept" | "acceptForSession" | "decline" | "cancel";

export interface CodexApprovalPrompt {
  requestId: string;
  kind: "command" | "fileChange" | string;
  label: string;
  summary?: string;
  reason?: string;
  availableDecisions: CodexApprovalDecision[];
}

export interface CodexTurnSnapshot {
  sessionId: string;
  threadId: string;
  turnId?: string;
  state: CodexTurnState;
  startedAt: number;
  finishedAt?: number;
  errorMessage?: string;
  pendingApproval?: CodexApprovalPrompt;
}
