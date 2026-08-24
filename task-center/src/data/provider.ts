import { invoke } from "@tauri-apps/api/core";
import { fixtureEvents, fixtureProjects, fixtureSources } from "./fixtures";
import type {
  PriorityEditPreview,
  PriorityEditReceipt,
  PriorityEditRequest,
  CreateTaskPreview,
  CreateTaskReceipt,
  NewTaskDraft,
  ProjectMapping,
  RawTaskSource,
  TaskEvent,
  TaskFieldEditPreview,
  TaskFieldEditReceipt,
  TaskFieldEditRequest,
} from "../domain/types";

export interface TaskDataProvider {
  loadMetadata(): Promise<RawTaskSource[]>;
  loadBody(fileToken: string): Promise<string>;
  loadEvents(taskId: string): Promise<TaskEvent[]>;
  loadProjectMappings(): Promise<ProjectMapping[]>;
  previewPriorityEdit(fileToken: string, newPriority: "high" | "medium" | "low"): Promise<PriorityEditPreview>;
  applyPriorityEdit(request: PriorityEditRequest): Promise<PriorityEditReceipt>;
  previewTaskFieldEdit(fileToken: string, field: TaskFieldEditRequest["field"], newValue: unknown): Promise<TaskFieldEditPreview>;
  applyTaskFieldEdit(request: TaskFieldEditRequest): Promise<TaskFieldEditReceipt>;
  previewCreateTask(draft: NewTaskDraft): Promise<CreateTaskPreview>;
  applyCreateTask(preview: CreateTaskPreview): Promise<CreateTaskReceipt>;
}

const isTauri = () => typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;

const demoBodies: Record<string, string> = {
  "tsk_demo_governance.md": "# 统一个人 AI 规则与能力边界\n\n这是合成预览正文。正式应用只会在打开详情时读取正文。",
  "tsk_demo_backup.md": "# 完成独立备份与恢复演练\n\n当前等待备份介质。",
  "tsk_demo_keyboard.md": "# 完成 Mac 键盘决策级研究\n\n继续从已核验断点推进。",
  "tsk_demo_done.md": "# 建立安全同步\n\n双端接入已经完成。",
};

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
};
