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
