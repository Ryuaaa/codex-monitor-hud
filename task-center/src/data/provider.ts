import { invoke } from "@tauri-apps/api/core";
import { fixtureEvents, fixtureProjects, fixtureSources } from "./fixtures";
import type {
  PriorityEditPreview,
  PriorityEditReceipt,
  PriorityEditRequest,
  CodexThreadPage,
  CodexThreadListPage,
  CodexThreadListRequest,
  CodexOpenReceipt,
  CodexTurnSnapshot,
  CodexApprovalDecision,
  CreateTaskPreview,
  CreateTaskReceipt,
  NewTaskDraft,
  ProjectMapping,
  RawTaskSource,
  SavedTaskFilter,
  SavedTaskFilterDraft,
  TaskEvent,
  TaskFieldEditPreview,
  TaskFieldEditReceipt,
  TaskFieldEditRequest,
  TaskNoteKind,
  TaskNotePreview,
  TaskNoteReceipt,
  TaskCenterUpdateInfo,
} from "../domain/types";

export interface TaskDataProvider {
  loadMetadata(): Promise<RawTaskSource[]>;
  loadBody(fileToken: string): Promise<string>;
  loadEvents(taskId: string): Promise<TaskEvent[]>;
  loadProjectMappings(): Promise<ProjectMapping[]>;
  loadCodexThreadList(request: CodexThreadListRequest): Promise<CodexThreadListPage>;
  loadCodexThreadPage(threadId: string, cursor?: string): Promise<CodexThreadPage>;
  openCodexThread(threadId: string): Promise<CodexOpenReceipt>;
  startCodexTurn(threadId: string, input: string): Promise<CodexTurnSnapshot>;
  getCodexTurnStatus(sessionId: string): Promise<CodexTurnSnapshot>;
  respondCodexTurnApproval(sessionId: string, requestId: string, decision: CodexApprovalDecision): Promise<CodexTurnSnapshot>;
  interruptCodexTurn(sessionId: string): Promise<CodexTurnSnapshot>;
  initializeLocalTaskLibrary(): Promise<void>;
  loadSavedFilters(): Promise<SavedTaskFilter[]>;
  saveTaskFilter(draft: SavedTaskFilterDraft): Promise<SavedTaskFilter>;
  deleteTaskFilter(id: string): Promise<void>;
  previewPriorityEdit(fileToken: string, newPriority: "high" | "medium" | "low"): Promise<PriorityEditPreview>;
  applyPriorityEdit(request: PriorityEditRequest): Promise<PriorityEditReceipt>;
  previewTaskFieldEdit(fileToken: string, field: TaskFieldEditRequest["field"], newValue: unknown): Promise<TaskFieldEditPreview>;
  applyTaskFieldEdit(request: TaskFieldEditRequest): Promise<TaskFieldEditReceipt>;
  previewCreateTask(draft: NewTaskDraft): Promise<CreateTaskPreview>;
  applyCreateTask(preview: CreateTaskPreview): Promise<CreateTaskReceipt>;
  previewTaskNote(fileToken: string, kind: TaskNoteKind, text: string, author: string): Promise<TaskNotePreview>;
  applyTaskNote(preview: TaskNotePreview): Promise<TaskNoteReceipt>;
  checkTaskCenterUpdate(): Promise<TaskCenterUpdateInfo>;
  installTaskCenterUpdate(expectedVersion: string): Promise<void>;
}

const isTauri = () => typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;

const demoBodies: Record<string, string> = {
  "tsk_demo_governance.md": "# 统一个人 AI 规则与能力边界\n\n这是合成预览正文。正式应用只会在打开详情时读取正文。",
  "tsk_demo_backup.md": "# 完成独立备份与恢复演练\n\n当前等待备份介质。",
  "tsk_demo_keyboard.md": "# 完成 Mac 键盘决策级研究\n\n继续从已核验断点推进。",
  "tsk_demo_done.md": "# 建立安全同步\n\n双端接入已经完成。",
};

const demoCodexPage = (threadId: string, cursor?: string): CodexThreadPage => ({
  threadId,
  name: "合成 Codex 任务",
  sourceKind: "vscode",
  sourceLabel: "Codex 桌面版或编辑器",
  reportedStatus: "notLoaded",
  createdAt: 1787443200,
  updatedAt: 1787616000,
  historyMode: "paginated",
  turns: cursor ? [{ id: "turn_demo_older", status: "completed", startedAt: 1787529600, completedAt: 1787529900, durationMs: 300000 }]
    : [{ id: "turn_demo_latest", status: "completed", startedAt: 1787615700, completedAt: 1787616000, durationMs: 300000 }],
  nextCursor: cursor ? undefined : "demo-older-page",
  historyState: "paged",
  observedAt: 1787616060,
});

const demoCodexList = (request: CodexThreadListRequest): CodexThreadListPage => ({
  threads: request.cursor ? [{
    threadId: "019f-demo-older",
    name: "较早的 Codex 任务",
    sourceKind: "cli",
    sourceLabel: "Codex 命令行",
    reportedStatus: "notLoaded",
    createdAt: 1787529600,
    updatedAt: 1787529900,
    workspaceName: "demo-project",
    isPinned: false,
    archived: request.archived,
  }] : [{
    threadId: "019f-demo-running",
    name: "合成 Codex 任务",
    sourceKind: "vscode",
    sourceLabel: "Codex 桌面版或编辑器",
    reportedStatus: "notLoaded",
    createdAt: 1787615700,
    updatedAt: 1787616000,
    workspaceName: "codex-monitor",
    isPinned: true,
    archived: request.archived,
  }],
  nextCursor: request.cursor ? undefined : "demo-next-page",
  observedAt: 1787616060,
});

let demoSavedFilters: SavedTaskFilter[] = [];
let demoTurn: CodexTurnSnapshot | undefined;

export const taskDataProvider: TaskDataProvider = {
  async loadMetadata() {
    return isTauri() ? invoke<RawTaskSource[]>("load_task_metadata") : fixtureSources;
  },
  async loadBody(fileToken) {
    return isTauri() ? invoke<string>("load_task_body", { fileToken }) : (demoBodies[fileToken] ?? "正文不可用");
  },
  async loadEvents(taskId) {
    return isTauri() ? invoke<TaskEvent[]>("load_task_events", { taskId }) : fixtureEvents.filter((event) => event.taskId === taskId);
  },
  async loadProjectMappings() {
    return isTauri() ? invoke<ProjectMapping[]>("load_project_mappings") : fixtureProjects;
  },
  async loadCodexThreadList(request) {
    return isTauri()
      ? invoke<CodexThreadListPage>("load_codex_thread_list", { request })
      : demoCodexList(request);
  },
  async loadCodexThreadPage(threadId, cursor) {
    return isTauri()
      ? invoke<CodexThreadPage>("load_codex_thread_page", { threadId, cursor: cursor ?? null })
      : demoCodexPage(threadId, cursor);
  },
  async openCodexThread(threadId) {
    return isTauri()
      ? invoke<CodexOpenReceipt>("open_codex_thread", { threadId })
      : { mode: "deepLink", message: `已在 Codex 中打开 ${threadId}` };
  },
  async startCodexTurn(threadId, input) {
    if (isTauri()) return invoke<CodexTurnSnapshot>("start_codex_turn", { threadId, input });
    demoTurn = {
      sessionId: `demo-turn-${Date.now()}`,
      threadId,
      turnId: "demo-turn-id",
      state: "completed",
      startedAt: Math.floor(Date.now() / 1000),
      finishedAt: Math.floor(Date.now() / 1000),
    };
    return demoTurn;
  },
  async getCodexTurnStatus(sessionId) {
    if (isTauri()) return invoke<CodexTurnSnapshot>("get_codex_turn_status", { sessionId });
    if (!demoTurn || demoTurn.sessionId !== sessionId) throw new Error("没有找到合成继续任务");
    return demoTurn;
  },
  async respondCodexTurnApproval(sessionId, requestId, decision) {
    if (isTauri()) {
      return invoke<CodexTurnSnapshot>("respond_codex_turn_approval", { sessionId, requestId, decision });
    }
    if (!demoTurn || demoTurn.sessionId !== sessionId) throw new Error("没有找到合成继续任务");
    demoTurn = { ...demoTurn, state: "running", pendingApproval: undefined };
    return demoTurn;
  },
  async interruptCodexTurn(sessionId) {
    if (isTauri()) return invoke<CodexTurnSnapshot>("interrupt_codex_turn", { sessionId });
    if (!demoTurn || demoTurn.sessionId !== sessionId) throw new Error("没有找到合成继续任务");
    demoTurn = {
      ...demoTurn,
      state: "interrupted",
      finishedAt: Math.floor(Date.now() / 1000),
      pendingApproval: undefined,
    };
    return demoTurn;
  },
  async initializeLocalTaskLibrary() {
    if (!isTauri()) return;
    return invoke<void>("initialize_local_task_library");
  },
  async loadSavedFilters() {
    return isTauri() ? invoke<SavedTaskFilter[]>("load_saved_task_filters") : [...demoSavedFilters];
  },
  async saveTaskFilter(draft) {
    if (isTauri()) return invoke<SavedTaskFilter>("save_task_filter", { draft });
    const saved: SavedTaskFilter = {
      ...draft,
      id: draft.id ?? `filter-demo-${demoSavedFilters.length + 1}`,
    };
    demoSavedFilters = [...demoSavedFilters.filter((item) => item.id !== saved.id), saved];
    return saved;
  },
  async deleteTaskFilter(id) {
    if (isTauri()) return invoke<void>("delete_task_filter", { id });
    demoSavedFilters = demoSavedFilters.filter((item) => item.id !== id);
  },
  async previewPriorityEdit(fileToken, newPriority) {
    if (!isTauri()) throw new Error("浏览器合成预览不执行文件写入");
    return invoke<PriorityEditPreview>("preview_priority_edit", { fileToken, newPriority });
  },
  async applyPriorityEdit(request) {
    if (!isTauri()) throw new Error("浏览器合成预览不执行文件写入");
    return invoke<PriorityEditReceipt>("apply_priority_edit", { request });
  },
  async previewTaskFieldEdit(fileToken, field, newValue) {
    if (!isTauri()) throw new Error("浏览器合成预览不执行文件写入");
    return invoke<TaskFieldEditPreview>("preview_task_field_edit", { fileToken, field, newValue });
  },
  async applyTaskFieldEdit(request) {
    if (!isTauri()) throw new Error("浏览器合成预览不执行文件写入");
    return invoke<TaskFieldEditReceipt>("apply_task_field_edit", { request });
  },
  async previewCreateTask(draft) {
    if (!isTauri()) throw new Error("浏览器合成预览不执行文件写入");
    return invoke<CreateTaskPreview>("preview_create_task", { draft });
  },
  async applyCreateTask(preview) {
    if (!isTauri()) throw new Error("浏览器合成预览不执行文件写入");
    return invoke<CreateTaskReceipt>("apply_create_task", { request: { preview, confirmed: true } });
  },
  async previewTaskNote(fileToken, kind, text, author) {
    if (!isTauri()) return {
      fileToken,
      taskId: "tsk_demo_governance",
      kind,
      text,
      author,
      occurredAt: new Date().toISOString(),
      expectedTaskHash: "demo-task-hash",
      expectedEventHash: "demo-event-hash",
    };
    return invoke<TaskNotePreview>("preview_task_note", { fileToken, kind, text, author });
  },
  async applyTaskNote(preview) {
    if (!isTauri()) return {
      taskId: preview.taskId,
      eventId: "evt_demo_note",
      eventFile: preview.occurredAt.slice(0, 7) + ".jsonl",
      verified: true,
    };
    return invoke<TaskNoteReceipt>("apply_task_note", { request: { ...preview, confirmed: true } });
  },
  async checkTaskCenterUpdate() {
    return isTauri()
      ? invoke<TaskCenterUpdateInfo>("check_task_center_update")
      : { currentVersion: "1.2.1", available: false };
  },
  async installTaskCenterUpdate(expectedVersion) {
    if (!isTauri()) throw new Error("浏览器合成预览不安装更新");
    return invoke<void>("install_task_center_update", { expectedVersion });
  },
};
