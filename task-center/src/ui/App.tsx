import { useEffect, useMemo, useRef, useState } from "react";
import type { CSSProperties } from "react";
import { codexThreadIds, deriveAttention, parseTask } from "../domain/task";
import type {
  PriorityEditPreview,
  PriorityEditReceipt,
  CodexHistoryFailure,
  CodexOpenReceipt,
  CodexThreadListPage,
  CodexThreadListRequest,
  CodexThreadSourceGroup,
  CodexThreadPage,
  CodexThreadSummary,
  CodexTurnSnapshot,
  CodexApprovalDecision,
  CodexTurnSummary,
  CodexRuntimeCapabilities,
  CodexGlobalSettingsRequest,
  CodexGlobalSettingsPreview,
  CodexGlobalSettingsReceipt,
  CodexThreadRuntimeSetting,
  CodexThreadRuntimeOverride,
  CreateTaskPreview,
  CreateTaskReceipt,
  NewTaskDraft,
  ProjectMapping,
  SavedTaskFilter,
  SavedTaskFilterDraft,
  TaskEvent,
  TaskLoadIssue,
  TaskPriority,
  TaskRecord,
  TaskStatus,
  TaskFieldEditPreview,
  TaskFieldEditReceipt,
  TaskWriteFailure,
  TaskNoteKind,
  TaskNotePreview,
  TaskNoteReceipt,
  TaskCenterUpdateInfo,
  WritableTaskField,
} from "../domain/types";
import { taskDataProvider, type TaskDataProvider } from "../data/provider";

const statusLabels: Record<TaskStatus, string> = {
  todo: "待处理",
  doing: "进行中",
  long_term: "长期",
  done: "已完成",
  cancelled: "已取消",
  unknown: "未知",
};
const columns: TaskStatus[] = ["todo", "doing", "long_term", "done", "cancelled", "unknown"];
const priorityLabels = { high: "高", medium: "中", low: "低", unknown: "未知" } as const;
const updateCheckStorageKey = "codex-monitor-task-center.last-update-check";
const updateCheckIntervalMs = 24 * 60 * 60 * 1000;
const onboardingStorageKey = "codex-monitor-task-center.onboarding-completed.v1";
const appearanceStorageKey = "codex-monitor-task-center.appearance.v1";
const runtimeRestoreStorageKey = "codex-monitor-task-center.runtime-restore.v1";
const runtimeActiveStorageKey = "codex-monitor-task-center.runtime-active.v1";
const fallbackUpdateError = "更新未安装；当前版本保持不变。请稍后重试。";

type AppearanceTheme = "midnight" | "slate" | "light" | "highContrast";
type AppearanceFont = "system" | "rounded" | "serif" | "mono";
type AppearanceDensity = "comfortable" | "compact" | "relaxed";
type AppearanceLineSpacing = "compact" | "comfortable" | "relaxed";

interface AppearanceSettings {
  theme: AppearanceTheme;
  accent: string;
  font: AppearanceFont;
  fontScale: number;
  density: AppearanceDensity;
  lineSpacing: AppearanceLineSpacing;
  reduceMotion: boolean;
}

const defaultAppearance: AppearanceSettings = {
  theme: "midnight",
  accent: "#4fa9ff",
  font: "system",
  fontScale: 100,
  density: "comfortable",
  lineSpacing: "comfortable",
  reduceMotion: false,
};

const fontStacks: Record<AppearanceFont, string> = {
  system: 'Inter, "SF Pro Display", "PingFang SC", system-ui, sans-serif',
  rounded: 'ui-rounded, "SF Pro Rounded", "PingFang SC", system-ui, sans-serif',
  serif: '"New York", "Songti SC", "STSong", serif',
  mono: '"SFMono-Regular", Menlo, Monaco, "PingFang SC", monospace',
};

function loadAppearance(): AppearanceSettings {
  try {
    const parsed = JSON.parse(localStorage.getItem(appearanceStorageKey) ?? "null") as Partial<AppearanceSettings> | null;
    if (!parsed || typeof parsed !== "object") return defaultAppearance;
    return {
      theme: ["midnight", "slate", "light", "highContrast"].includes(String(parsed.theme)) ? parsed.theme as AppearanceTheme : defaultAppearance.theme,
      accent: typeof parsed.accent === "string" && /^#[0-9a-f]{6}$/i.test(parsed.accent) ? parsed.accent : defaultAppearance.accent,
      font: ["system", "rounded", "serif", "mono"].includes(String(parsed.font)) ? parsed.font as AppearanceFont : defaultAppearance.font,
      fontScale: typeof parsed.fontScale === "number" && parsed.fontScale >= 80 && parsed.fontScale <= 160 ? parsed.fontScale : defaultAppearance.fontScale,
      density: ["comfortable", "compact", "relaxed"].includes(String(parsed.density)) ? parsed.density as AppearanceDensity : defaultAppearance.density,
      lineSpacing: ["compact", "comfortable", "relaxed"].includes(String(parsed.lineSpacing)) ? parsed.lineSpacing as AppearanceLineSpacing : defaultAppearance.lineSpacing,
      reduceMotion: typeof parsed.reduceMotion === "boolean" ? parsed.reduceMotion : defaultAppearance.reduceMotion,
    };
  } catch {
    return defaultAppearance;
  }
}

function loadRuntimeRestore(): CodexThreadRuntimeSetting[] {
  try {
    const parsed = JSON.parse(localStorage.getItem(runtimeRestoreStorageKey) ?? "[]") as unknown;
    if (!Array.isArray(parsed) || parsed.length > 500) return [];
    return parsed.filter((item): item is CodexThreadRuntimeSetting => {
      if (!item || typeof item !== "object") return false;
      const value = item as Record<string, unknown>;
      return typeof value.threadId === "string" && typeof value.model === "string"
        && (value.effort === null || typeof value.effort === "string")
        && (value.serviceTier === null || typeof value.serviceTier === "string")
        && typeof value.effortChanged === "boolean"
        && typeof value.serviceTierChanged === "boolean";
    });
  } catch {
    return [];
  }
}

function loadRuntimeOverrides(): CodexThreadRuntimeOverride[] {
  try {
    const parsed = JSON.parse(localStorage.getItem(runtimeActiveStorageKey) ?? "[]") as unknown;
    if (!Array.isArray(parsed) || parsed.length > 500) return [];
    return parsed.filter((item): item is CodexThreadRuntimeOverride => {
      if (!item || typeof item !== "object") return false;
      const value = item as Record<string, unknown>;
      return typeof value.threadId === "string"
        && typeof value.effortSet === "boolean"
        && (value.effort === null || typeof value.effort === "string")
        && typeof value.serviceTierSet === "boolean"
        && (value.serviceTier === null || typeof value.serviceTier === "string");
    });
  } catch {
    return [];
  }
}

function mergeRuntimeOverrides(
  current: CodexThreadRuntimeOverride[],
  updates: CodexThreadRuntimeOverride[],
): CodexThreadRuntimeOverride[] {
  const byThread = new Map(current.map((item) => [item.threadId, item]));
  for (const update of updates) {
    const previous = byThread.get(update.threadId);
    const merged: CodexThreadRuntimeOverride = {
      threadId: update.threadId,
      effortSet: update.effortSet || Boolean(previous?.effortSet),
      effort: update.effortSet ? update.effort : (previous?.effort ?? null),
      serviceTierSet: update.serviceTierSet || Boolean(previous?.serviceTierSet),
      serviceTier: update.serviceTierSet ? update.serviceTier : (previous?.serviceTier ?? null),
    };
    if (merged.effortSet || merged.serviceTierSet) byThread.set(update.threadId, merged);
    else byThread.delete(update.threadId);
  }
  return [...byThread.values()].slice(-500);
}

function contrastText(color: string): "#071018" | "#ffffff" {
  const red = Number.parseInt(color.slice(1, 3), 16);
  const green = Number.parseInt(color.slice(3, 5), 16);
  const blue = Number.parseInt(color.slice(5, 7), 16);
  const luminance = (0.2126 * red + 0.7152 * green + 0.0722 * blue) / 255;
  return luminance > 0.58 ? "#071018" : "#ffffff";
}

function safeUpdateError(error: unknown): string {
  if (
    typeof error === "string" &&
    error.length <= 160 &&
    /^(更新|当前|可安装版本|暂时无法)/.test(error)
  ) {
    return error;
  }
  return fallbackUpdateError;
}

interface CodexHistoryView {
  page?: CodexThreadPage;
  turns: CodexTurnSummary[];
  loading: boolean;
  failure?: CodexHistoryFailure;
}

type AppSection = "codex" | "managed";
type ManagedLoadState = "idle" | "loading" | "ready" | "error";

export function App({ provider = taskDataProvider }: { provider?: TaskDataProvider }) {
  const [section, setSection] = useState<AppSection>("codex");
  const [tasks, setTasks] = useState<TaskRecord[]>([]);
  const [issues, setIssues] = useState<TaskLoadIssue[]>([]);
  const [projects, setProjects] = useState<ProjectMapping[]>([]);
  const [selectedProject, setSelectedProject] = useState("all");
  const [view, setView] = useState<"board" | "list">("board");
  const [query, setQuery] = useState("");
  const [codexQuery, setCodexQuery] = useState("");
  const [codexAppliedSearch, setCodexAppliedSearch] = useState("");
  const [codexSourceGroup, setCodexSourceGroup] = useState<CodexThreadSourceGroup>("interactive");
  const [codexArchived, setCodexArchived] = useState(false);
  const [statusFilter, setStatusFilter] = useState<TaskStatus | "all">("all");
  const [tagFilter, setTagFilter] = useState("");
  const [savedFilters, setSavedFilters] = useState<SavedTaskFilter[]>([]);
  const [selectedSavedFilterId, setSelectedSavedFilterId] = useState("");
  const [filterNameDraft, setFilterNameDraft] = useState("");
  const [filterEditorOpen, setFilterEditorOpen] = useState(false);
  const [filterSaving, setFilterSaving] = useState(false);
  const [filterFailure, setFilterFailure] = useState<string>();
  const [appearance, setAppearance] = useState<AppearanceSettings>(loadAppearance);
  const [appearanceOpen, setAppearanceOpen] = useState(false);
  const [showArchived, setShowArchived] = useState(false);
  const [selected, setSelected] = useState<TaskRecord>();
  const [body, setBody] = useState<string>();
  const [events, setEvents] = useState<TaskEvent[]>([]);
  const [loadingDetail, setLoadingDetail] = useState(false);
  const [loadError, setLoadError] = useState<string>();
  const [managedLoadState, setManagedLoadState] = useState<ManagedLoadState>("idle");
  const [libraryInitializing, setLibraryInitializing] = useState(false);
  const [codexThreads, setCodexThreads] = useState<CodexThreadSummary[]>([]);
  const [codexListPage, setCodexListPage] = useState<CodexThreadListPage>();
  const [codexListLoading, setCodexListLoading] = useState(false);
  const [codexListFailure, setCodexListFailure] = useState<CodexHistoryFailure>();
  const [selectedCodex, setSelectedCodex] = useState<CodexThreadSummary>();
  const [codexContinueDraft, setCodexContinueDraft] = useState("");
  const [codexContinuePreview, setCodexContinuePreview] = useState<string>();
  const [codexTurn, setCodexTurn] = useState<CodexTurnSnapshot>();
  const [codexTurnOwner, setCodexTurnOwner] = useState<CodexThreadSummary>();
  const [codexTurnFailure, setCodexTurnFailure] = useState<CodexHistoryFailure>();
  const [codexPollFailures, setCodexPollFailures] = useState(0);
  const [codexOpenBusy, setCodexOpenBusy] = useState(false);
  const [codexOpenReceipt, setCodexOpenReceipt] = useState<CodexOpenReceipt>();
  const [codexOpenFailure, setCodexOpenFailure] = useState<CodexHistoryFailure>();
  const [priorityDraft, setPriorityDraft] = useState<"high" | "medium" | "low">();
  const [writePreview, setWritePreview] = useState<PriorityEditPreview>();
  const [writeReceipt, setWriteReceipt] = useState<PriorityEditReceipt>();
  const [writeFailure, setWriteFailure] = useState<TaskWriteFailure>();
  const [writing, setWriting] = useState(false);
  const [fieldDraft, setFieldDraft] = useState<{ field: WritableTaskField; rawValue: string }>();
  const [fieldPreview, setFieldPreview] = useState<TaskFieldEditPreview>();
  const [fieldReceipt, setFieldReceipt] = useState<TaskFieldEditReceipt>();
  const [noteDraft, setNoteDraft] = useState<{ kind: TaskNoteKind; text: string; author: string }>({ kind: "comment", text: "", author: "本人" });
  const [notePreview, setNotePreview] = useState<TaskNotePreview>();
  const [noteReceipt, setNoteReceipt] = useState<TaskNoteReceipt>();
  const [noteFailure, setNoteFailure] = useState<TaskWriteFailure>();
  const [updateInfo, setUpdateInfo] = useState<TaskCenterUpdateInfo>();
  const [updateBusy, setUpdateBusy] = useState(false);
  const [updateMessage, setUpdateMessage] = useState<string>();
  const [updateFailed, setUpdateFailed] = useState(false);
  const [onboardingOpen, setOnboardingOpen] = useState(() => {
    try { return localStorage.getItem(onboardingStorageKey) !== "1"; } catch { return true; }
  });
  const [runtimeOpen, setRuntimeOpen] = useState(false);
  const [runtimeCapabilities, setRuntimeCapabilities] = useState<CodexRuntimeCapabilities>();
  const [runtimeRequest, setRuntimeRequest] = useState<CodexGlobalSettingsRequest>({
    scope: "allActive",
    threadIds: [],
    reasoningSelection: "keep",
    speedSelection: "keep",
  });
  const [runtimePreview, setRuntimePreview] = useState<CodexGlobalSettingsPreview>();
  const [runtimeReceipt, setRuntimeReceipt] = useState<CodexGlobalSettingsReceipt>();
  const [runtimeRestore, setRuntimeRestore] = useState<CodexThreadRuntimeSetting[]>(loadRuntimeRestore);
  const [runtimeOverrides, setRuntimeOverrides] = useState<CodexThreadRuntimeOverride[]>(loadRuntimeOverrides);
  const [runtimeRestoreArmed, setRuntimeRestoreArmed] = useState(false);
  const [runtimeBusy, setRuntimeBusy] = useState(false);
  const [runtimeFailure, setRuntimeFailure] = useState<string>();
  const [createOpen, setCreateOpen] = useState(false);
  const [createDraft, setCreateDraft] = useState<NewTaskDraft>({
    title: "", domain: "task_hub", taskStatus: "todo", priority: "medium",
    assignee: "本人", deadline: "", tags: [], parentId: "", blockedByIds: [], relatedIds: [],
  });
  const [createPreview, setCreatePreview] = useState<CreateTaskPreview>();
  const [createReceipt, setCreateReceipt] = useState<CreateTaskReceipt>();
  const [createFailure, setCreateFailure] = useState<TaskWriteFailure>();
  const [codexHistory, setCodexHistory] = useState<Record<string, CodexHistoryView>>({});
  const codexHistoryGeneration = useRef(0);
  const codexListGeneration = useRef(0);
  const managedLoadGeneration = useRef(0);
  const taskDetailGeneration = useRef(0);

  useEffect(() => {
    void loadCodexThreads(undefined, true);
    let lastCheck = 0;
    try { lastCheck = Number(localStorage.getItem(updateCheckStorageKey) ?? "0"); } catch { /* 本地偏好不可用时仍允许手动检查 */ }
    if (!Number.isFinite(lastCheck) || Date.now() - lastCheck >= updateCheckIntervalMs) {
      void checkForUpdate(false);
    }
    return () => {
      codexListGeneration.current += 1;
      codexHistoryGeneration.current += 1;
      managedLoadGeneration.current += 1;
      taskDetailGeneration.current += 1;
    };
  }, [provider]);

  useEffect(() => {
    try { localStorage.setItem(appearanceStorageKey, JSON.stringify(appearance)); } catch { /* 本机偏好不可写时仅本次会话生效 */ }
  }, [appearance]);

  useEffect(() => {
    if (section === "managed" && managedLoadState === "idle") {
      void loadManagedTasks();
    }
  }, [section, managedLoadState]);

  useEffect(() => {
    if (!codexTurn || codexPollFailures >= 3 || ["completed", "failed", "interrupted", "timedOut"].includes(codexTurn.state)) return;
    let cancelled = false;
    const timer = window.setTimeout(async () => {
      try {
        const next = await provider.getCodexTurnStatus(codexTurn.sessionId);
        if (!cancelled) {
          setCodexTurn(next);
          setCodexTurnFailure(undefined);
          setCodexPollFailures(0);
        }
      } catch (error) {
        if (!cancelled) {
          setCodexTurnFailure(normalizeCodexHistoryError(error));
          setCodexPollFailures((current) => current + 1);
        }
      }
    }, 700);
    return () => {
      cancelled = true;
      window.clearTimeout(timer);
    };
  }, [codexTurn, codexPollFailures, provider]);

  const filtered = useMemo(() => {
    const normalized = query.trim().toLowerCase();
    return tasks.filter((task) => {
      const projectMatch = selectedProject === "all" || task.projectId === selectedProject || task.domain === selectedProject;
      const statusMatch = statusFilter === "all" || task.status === statusFilter;
      const archiveMatch = showArchived || task.recordStatus !== "archived";
      const tagMatch = !tagFilter || task.tags.includes(tagFilter);
      const searchMatch = !normalized || [task.title, task.domain, task.category, task.assignee, task.workflowStatus, ...task.tags]
        .filter(Boolean).join(" ").toLowerCase().includes(normalized);
      return projectMatch && statusMatch && archiveMatch && tagMatch && searchMatch;
    });
  }, [tasks, query, selectedProject, statusFilter, tagFilter, showArchived]);

  const allTags = useMemo(() => [...new Set(tasks.flatMap((task) => task.tags))]
    .sort((left, right) => left.localeCompare(right, "zh-CN")), [tasks]);

  const filteredCodexThreads = codexThreads;

  const appearanceStyle = useMemo(() => {
    const scale = appearance.fontScale / 100;
    const scaled = (value: number) => `${Math.round(value * scale * 100) / 100}px`;
    return ({
    "--user-accent": appearance.accent,
    "--accent-contrast": contrastText(appearance.accent),
    "--user-font": fontStacks[appearance.font],
    "--content-leading": appearance.lineSpacing === "compact" ? "1.35" : appearance.lineSpacing === "relaxed" ? "1.8" : "1.58",
    "--font-9": scaled(9),
    "--font-10": scaled(10),
    "--font-11": scaled(11),
    "--font-12": scaled(12),
    "--font-13": scaled(13),
    "--font-14": scaled(14),
    "--font-16": scaled(16),
    "--font-20": scaled(20),
    "--font-22": scaled(22),
    "--font-24": scaled(24),
    "--font-25": scaled(25),
  } as CSSProperties);
  }, [appearance]);

  const projectOptions = useMemo(() => {
    const mapped = projects.map((project) => ({ id: project.id, name: project.name, workdirs: project.workdirs }));
    const mappedIds = new Set(mapped.map((project) => project.id));
    const domains = [...new Set(tasks.filter((task) => !task.projectId || !mappedIds.has(task.projectId)).map((task) => task.domain))]
      .map((domain) => ({ id: domain, name: domain, workdirs: [] as string[] }));
    return [...mapped, ...domains];
  }, [projects, tasks]);

  async function loadCodexThreads(
    cursor?: string,
    replace = false,
    overrides: Partial<Omit<CodexThreadListRequest, "cursor">> = {},
  ) {
    const generation = codexListGeneration.current + 1;
    codexListGeneration.current = generation;
    setCodexListLoading(true);
    setCodexListFailure(undefined);
    if (replace) {
      setCodexThreads([]);
      setCodexListPage(undefined);
    }
    try {
      const request: CodexThreadListRequest = {
        cursor,
        archived: overrides.archived ?? codexArchived,
        sourceGroup: overrides.sourceGroup ?? codexSourceGroup,
        searchTerm: Object.prototype.hasOwnProperty.call(overrides, "searchTerm")
          ? overrides.searchTerm
          : (codexAppliedSearch || undefined),
      };
      const page = await provider.loadCodexThreadList(request);
      if (generation !== codexListGeneration.current) return;
      setCodexThreads((current) => {
        const previous = replace ? [] : current;
        const seen = new Set(previous.map((thread) => thread.threadId));
        return [...previous, ...page.threads.filter((thread) => !seen.has(thread.threadId))];
      });
      setCodexListPage(page);
    } catch (error) {
      if (generation !== codexListGeneration.current) return;
      setCodexListFailure(normalizeCodexHistoryError(error));
    } finally {
      if (generation === codexListGeneration.current) setCodexListLoading(false);
    }
  }

  function applyCodexSearch() {
    const searchTerm = codexQuery.trim();
    setCodexAppliedSearch(searchTerm);
    void loadCodexThreads(undefined, true, { searchTerm: searchTerm || undefined });
  }

  function changeCodexSourceGroup(sourceGroup: CodexThreadSourceGroup) {
    setCodexSourceGroup(sourceGroup);
    closeDetails();
    void loadCodexThreads(undefined, true, { sourceGroup });
  }

  function changeCodexArchive(archived: boolean) {
    setCodexArchived(archived);
    closeDetails();
    void loadCodexThreads(undefined, true, { archived });
  }

  async function loadManagedTasks() {
    const generation = managedLoadGeneration.current + 1;
    managedLoadGeneration.current = generation;
    setManagedLoadState("loading");
    setLoadError(undefined);
    try {
      const [sources, mappings] = await Promise.all([
        provider.loadMetadata(),
        provider.loadProjectMappings(),
      ]);
      if (generation !== managedLoadGeneration.current) return;
      const parsed = sources.map(parseTask);
      setTasks(parsed.flatMap((result) => (result.task ? [result.task] : [])));
      setIssues(parsed.flatMap((result) => (result.issue ? [result.issue] : [])));
      setProjects(mappings);
      try {
        setSavedFilters(await provider.loadSavedFilters());
        setFilterFailure(undefined);
      } catch {
        setSavedFilters([]);
        setFilterFailure("已保存筛选暂不可用；任务数据不受影响。");
      }
      setManagedLoadState("ready");
    } catch {
      if (generation !== managedLoadGeneration.current) return;
      setManagedLoadState("error");
      setLoadError("正式任务库尚未连接或不可用；Codex 活动不受影响。");
    }
  }

  async function initializeLocalTaskLibrary() {
    setLibraryInitializing(true);
    setLoadError(undefined);
    try {
      await provider.initializeLocalTaskLibrary();
      setManagedLoadState("idle");
    } catch {
      setLoadError("本地任务库创建失败；未修改现有任务资料。");
    } finally {
      setLibraryInitializing(false);
    }
  }

  function clearSavedFilterSelection() {
    setSelectedSavedFilterId("");
  }

  function applySavedFilter(id: string) {
    setSelectedSavedFilterId(id);
    const filter = savedFilters.find((item) => item.id === id);
    if (!filter) return;
    setSelectedProject(filter.projectId);
    setStatusFilter(filter.status);
    setTagFilter(filter.tag);
    setShowArchived(filter.showArchived);
    setView(filter.view);
    setFilterFailure(undefined);
  }

  async function saveCurrentFilter() {
    const name = filterNameDraft.trim();
    if (!name) return;
    const draft: SavedTaskFilterDraft = {
      name,
      projectId: selectedProject,
      status: statusFilter,
      tag: tagFilter,
      showArchived,
      view,
    };
    setFilterSaving(true);
    setFilterFailure(undefined);
    try {
      const saved = await provider.saveTaskFilter(draft);
      setSavedFilters((current) => [...current.filter((item) => item.id !== saved.id), saved]);
      setSelectedSavedFilterId(saved.id);
      setFilterNameDraft("");
      setFilterEditorOpen(false);
    } catch {
      setFilterFailure("筛选方案保存失败；当前筛选仍保留。");
    } finally {
      setFilterSaving(false);
    }
  }

  async function deleteSelectedFilter() {
    if (!selectedSavedFilterId) return;
    setFilterSaving(true);
    setFilterFailure(undefined);
    try {
      await provider.deleteTaskFilter(selectedSavedFilterId);
      setSavedFilters((current) => current.filter((item) => item.id !== selectedSavedFilterId));
      setSelectedSavedFilterId("");
    } catch {
      setFilterFailure("筛选方案删除失败；任务数据不受影响。");
    } finally {
      setFilterSaving(false);
    }
  }

  function openCodexThread(thread: CodexThreadSummary) {
    codexHistoryGeneration.current += 1;
    setCodexHistory({});
    setCodexContinueDraft("");
    setCodexContinuePreview(undefined);
    setCodexTurnFailure(undefined);
    setCodexOpenReceipt(undefined);
    setCodexOpenFailure(undefined);
    setSelectedCodex(thread);
  }

  async function openSelectedThreadInCodex() {
    if (!selectedCodex || codexOpenBusy) return;
    setCodexOpenBusy(true);
    setCodexOpenReceipt(undefined);
    setCodexOpenFailure(undefined);
    try {
      setCodexOpenReceipt(await provider.openCodexThread(selectedCodex.threadId));
    } catch (error) {
      setCodexOpenFailure(normalizeCodexHistoryError(error));
    } finally {
      setCodexOpenBusy(false);
    }
  }

  function previewCodexContinuation() {
    const value = codexContinueDraft.trim();
    if (!value) return;
    setCodexContinuePreview(value);
    setCodexTurnFailure(undefined);
  }

  async function confirmCodexContinuation() {
    if (!selectedCodex || !codexContinuePreview) return;
    setCodexTurnFailure(undefined);
    try {
      const savedRuntime = runtimeOverrides.find((item) => item.threadId === selectedCodex.threadId);
      const runtime = savedRuntime ? {
        effortSet: savedRuntime.effortSet,
        effort: savedRuntime.effort,
        serviceTierSet: savedRuntime.serviceTierSet,
        serviceTier: savedRuntime.serviceTier,
      } : undefined;
      const next = runtime
        ? await provider.startCodexTurn(selectedCodex.threadId, codexContinuePreview, runtime)
        : await provider.startCodexTurn(selectedCodex.threadId, codexContinuePreview);
      setCodexTurn(next);
      setCodexTurnOwner(selectedCodex);
      setCodexPollFailures(0);
      // The application never persists the user's continuation text.
      setCodexContinueDraft("");
      setCodexContinuePreview(undefined);
    } catch (error) {
      setCodexTurnFailure(normalizeCodexHistoryError(error));
    }
  }

  async function respondCodexApproval(decision: CodexApprovalDecision) {
    if (!codexTurn?.pendingApproval) return;
    setCodexTurnFailure(undefined);
    try {
      const next = await provider.respondCodexTurnApproval(
        codexTurn.sessionId,
        codexTurn.pendingApproval.requestId,
        decision,
      );
      setCodexTurn(next);
      setCodexPollFailures(0);
    } catch (error) {
      setCodexTurnFailure(normalizeCodexHistoryError(error));
    }
  }

  async function interruptCodexContinuation() {
    if (!codexTurn) return;
    setCodexTurnFailure(undefined);
    try {
      setCodexTurn(await provider.interruptCodexTurn(codexTurn.sessionId));
      setCodexPollFailures(0);
    } catch (error) {
      setCodexTurnFailure(normalizeCodexHistoryError(error));
    }
  }

  function resetCodexContinuation() {
    setCodexContinueDraft("");
    setCodexContinuePreview(undefined);
    setCodexTurnFailure(undefined);
  }

  function retryCodexTurnStatus() {
    if (!codexTurn) return;
    setCodexTurnFailure(undefined);
    setCodexPollFailures(0);
    setCodexTurn({ ...codexTurn });
  }

  function closeDetails() {
    taskDetailGeneration.current += 1;
    codexHistoryGeneration.current += 1;
    resetWriteFlow();
    resetNoteFlow();
    setCodexHistory({});
    setSelected(undefined);
    setSelectedCodex(undefined);
  }

  async function openTask(task: TaskRecord) {
    const generation = taskDetailGeneration.current + 1;
    taskDetailGeneration.current = generation;
    codexHistoryGeneration.current += 1;
    setSelected(task);
    setBody(undefined);
    setEvents([]);
    setLoadingDetail(true);
    setLoadError(undefined);
    setCodexHistory({});
    resetWriteFlow();
    resetNoteFlow();
    try {
      const [nextBody, nextEvents] = await Promise.all([
        provider.loadBody(task.fileToken),
        provider.loadEvents(task.id),
      ]);
      if (generation !== taskDetailGeneration.current) return;
      setBody(nextBody);
      setEvents(nextEvents);
    } catch {
      if (generation === taskDetailGeneration.current) {
        setLoadError("该任务详情读取失败；其他任务仍可继续使用。 ");
      }
    } finally {
      if (generation === taskDetailGeneration.current) setLoadingDetail(false);
    }
  }

  function normalizeCodexHistoryError(error: unknown): CodexHistoryFailure {
    if (error && typeof error === "object" && "code" in error && "message" in error) {
      return { code: String(error.code), message: String(error.message) };
    }
    return { code: "history_failed", message: "本轮未能读取 Codex 官方历史。" };
  }

  async function loadCodexHistory(threadId: string, cursor?: string) {
    const generation = codexHistoryGeneration.current;
    setCodexHistory((current) => ({
      ...current,
      [threadId]: {
        page: current[threadId]?.page,
        turns: current[threadId]?.turns ?? [],
        loading: true,
      },
    }));
    try {
      const page = await provider.loadCodexThreadPage(threadId, cursor);
      if (generation !== codexHistoryGeneration.current) return;
      setCodexHistory((current) => {
        const previous = cursor ? current[threadId]?.turns ?? [] : [];
        const seen = new Set(previous.map((turn) => turn.id));
        return {
          ...current,
          [threadId]: {
            page,
            turns: [...previous, ...page.turns.filter((turn) => !seen.has(turn.id))],
            loading: false,
          },
        };
      });
    } catch (error) {
      if (generation !== codexHistoryGeneration.current) return;
      setCodexHistory((current) => ({
        ...current,
        [threadId]: {
          page: current[threadId]?.page,
          turns: current[threadId]?.turns ?? [],
          loading: false,
          failure: normalizeCodexHistoryError(error),
        },
      }));
    }
  }

  function resetWriteFlow() {
    setPriorityDraft(undefined);
    setWritePreview(undefined);
    setWriteReceipt(undefined);
    setWriteFailure(undefined);
    setWriting(false);
    setFieldDraft(undefined);
    setFieldPreview(undefined);
    setFieldReceipt(undefined);
  }

  function resetNoteFlow() {
    setNoteDraft({ kind: "comment", text: "", author: "本人" });
    setNotePreview(undefined);
    setNoteReceipt(undefined);
    setNoteFailure(undefined);
  }

  function normalizeWriteError(error: unknown): TaskWriteFailure {
    if (error && typeof error === "object" && "code" in error && "message" in error) {
      return { code: String(error.code), message: String(error.message) };
    }
    return { code: "write_failed", message: "写入失败，原任务已保留。" };
  }

  function isConflict(failure: TaskWriteFailure): boolean {
    return failure.code === "conflict" || failure.code === "event_conflict";
  }

  async function previewPriority() {
    if (!selected || !priorityDraft) return;
    setWriting(true);
    setWriteFailure(undefined);
    setWriteReceipt(undefined);
    try {
      setWritePreview(await provider.previewPriorityEdit(selected.fileToken, priorityDraft));
    } catch (error) {
      setWritePreview(undefined);
      setWriteFailure(normalizeWriteError(error));
    } finally {
      setWriting(false);
    }
  }

  async function refreshAfterConflict(selectedId: string) {
    try {
      const sources = await provider.loadMetadata();
      const parsed = sources.map(parseTask);
      const nextTasks = parsed.flatMap((result) => (result.task ? [result.task] : []));
      setTasks(nextTasks);
      setIssues(parsed.flatMap((result) => (result.issue ? [result.issue] : [])));
      setSelected(nextTasks.find((task) => task.id === selectedId));
    } catch {
      // 保留原写入错误，不用刷新错误覆盖它。
    }
  }

  async function confirmPriority() {
    if (!selected || !priorityDraft || !writePreview) return;
    setWriting(true);
    setWriteFailure(undefined);
    try {
      const receipt = await provider.applyPriorityEdit({
        fileToken: selected.fileToken,
        newPriority: priorityDraft,
        expectedHash: writePreview.expectedHash,
        confirmed: true,
      });
      const nextTask: TaskRecord = { ...selected, priority: receipt.newPriority };
      setTasks((current) => current.map((task) => task.id === nextTask.id ? nextTask : task));
      setSelected(nextTask);
      setEvents(await provider.loadEvents(nextTask.id));
      setWriteReceipt(receipt);
      setWritePreview(undefined);
      setPriorityDraft(undefined);
    } catch (error) {
      const failure = normalizeWriteError(error);
      setWriteFailure(failure);
      setWritePreview(undefined);
      if (isConflict(failure)) await refreshAfterConflict(selected.id);
    } finally {
      setWriting(false);
    }
  }

  function initialFieldValue(task: TaskRecord, field: WritableTaskField): string {
    if (field === "title") return task.title;
    if (field === "task_status") return task.status;
    if (field === "priority") return task.priority === "unknown" ? "medium" : task.priority;
    if (field === "deadline") return task.deadline ?? "";
    if (field === "assignee") return task.assignee ?? "";
    if (field === "tags") return task.tags.join(", ");
    if (field === "parent_id") return task.parentId ?? "";
    if (field === "blocked_by_ids") return task.blockedByIds.join(", ");
    if (field === "related_ids") return task.relatedIds.join(", ");
    return task.recordStatus === "unknown" ? "current" : task.recordStatus;
  }

  function fieldValue(draft: { field: WritableTaskField; rawValue: string }): unknown {
    if (["tags", "blocked_by_ids", "related_ids"].includes(draft.field)) {
      return draft.rawValue.split(",").map((value) => value.trim()).filter(Boolean);
    }
    return draft.rawValue.trim();
  }

  async function previewFieldEdit() {
    if (!selected || !fieldDraft) return;
    setWriting(true);
    setWriteFailure(undefined);
    setFieldReceipt(undefined);
    try {
      setFieldPreview(await provider.previewTaskFieldEdit(
        selected.fileToken,
        fieldDraft.field,
        fieldValue(fieldDraft),
      ));
    } catch (error) {
      setFieldPreview(undefined);
      setWriteFailure(normalizeWriteError(error));
    } finally {
      setWriting(false);
    }
  }

  function taskAfterFieldEdit(task: TaskRecord, receipt: TaskFieldEditReceipt): TaskRecord {
    const value = receipt.newValue;
    if (receipt.field === "title") return { ...task, title: String(value) };
    if (receipt.field === "task_status") return { ...task, status: value as TaskStatus, rawStatus: String(value) };
    if (receipt.field === "priority") return { ...task, priority: value as TaskPriority };
    if (receipt.field === "deadline") return { ...task, deadline: String(value) || undefined };
    if (receipt.field === "assignee") return { ...task, assignee: String(value) || undefined };
    if (receipt.field === "tags") return { ...task, tags: value as string[] };
    if (receipt.field === "parent_id") return { ...task, parentId: String(value) || undefined };
    if (receipt.field === "blocked_by_ids") return { ...task, blockedByIds: value as string[] };
    if (receipt.field === "related_ids") return { ...task, relatedIds: value as string[] };
    return { ...task, recordStatus: value as TaskRecord["recordStatus"] };
  }

  async function confirmFieldEdit() {
    if (!selected || !fieldDraft || !fieldPreview) return;
    setWriting(true);
    setWriteFailure(undefined);
    try {
      const receipt = await provider.applyTaskFieldEdit({
        fileToken: selected.fileToken,
        field: fieldDraft.field,
        newValue: fieldValue(fieldDraft),
        expectedHash: fieldPreview.expectedHash,
        confirmed: true,
      });
      const nextTask = taskAfterFieldEdit(selected, receipt);
      setTasks((current) => current.map((task) => task.id === nextTask.id ? nextTask : task));
      setSelected(nextTask);
      setEvents(await provider.loadEvents(nextTask.id));
      setFieldReceipt(receipt);
      setFieldPreview(undefined);
      setFieldDraft(undefined);
    } catch (error) {
      const failure = normalizeWriteError(error);
      setWriteFailure(failure);
      setFieldPreview(undefined);
      if (isConflict(failure)) await refreshAfterConflict(selected.id);
    } finally {
      setWriting(false);
    }
  }

  async function previewNote() {
    if (!selected || !noteDraft.text.trim() || !noteDraft.author.trim()) return;
    setWriting(true);
    setNoteFailure(undefined);
    setNoteReceipt(undefined);
    try {
      setNotePreview(await provider.previewTaskNote(
        selected.fileToken,
        noteDraft.kind,
        noteDraft.text,
        noteDraft.author,
      ));
    } catch (error) {
      setNotePreview(undefined);
      setNoteFailure(normalizeWriteError(error));
    } finally {
      setWriting(false);
    }
  }

  async function confirmNote() {
    if (!selected || !notePreview) return;
    setWriting(true);
    setNoteFailure(undefined);
    try {
      const receipt = await provider.applyTaskNote(notePreview);
      setEvents(await provider.loadEvents(selected.id));
      setNoteReceipt(receipt);
      setNotePreview(undefined);
      setNoteDraft((current) => ({ ...current, text: "" }));
    } catch (error) {
      const failure = normalizeWriteError(error);
      setNoteFailure(failure);
      setNotePreview(undefined);
      if (failure.code === "conflict") await refreshAfterConflict(selected.id);
      if (failure.code === "event_conflict") {
        try { setEvents(await provider.loadEvents(selected.id)); } catch { /* 保留原错误 */ }
      }
    } finally {
      setWriting(false);
    }
  }

  function resetCreateFlow() {
    setCreateOpen(false);
    setCreatePreview(undefined);
    setCreateReceipt(undefined);
    setCreateFailure(undefined);
    setCreateDraft({
      title: "", domain: "task_hub", taskStatus: "todo", priority: "medium",
      assignee: "本人", deadline: "", tags: [], parentId: "", blockedByIds: [], relatedIds: [],
    });
  }

  async function previewCreate() {
    setWriting(true);
    setCreateFailure(undefined);
    setCreateReceipt(undefined);
    try {
      setCreatePreview(await provider.previewCreateTask(createDraft));
    } catch (error) {
      setCreatePreview(undefined);
      setCreateFailure(normalizeWriteError(error));
    } finally {
      setWriting(false);
    }
  }

  async function confirmCreate() {
    if (!createPreview) return;
    setWriting(true);
    setCreateFailure(undefined);
    try {
      const receipt = await provider.applyCreateTask(createPreview);
      const sources = await provider.loadMetadata();
      const parsed = sources.map(parseTask);
      setTasks(parsed.flatMap((result) => (result.task ? [result.task] : [])));
      setIssues(parsed.flatMap((result) => (result.issue ? [result.issue] : [])));
      setCreateReceipt(receipt);
      setCreatePreview(undefined);
    } catch (error) {
      setCreateFailure(normalizeWriteError(error));
      setCreatePreview(undefined);
    } finally {
      setWriting(false);
    }
  }

  async function checkForUpdate(manual: boolean) {
    setUpdateBusy(true);
    setUpdateFailed(false);
    if (manual) setUpdateMessage("正在检查任务中心更新…");
    try {
      const info = await provider.checkTaskCenterUpdate();
      setUpdateInfo(info);
      try { localStorage.setItem(updateCheckStorageKey, String(Date.now())); } catch { /* 不影响更新检查 */ }
      if (info.available && info.version) setUpdateMessage(`发现任务中心 ${info.version}，已通过独立签名通道提供。`);
      else setUpdateMessage(manual ? `当前已是最新版 ${info.currentVersion}。` : undefined);
    } catch {
      setUpdateFailed(true);
      setUpdateMessage(manual ? "暂时无法检查更新；当前版本不受影响。" : undefined);
    } finally {
      setUpdateBusy(false);
    }
  }

  async function installUpdate() {
    if (!updateInfo?.available || !updateInfo.version) return;
    setUpdateBusy(true);
    setUpdateFailed(false);
    setUpdateMessage(`正在下载并安全验证 ${updateInfo.version}…`);
    try {
      await provider.installTaskCenterUpdate(updateInfo.version);
      setUpdateMessage("更新已安装，正在重新打开任务中心…");
    } catch (error) {
      setUpdateFailed(true);
      setUpdateMessage(safeUpdateError(error));
    } finally {
      setUpdateBusy(false);
    }
  }

  function updateAppearance(patch: Partial<AppearanceSettings>) {
    setAppearance((current) => ({ ...current, ...patch }));
  }

  async function openRuntimeSettings() {
    setRuntimeOpen(true);
    setRuntimeFailure(undefined);
    setRuntimePreview(undefined);
    setRuntimeReceipt(undefined);
    setRuntimeRestoreArmed(false);
    if (runtimeCapabilities || runtimeBusy) return;
    setRuntimeBusy(true);
    try {
      setRuntimeCapabilities(await provider.loadCodexRuntimeCapabilities());
    } catch (error) {
      setRuntimeFailure(normalizeCodexHistoryError(error).message);
    } finally {
      setRuntimeBusy(false);
    }
  }

  function updateRuntimeRequest(patch: Partial<CodexGlobalSettingsRequest>) {
    setRuntimeRequest((current) => ({ ...current, ...patch }));
    setRuntimePreview(undefined);
    setRuntimeReceipt(undefined);
    setRuntimeFailure(undefined);
    setRuntimeRestoreArmed(false);
  }

  async function previewRuntimeSettings() {
    setRuntimeBusy(true);
    setRuntimeFailure(undefined);
    setRuntimeReceipt(undefined);
    setRuntimeRestoreArmed(false);
    try {
      const request: CodexGlobalSettingsRequest = {
        ...runtimeRequest,
        threadIds: runtimeRequest.scope === "currentView"
          ? filteredCodexThreads.map((thread) => thread.threadId)
          : [],
      };
      setRuntimePreview(await provider.previewCodexGlobalSettings(request));
    } catch (error) {
      setRuntimePreview(undefined);
      setRuntimeFailure(normalizeCodexHistoryError(error).message);
    } finally {
      setRuntimeBusy(false);
    }
  }

  async function applyRuntimeSettings() {
    if (!runtimePreview) return;
    setRuntimeBusy(true);
    setRuntimeFailure(undefined);
    try {
      const receipt = await provider.applyCodexGlobalSettings(runtimePreview.previewId);
      setRuntimeReceipt(receipt);
      setRuntimePreview(undefined);
      if (receipt.previous.length) {
        setRuntimeRestore(receipt.previous);
        try { localStorage.setItem(runtimeRestoreStorageKey, JSON.stringify(receipt.previous)); } catch { /* 当前会话仍可恢复 */ }
      }
      if (receipt.applied.length) {
        setRuntimeOverrides((current) => {
          const next = mergeRuntimeOverrides(current, receipt.applied);
          try { localStorage.setItem(runtimeActiveStorageKey, JSON.stringify(next)); } catch { /* 当前会话仍会应用 */ }
          return next;
        });
      }
    } catch (error) {
      setRuntimeFailure(normalizeCodexHistoryError(error).message);
      setRuntimePreview(undefined);
    } finally {
      setRuntimeBusy(false);
    }
  }

  async function restoreRuntimeSettings() {
    if (!runtimeRestore.length) return;
    if (!runtimeRestoreArmed) {
      setRuntimeRestoreArmed(true);
      setRuntimeFailure(undefined);
      return;
    }
    setRuntimeBusy(true);
    setRuntimeFailure(undefined);
    try {
      const receipt = await provider.restoreCodexGlobalSettings(runtimeRestore);
      setRuntimeRestore(receipt.remaining);
      setRuntimeRestoreArmed(false);
      if (receipt.remaining.length) {
        try { localStorage.setItem(runtimeRestoreStorageKey, JSON.stringify(receipt.remaining)); } catch { /* 当前会话继续保留 */ }
      } else {
        try { localStorage.removeItem(runtimeRestoreStorageKey); } catch { /* 不影响恢复结果 */ }
      }
      if (receipt.restored.length) {
        setRuntimeOverrides((current) => {
          const next = mergeRuntimeOverrides(current, receipt.restored);
          try { localStorage.setItem(runtimeActiveStorageKey, JSON.stringify(next)); } catch { /* 当前会话仍会应用 */ }
          return next;
        });
      }
      setRuntimeReceipt({
        changedCount: receipt.restoredCount,
        unchangedCount: 0,
        failedCount: receipt.failedCount,
        failures: receipt.failures,
        previous: [],
        applied: receipt.restored,
        appliedAt: receipt.restoredAt,
      });
    } catch (error) {
      setRuntimeFailure(normalizeCodexHistoryError(error).message);
    } finally {
      setRuntimeBusy(false);
    }
  }

  function closeOnboarding() {
    setOnboardingOpen(false);
    try { localStorage.setItem(onboardingStorageKey, "1"); } catch { /* 本地偏好不可用时只影响下次是否再次显示 */ }
  }

  return (
    <div
      className={`app density-${appearance.density}${appearance.density === "compact" ? " compact" : ""}`}
      data-theme={appearance.theme}
      data-reduce-motion={appearance.reduceMotion ? "true" : "false"}
      style={appearanceStyle}
    >
      <header className="topbar">
        <div>
          <p className="eyebrow">CODEX MONITOR</p>
          <h1>任务中心 <span>{section === "codex" ? "官方任务" : "安全写入"}</span></h1>
        </div>
        <div className="top-actions" aria-label="视图设置">
          {section === "codex" ? <>
            <button disabled={codexListLoading} onClick={() => loadCodexThreads(undefined, true)}>{codexListLoading ? "读取中…" : "刷新"}</button>
            <button onClick={() => void openRuntimeSettings()}>运行配置</button>
          </> : <>
            {managedLoadState === "ready" && <button className="create-entry" onClick={() => { resetCreateFlow(); setCreateOpen(true); }}>新建任务</button>}
            <button className={view === "board" ? "active" : ""} onClick={() => { setView("board"); clearSavedFilterSelection(); }}>看板</button>
            <button className={view === "list" ? "active" : ""} onClick={() => { setView("list"); clearSavedFilterSelection(); }}>列表</button>
            <button aria-pressed={showArchived} onClick={() => { setShowArchived((value) => !value); clearSavedFilterSelection(); }}>归档</button>
          </>}
          <button aria-pressed={appearance.density === "compact"} onClick={() => updateAppearance({ density: appearance.density === "compact" ? "comfortable" : "compact" })}>紧凑</button>
          <button onClick={() => setAppearanceOpen(true)}>显示</button>
          <button className={updateInfo?.available ? "update-ready" : ""} disabled={updateBusy} onClick={() => updateInfo?.available ? installUpdate() : checkForUpdate(true)}>{updateBusy ? "更新处理中…" : updateInfo?.available && updateInfo.version ? `安装 ${updateInfo.version}` : "检查更新"}</button>
          <button className="help-entry" aria-label="打开新手说明" title="新手说明" onClick={() => setOnboardingOpen(true)}>?</button>
        </div>
      </header>

      <div className="workspace">
        <aside className="sidebar" aria-label="任务中心导航">
          <nav className="source-nav" aria-label="数据来源">
            <button className={section === "codex" ? "source-entry active" : "source-entry"} title="查看本机 Codex 官方任务，可打开或继续执行" onClick={() => { closeDetails(); setLoadError(undefined); setSection("codex"); }}>
              <span>Codex 活动<small>官方任务 · 可继续</small></span><strong>{codexThreads.length}</strong>
            </button>
            <button className={section === "managed" ? "source-entry active" : "source-entry"} title="可选的本地任务库，写入前都会再次确认" onClick={() => { closeDetails(); setLoadError(undefined); setSection("managed"); }}>
              <span>管理任务<small>可选本地任务库</small></span><strong>{managedLoadState === "ready" ? tasks.length : "—"}</strong>
            </button>
          </nav>
          {section === "managed" && <div className="project-nav" aria-label="项目">
            <button className={selectedProject === "all" ? "project active" : "project"} onClick={() => { setSelectedProject("all"); clearSavedFilterSelection(); }}>
              <span>全项目</span><strong>{tasks.length}</strong>
            </button>
            {projectOptions.map((project) => (
              <button key={project.id} className={selectedProject === project.id ? "project active" : "project"} onClick={() => { setSelectedProject(project.id); clearSavedFilterSelection(); }}>
                <span>{project.name}<small>{project.workdirs[0] ?? "按正式领域归类"}</small></span>
                <strong>{tasks.filter((task) => task.projectId === project.id || task.domain === project.id).length}</strong>
              </button>
            ))}
          </div>}
          <div className="boundary-note">
            <strong>运行边界</strong>
            <p>独立进程 · 无后台服务<br />关闭窗口即退出</p>
          </div>
        </aside>

        <main>
          {updateMessage && <div role={updateFailed ? "alert" : "status"} className={updateFailed ? "alert update-message" : "update-message"}>{updateMessage}</div>}
          {codexTurn && codexTurnOwner && <div className={`codex-global-turn ${codexTurn.state}`} aria-live="polite">
            <div><strong>{["starting", "running", "waitingApproval", "interrupting"].includes(codexTurn.state) ? "Codex 正在运行" : "Codex 最近结果"}</strong><span>{codexTurnOwner.name ?? "未命名 Codex 任务"} · {codexTurnStateLabel(codexTurn.state)}</span></div>
            <button onClick={() => { setSection("codex"); openCodexThread(codexTurnOwner); }}>查看任务</button>
          </div>}
          {section === "codex" ? <CodexActivity
            threads={filteredCodexThreads}
            allThreads={codexThreads}
            page={codexListPage}
            loading={codexListLoading}
            failure={codexListFailure}
            query={codexQuery}
            appliedSearch={codexAppliedSearch}
            sourceGroup={codexSourceGroup}
            archived={codexArchived}
            onQuery={setCodexQuery}
            onSearch={applyCodexSearch}
            onSourceGroup={changeCodexSourceGroup}
            onArchive={changeCodexArchive}
            onOpen={openCodexThread}
            onRetry={() => loadCodexThreads(undefined, true)}
            onMore={(cursor) => loadCodexThreads(cursor)}
          /> : <>
            {managedLoadState === "ready" && <>
              <section className="summary" aria-label="任务概况">
                <Metric label="进行中" value={tasks.filter((task) => task.status === "doing").length} tone="blue" />
                <Metric label="需要关注" value={tasks.filter((task) => deriveAttention(task).length).length} tone="orange" />
                <Metric label="已完成" value={tasks.filter((task) => task.status === "done").length} tone="green" />
                <Metric label="已隔离文件" value={issues.length} tone={issues.length ? "red" : "muted"} />
              </section>

              <section className="toolbar" aria-label="搜索和筛选">
                <label className="search"><span>搜索</span><input value={query} onChange={(event) => setQuery(event.target.value)} placeholder="标题、领域、负责人…" /></label>
                <label><span>状态</span><select value={statusFilter} onChange={(event) => { setStatusFilter(event.target.value as TaskStatus | "all"); clearSavedFilterSelection(); }}>
                  <option value="all">全部状态</option>
                  {columns.map((status) => <option key={status} value={status}>{statusLabels[status]}</option>)}
                </select></label>
                <label><span>标签</span><select value={tagFilter} onChange={(event) => { setTagFilter(event.target.value); clearSavedFilterSelection(); }}>
                  <option value="">全部标签</option>{allTags.map((tag) => <option key={tag} value={tag}>{tag}</option>)}
                </select></label>
                <label><span>已保存</span><select aria-label="已保存筛选" value={selectedSavedFilterId} onChange={(event) => applySavedFilter(event.target.value)}>
                  <option value="">选择方案</option>{savedFilters.map((filter) => <option key={filter.id} value={filter.id}>{filter.name}</option>)}
                </select></label>
                <div className="filter-actions"><button onClick={() => { setFilterEditorOpen((value) => !value); setFilterFailure(undefined); }}>保存当前筛选</button>{selectedSavedFilterId && <button className="danger-subtle" disabled={filterSaving} onClick={deleteSelectedFilter}>删除方案</button>}</div>
                <div className="result-count">{filtered.length} 项</div>
              </section>

              {filterEditorOpen && <section className="filter-save" aria-label="保存筛选方案">
                <label><span>方案名称</span><input aria-label="筛选方案名称" maxLength={60} value={filterNameDraft} onChange={(event) => setFilterNameDraft(event.target.value)} placeholder="例如：高优先级的AI任务" /></label>
                <p>保存项目、状态、标签、归档和视图；不保存搜索文字或任务内容。</p>
                <div><button disabled={filterSaving || !filterNameDraft.trim()} onClick={saveCurrentFilter}>{filterSaving ? "保存中…" : "确认保存"}</button><button className="secondary" disabled={filterSaving} onClick={() => setFilterEditorOpen(false)}>取消</button></div>
              </section>}
              {filterFailure && <div role="alert" className="alert">{filterFailure}</div>}

              {loadError && <div role="alert" className="alert">{loadError}</div>}
              {view === "board" ? (
                <section className="board" aria-label="任务看板">
                  {columns.map((status) => {
                    const cards = filtered.filter((task) => task.status === status);
                    if (!cards.length && (status === "cancelled" || status === "unknown")) return null;
                    return <div className="column" key={status}>
                      <h2><span className={`dot ${status}`} />{statusLabels[status]} <b>{cards.length}</b></h2>
                      <div className="card-stack">{cards.map((task) => <TaskCard key={task.id} task={task} onOpen={() => openTask(task)} />)}</div>
                    </div>;
                  })}
                </section>
              ) : (
                <section className="list-view" aria-label="任务列表">
                  <div className="list-head"><span>任务</span><span>状态</span><span>优先级</span><span>归属</span><span>更新</span></div>
                  {filtered.map((task) => <button key={task.id} className="list-row" onClick={() => openTask(task)}>
                    <span><strong>{task.title}{task.recordStatus === "archived" ? "（已归档）" : ""}</strong><small>{task.id}</small></span>
                    <span>{statusLabels[task.status]}</span><span>{priorityLabels[task.priority]}</span><span>{task.domain}</span><span>{task.updatedAt ?? "—"}</span>
                  </button>)}
                </section>
              )}
            </>}
            {managedLoadState === "loading" && <div className="empty-state" role="status"><strong>正在连接本地任务库…</strong><p>这一步只在你打开“管理任务”后执行。</p></div>}
            {managedLoadState === "error" && <div className="empty-state library-setup">
              {loadError && <div role="alert" className="alert">{loadError}</div>}
              <strong>尚未建立管理任务库</strong>
              <p>可以创建一个透明、可迁移的本地任务库；不会影响上面的 Codex 官方任务列表。</p>
              <div className="empty-actions"><button disabled={libraryInitializing} onClick={initializeLocalTaskLibrary}>{libraryInitializing ? "创建中…" : "创建本地任务库"}</button><button className="secondary" onClick={() => setManagedLoadState("idle")}>重新检测</button></div>
            </div>}
          </>}
        </main>
      </div>

      {selected && <DetailPanel
        task={selected}
        allTasks={tasks}
        body={body}
        events={events}
        loading={loadingDetail}
        priorityDraft={priorityDraft}
        writePreview={writePreview}
        writeReceipt={writeReceipt}
        writeFailure={writeFailure}
        writing={writing}
        fieldDraft={fieldDraft}
        fieldPreview={fieldPreview}
        fieldReceipt={fieldReceipt}
        noteDraft={noteDraft}
        notePreview={notePreview}
        noteReceipt={noteReceipt}
        noteFailure={noteFailure}
        codexHistory={codexHistory}
        onBeginPriority={() => {
          const initial = selected.priority === "unknown" ? "medium" : selected.priority;
          setPriorityDraft(initial as "high" | "medium" | "low");
          setWritePreview(undefined);
          setWriteReceipt(undefined);
          setWriteFailure(undefined);
        }}
        onPriorityDraft={(value) => {
          setPriorityDraft(value);
          setWritePreview(undefined);
          setWriteFailure(undefined);
        }}
        onPreviewPriority={previewPriority}
        onConfirmPriority={confirmPriority}
        onCancelPriority={resetWriteFlow}
        onBeginField={() => {
          const field: WritableTaskField = "title";
          setFieldDraft({ field, rawValue: initialFieldValue(selected, field) });
          setFieldPreview(undefined);
          setFieldReceipt(undefined);
          setWriteReceipt(undefined);
          setWriteFailure(undefined);
        }}
        onFieldChange={(field) => {
          setFieldDraft({ field, rawValue: initialFieldValue(selected, field) });
          setFieldPreview(undefined);
          setWriteFailure(undefined);
        }}
        onFieldValue={(rawValue) => {
          setFieldDraft((current) => current ? { ...current, rawValue } : current);
          setFieldPreview(undefined);
          setWriteFailure(undefined);
        }}
        onPreviewField={previewFieldEdit}
        onConfirmField={confirmFieldEdit}
        onCancelField={resetWriteFlow}
        onNoteDraft={(draft) => { setNoteDraft(draft); setNotePreview(undefined); setNoteReceipt(undefined); setNoteFailure(undefined); }}
        onPreviewNote={previewNote}
        onConfirmNote={confirmNote}
        onCancelNote={resetNoteFlow}
        onOpenTask={openTask}
        onLoadCodexHistory={loadCodexHistory}
        onClose={closeDetails}
      />}
      {selectedCodex && <CodexActivityDetail
        thread={selectedCodex}
        history={codexHistory[selectedCodex.threadId]}
        continueDraft={codexContinueDraft}
        continuePreview={codexContinuePreview}
        turn={codexTurn?.threadId === selectedCodex.threadId ? codexTurn : undefined}
        turnFailure={codexTurnFailure}
        openBusy={codexOpenBusy}
        openReceipt={codexOpenReceipt}
        openFailure={codexOpenFailure}
        onLoad={loadCodexHistory}
        onOpenInCodex={openSelectedThreadInCodex}
        onContinueDraft={(value) => {
          setCodexContinueDraft(value);
          setCodexContinuePreview(undefined);
          setCodexTurnFailure(undefined);
        }}
        onPreviewContinue={previewCodexContinuation}
        onConfirmContinue={confirmCodexContinuation}
        onCancelContinue={resetCodexContinuation}
        onApproval={respondCodexApproval}
        onInterrupt={interruptCodexContinuation}
        onRetryStatus={retryCodexTurnStatus}
        onClose={closeDetails}
      />}
      {createOpen && <CreateTaskDialog
        draft={createDraft}
        preview={createPreview}
        receipt={createReceipt}
        failure={createFailure}
        writing={writing}
        onDraft={(draft) => { setCreateDraft(draft); setCreatePreview(undefined); setCreateFailure(undefined); }}
        onPreview={previewCreate}
        onConfirm={confirmCreate}
        onClose={resetCreateFlow}
      />}
      {onboardingOpen && <OnboardingDialog onClose={closeOnboarding} />}
      {appearanceOpen && <AppearanceDialog
        settings={appearance}
        onChange={updateAppearance}
        onReset={() => setAppearance(defaultAppearance)}
        onClose={() => setAppearanceOpen(false)}
      />}
      {runtimeOpen && <RuntimeSettingsDialog
        capabilities={runtimeCapabilities}
        request={runtimeRequest}
        preview={runtimePreview}
        receipt={runtimeReceipt}
        restoreCount={runtimeRestore.length}
        restoreArmed={runtimeRestoreArmed}
        currentViewCount={filteredCodexThreads.length}
        busy={runtimeBusy}
        failure={runtimeFailure}
        onRequest={updateRuntimeRequest}
        onPreview={previewRuntimeSettings}
        onApply={applyRuntimeSettings}
        onRestore={restoreRuntimeSettings}
        onClose={() => setRuntimeOpen(false)}
      />}
    </div>
  );
}

function OnboardingDialog({ onClose }: { onClose: () => void }) {
  useEffect(() => {
    const closeOnEscape = (event: KeyboardEvent) => event.key === "Escape" && onClose();
    window.addEventListener("keydown", closeOnEscape);
    return () => window.removeEventListener("keydown", closeOnEscape);
  }, [onClose]);

  return <div className="scrim onboarding-scrim" onMouseDown={(event) => event.target === event.currentTarget && onClose()}>
    <section className="onboarding-dialog" role="dialog" aria-modal="true" aria-labelledby="onboarding-title">
      <div className="onboarding-head">
        <div><p className="eyebrow">QUICK START</p><h2 id="onboarding-title">三步开始使用任务中心</h2></div>
        <button aria-label="关闭新手说明" onClick={onClose}>×</button>
      </div>
      <p className="onboarding-lead">默认页面只读取本机 Codex 官方任务；不读取或保存对话正文。</p>
      <ol className="onboarding-steps">
        <li><span>1</span><div><strong>找到任务</strong><p>在“Codex 活动”中搜索或筛选，点击任务名称查看详情。</p></div></li>
        <li><span>2</span><div><strong>打开或继续</strong><p>“在 Codex 中打开”只负责定位；“继续这个任务”会先让你确认，再启动新一轮。</p></div></li>
        <li><span>3</span><div><strong>按需管理</strong><p>“管理任务”是可选本地任务库。任何修改都会先预览，再由你确认。</p></div></li>
      </ol>
      <div className="onboarding-footer"><p>以后可点击右上角“？”再次查看。</p><button autoFocus onClick={onClose}>开始使用</button></div>
    </section>
  </div>;
}

function AppearanceDialog({
  settings, onChange, onReset, onClose,
}: {
  settings: AppearanceSettings;
  onChange: (patch: Partial<AppearanceSettings>) => void;
  onReset: () => void;
  onClose: () => void;
}) {
  useEffect(() => {
    const closeOnEscape = (event: KeyboardEvent) => event.key === "Escape" && onClose();
    window.addEventListener("keydown", closeOnEscape);
    return () => window.removeEventListener("keydown", closeOnEscape);
  }, [onClose]);

  return <div className="scrim settings-scrim" onMouseDown={(event) => event.target === event.currentTarget && onClose()}>
    <section className="settings-dialog" role="dialog" aria-modal="true" aria-labelledby="appearance-title">
      <div className="settings-head">
        <div><p className="eyebrow">APPEARANCE</p><h2 id="appearance-title">显示设置</h2></div>
        <button aria-label="关闭显示设置" onClick={onClose}>×</button>
      </div>
      <p className="settings-lead">所有设置只保存在这台电脑，调整时会立即预览，不增加后台读取。</p>

      <div className="settings-grid">
        <fieldset>
          <legend>主题颜色</legend>
          <div className="choice-grid theme-choices">
            {([
              ["midnight", "深蓝"], ["slate", "中性深色"], ["light", "浅色"], ["highContrast", "高对比"],
            ] as Array<[AppearanceTheme, string]>).map(([value, label]) => <button
              key={value}
              className={settings.theme === value ? "selected" : ""}
              aria-pressed={settings.theme === value}
              onClick={() => onChange({ theme: value })}
            >{label}</button>)}
          </div>
        </fieldset>

        <label className="settings-field color-field"><span>强调色</span><div><input aria-label="强调色" type="color" value={settings.accent} onChange={(event) => onChange({ accent: event.target.value })} /><code>{settings.accent.toUpperCase()}</code></div></label>

        <label className="settings-field"><span>字体</span><select aria-label="界面字体" value={settings.font} onChange={(event) => onChange({ font: event.target.value as AppearanceFont })}>
          <option value="system">系统字体</option><option value="rounded">圆体</option><option value="serif">宋体 / 衬线</option><option value="mono">等宽字体</option>
        </select></label>

        <label className="settings-field range-field"><span>字体大小 <output>{settings.fontScale}%</output></span><input aria-label="字体大小" type="range" min="80" max="160" step="5" value={settings.fontScale} onChange={(event) => onChange({ fontScale: Number(event.target.value) })} /></label>

        <label className="settings-field"><span>内容密度</span><select aria-label="内容密度" value={settings.density} onChange={(event) => onChange({ density: event.target.value as AppearanceDensity })}>
          <option value="compact">紧凑</option><option value="comfortable">舒适</option><option value="relaxed">宽松</option>
        </select></label>

        <label className="settings-field"><span>行距</span><select aria-label="界面行距" value={settings.lineSpacing} onChange={(event) => onChange({ lineSpacing: event.target.value as AppearanceLineSpacing })}>
          <option value="compact">紧凑</option><option value="comfortable">舒适</option><option value="relaxed">宽松</option>
        </select></label>
      </div>

      <label className="check-row"><input type="checkbox" checked={settings.reduceMotion} onChange={(event) => onChange({ reduceMotion: event.target.checked })} /><span><strong>减少动态效果</strong><small>关闭悬停位移等非必要动画。</small></span></label>
      <div className="settings-actions"><button className="secondary" onClick={onReset}>恢复默认</button><button onClick={onClose}>完成</button></div>
    </section>
  </div>;
}

function RuntimeSettingsDialog({
  capabilities, request, preview, receipt, restoreCount, restoreArmed, currentViewCount,
  busy, failure, onRequest, onPreview, onApply, onRestore, onClose,
}: {
  capabilities?: CodexRuntimeCapabilities;
  request: CodexGlobalSettingsRequest;
  preview?: CodexGlobalSettingsPreview;
  receipt?: CodexGlobalSettingsReceipt;
  restoreCount: number;
  restoreArmed: boolean;
  currentViewCount: number;
  busy: boolean;
  failure?: string;
  onRequest: (patch: Partial<CodexGlobalSettingsRequest>) => void;
  onPreview: () => void;
  onApply: () => void;
  onRestore: () => void;
  onClose: () => void;
}) {
  useEffect(() => {
    const closeOnEscape = (event: KeyboardEvent) => event.key === "Escape" && !busy && onClose();
    window.addEventListener("keydown", closeOnEscape);
    return () => window.removeEventListener("keydown", closeOnEscape);
  }, [busy, onClose]);

  const noSelection = request.reasoningSelection === "keep" && request.speedSelection === "keep";
  return <div className="scrim settings-scrim" onMouseDown={(event) => event.target === event.currentTarget && !busy && onClose()}>
    <section className="settings-dialog runtime-dialog" role="dialog" aria-modal="true" aria-labelledby="runtime-settings-title">
      <div className="settings-head">
        <div><p className="eyebrow">CODEX RUNTIME</p><h2 id="runtime-settings-title">全局运行配置</h2></div>
        <button aria-label="关闭全局运行配置" disabled={busy} onClick={onClose}>×</button>
      </div>
      <p className="settings-lead">统一切换任务的速度和推理强度。不会启动任务，也不会改变正在运行的回合。速度设置会在你从任务中心继续任务时再次带入；直接在 Codex 中继续时，以 Codex 当时的设置为准。</p>

      {!capabilities && !failure && <div className="runtime-loading" role="status">正在读取当前 Codex 支持的配置…</div>}
      {capabilities && <div className="runtime-form">
        <label className="settings-field"><span>任务范围</span><select aria-label="全局配置任务范围" value={request.scope} onChange={(event) => onRequest({ scope: event.target.value as CodexGlobalSettingsRequest["scope"] })}>
          <option value="allActive">全部未归档任务</option>
          <option value="currentView">当前列表（{currentViewCount} 个）</option>
          <option value="allIncludingArchived">全部任务（含归档）</option>
        </select></label>
        <label className="settings-field"><span>速度</span><select aria-label="全局速度" value={request.speedSelection} onChange={(event) => onRequest({ speedSelection: event.target.value })}>
          <option value="keep">保持各任务当前速度</option>
          <option value="standard">统一为标准速度</option>
          {capabilities.speedTiers.map((option) => <option key={option.id} value={option.id}>{option.label}</option>)}
        </select></label>
        <label className="settings-field"><span>推理强度</span><select aria-label="全局推理强度" value={request.reasoningSelection} onChange={(event) => onRequest({ reasoningSelection: event.target.value })}>
          <option value="keep">保持各任务当前强度</option>
          <option value="default">统一为各模型默认强度</option>
          <option value="minimum">统一为各模型最低强度</option>
          <option value="maximum">统一为各模型最高强度</option>
          {capabilities.reasoningEfforts.map((option) => <option key={option.id} value={option.id}>{option.label}{option.id === "ultra" ? "（可能启用多智能体）" : ""}</option>)}
        </select></label>
        <p className="runtime-capability-note">当前从 {capabilities.modelCount} 个可用模型动态读取；模型不支持的选项会跳过并列明。</p>
        {!preview && <button className="runtime-preview-button" disabled={busy || noSelection} onClick={onPreview}>{busy ? "正在检查任务…" : "预览批量修改"}</button>}
      </div>}

      {preview && <section className="runtime-preview" aria-label="全局运行配置确认">
        <div className="runtime-metrics"><div><strong>{preview.changeableCount}</strong><span>将修改</span></div><div><strong>{preview.unchangedCount}</strong><span>无需修改</span></div><div><strong>{preview.skippedCount}</strong><span>跳过</span></div></div>
        <p>已检查 {preview.discoveredCount} 个任务；涉及 {preview.models.map((item) => `${item.model} ${item.count}个`).join("、") || "暂无可读模型"}。</p>
        {preview.warnings.map((warning) => <small key={warning}>{warning}</small>)}
        <div className="settings-actions"><button className="secondary" disabled={busy} onClick={onPreview}>重新预览</button><button className="confirm-runtime" disabled={busy || preview.changeableCount === 0} onClick={onApply}>{busy ? "应用中…" : `确认应用到 ${preview.changeableCount} 个任务`}</button></div>
      </section>}

      {receipt && <div className={receipt.failedCount ? "runtime-receipt partial" : "runtime-receipt"} role="status"><strong>{receipt.previous.length ? "批量配置完成" : "恢复完成"}</strong><span>成功 {receipt.changedCount} 个，失败 {receipt.failedCount} 个。</span>{receipt.failedCount > 0 && <small>{receipt.failures.slice(0, 3).map((item) => item.message).join("；")}</small>}</div>}
      {failure && <div className="write-failure" role="alert"><strong>{failure}</strong></div>}

      {restoreCount > 0 && <div className="runtime-restore"><div><strong>可恢复上次批量修改</strong><small>已保存 {restoreCount} 个任务修改前的速度和强度。</small></div><button className={restoreArmed ? "confirm-runtime" : "secondary"} disabled={busy} onClick={onRestore}>{restoreArmed ? `确认恢复 ${restoreCount} 个任务` : "恢复上次设置"}</button></div>}
    </section>
  </div>;
}

function CodexActivity({
  threads, allThreads, page, loading, failure, query, appliedSearch, sourceGroup, archived,
  onQuery, onSearch, onSourceGroup, onArchive, onOpen, onRetry, onMore,
}: {
  threads: CodexThreadSummary[];
  allThreads: CodexThreadSummary[];
  page?: CodexThreadListPage;
  loading: boolean;
  failure?: CodexHistoryFailure;
  query: string;
  appliedSearch: string;
  sourceGroup: CodexThreadSourceGroup;
  archived: boolean;
  onQuery: (value: string) => void;
  onSearch: () => void;
  onSourceGroup: (value: CodexThreadSourceGroup) => void;
  onArchive: (value: boolean) => void;
  onOpen: (thread: CodexThreadSummary) => void;
  onRetry: () => void;
  onMore: (cursor: string) => void;
}) {
  const workspaces = new Set(allThreads.map((thread) => thread.workspaceName).filter(Boolean));
  return <>
    <section className="source-intro">
      <div><p className="eyebrow">DEFAULT DATA SOURCE</p><h2>Codex 活动</h2></div>
      <p>直接读取本机官方任务列表，支持原任务继续执行。列表仍只显示名称与必要元数据，不展示或保存对话正文。</p>
    </section>
    <section className="summary" aria-label="Codex 活动概况">
      <Metric label="已读取任务" value={allThreads.length} tone="blue" />
      <Metric label="已有名称" value={allThreads.filter((thread) => thread.name).length} tone="green" />
      <Metric label="置顶任务" value={allThreads.filter((thread) => thread.isPinned).length} tone="orange" />
      <Metric label="工作目录" value={workspaces.size} tone="muted" />
    </section>
    <form className="toolbar codex-toolbar" aria-label="Codex 任务筛选" onSubmit={(event) => { event.preventDefault(); onSearch(); }}>
      <label className="search"><span>官方标题搜索</span><input maxLength={200} value={query} onChange={(event) => onQuery(event.target.value)} placeholder="输入任务名称后按回车" /></label>
      <button className="codex-filter-action" type="submit" disabled={loading}>{loading ? "读取中…" : "搜索"}</button>
      <label><span>来源分类</span><select aria-label="Codex 来源分类" value={sourceGroup} onChange={(event) => onSourceGroup(event.target.value as CodexThreadSourceGroup)}>
        <option value="interactive">主要任务</option><option value="all">全部记录（含子任务）</option><option value="automation">自动化与批处理</option><option value="subagents">子智能体</option>
      </select></label>
      <label><span>归档状态</span><select aria-label="Codex 归档状态" value={archived ? "archived" : "current"} onChange={(event) => onArchive(event.target.value === "archived")}>
        <option value="current">未归档</option><option value="archived">已归档</option>
      </select></label>
      <div className="result-count">{threads.length} 项</div>
    </form>
    {failure && <div role="alert" className="alert codex-list-error"><span>{failure.message}</span><button disabled={loading} onClick={onRetry}>重新读取</button></div>}
    {loading && !allThreads.length && <div className="empty-state" role="status"><strong>正在读取 Codex 官方任务…</strong><p>完成后接口进程会立即退出。</p></div>}
    {!loading && !failure && !allThreads.length && <div className="empty-state"><strong>{appliedSearch ? "没有匹配的 Codex 任务" : archived ? "没有已归档的 Codex 任务" : "没有可显示的 Codex 任务"}</strong><p>{appliedSearch ? `官方标题搜索：${appliedSearch}` : "Codex 未安装、未登录或尚无任务时都可能出现此状态。"}</p></div>}
    {allThreads.length > 0 && <section className="codex-activity-list" aria-label="Codex 官方任务列表">
      <div className="codex-list-head"><span>任务</span><span>来源</span><span>项目</span><span>最近活动</span><span>属性</span></div>
      {threads.map((thread) => <button key={thread.threadId} className="codex-activity-row" onClick={() => onOpen(thread)}>
        <span><strong>{thread.name ?? "未命名 Codex 任务"}</strong><small>{thread.threadId}</small></span>
        <span>{thread.sourceLabel}</span>
        <span>{thread.workspaceName ?? "—"}</span>
        <span>{formatUnixTime(thread.updatedAt ?? thread.createdAt)}</span>
        <span>{thread.archived ? "已归档" : thread.isPinned ? "已置顶" : "普通"}</span>
      </button>)}
      {!threads.length && <p className="no-search-result">没有匹配当前搜索的任务。</p>}
    </section>}
    {page?.nextCursor && <button className="load-more codex-list-more" disabled={loading} onClick={() => onMore(page.nextCursor!)}>{loading ? "正在加载…" : "加载更多官方任务"}</button>}
    {page && <p className="observed-at">本轮读取：{formatUnixTime(page.observedAt)} · {appliedSearch ? `官方标题搜索“${appliedSearch}” · ` : ""}按游标分页，不会高频扫描</p>}
  </>;
}

function CodexActivityDetail({
  thread, history, continueDraft, continuePreview, turn, turnFailure, openBusy, openReceipt, openFailure,
  onLoad, onOpenInCodex, onContinueDraft, onPreviewContinue, onConfirmContinue, onCancelContinue, onClose,
  onApproval, onInterrupt, onRetryStatus,
}: {
  thread: CodexThreadSummary;
  history?: CodexHistoryView;
  continueDraft: string;
  continuePreview?: string;
  turn?: CodexTurnSnapshot;
  turnFailure?: CodexHistoryFailure;
  openBusy: boolean;
  openReceipt?: CodexOpenReceipt;
  openFailure?: CodexHistoryFailure;
  onLoad: (threadId: string, cursor?: string) => void;
  onOpenInCodex: () => void;
  onContinueDraft: (value: string) => void;
  onPreviewContinue: () => void;
  onConfirmContinue: () => void;
  onCancelContinue: () => void;
  onApproval: (decision: CodexApprovalDecision) => void;
  onInterrupt: () => void;
  onRetryStatus: () => void;
  onClose: () => void;
}) {
  const turnActive = Boolean(turn && ["starting", "running", "waitingApproval", "interrupting"].includes(turn.state));
  useEffect(() => {
    const closeOnEscape = (event: KeyboardEvent) => event.key === "Escape" && onClose();
    window.addEventListener("keydown", closeOnEscape);
    return () => window.removeEventListener("keydown", closeOnEscape);
  }, [onClose]);
  return <div className="scrim" onMouseDown={(event) => event.target === event.currentTarget && onClose()}>
    <aside className="detail" role="dialog" aria-modal="true" aria-labelledby="codex-detail-title">
      <div className="detail-head"><div><p className="eyebrow">CODEX OFFICIAL THREAD</p><h2 id="codex-detail-title">{thread.name ?? "未命名 Codex 任务"}</h2></div><button autoFocus aria-label="关闭 Codex 详情" onClick={onClose}>×</button></div>
      <dl className="facts">
        <div><dt>来源</dt><dd>{thread.sourceLabel}</dd></div>
        <div><dt>项目</dt><dd>{thread.workspaceName ?? "—"}</dd></div>
        <div><dt>最近活动</dt><dd>{formatUnixTime(thread.updatedAt ?? thread.createdAt)}</dd></div>
        <div><dt>记录状态</dt><dd>{thread.archived ? "已归档" : codexReportedStatus(thread.reportedStatus)}</dd></div>
      </dl>
      <section className="codex-open-section"><div className="section-title"><h3>任务编号</h3><button disabled={openBusy} title="只在 Codex 中定位，不会发送新内容" onClick={onOpenInCodex}>{openBusy ? "正在打开…" : "在 Codex 中打开"}</button></div><p className="thread-id-copy">{thread.threadId}</p>
        <p className="action-hint">只定位到这个现有任务，不会新建任务或发送内容。</p>
        {openReceipt && <p className="codex-open-message" aria-live="polite">{openReceipt.message}</p>}
        {openFailure && <div className="write-failure" role="alert"><strong>{openFailure.message}</strong><small>{openFailure.code}</small></div>}
      </section>
      <section className="codex-continue" aria-label="继续 Codex 任务">
        <div className="section-title"><h3>继续这个任务</h3><span className="on-demand">官方接口</span></div>
        <p className="write-boundary">先生成确认，再由你确认后在原 Codex 任务中开始新一轮；会带入已确认的全局速度和推理强度，但不会改动模型、目录或权限。</p>
        {!turnActive && <div className="continue-editor">
          <label><span>要交给 Codex 的内容</span><textarea aria-label="继续任务内容" maxLength={16000} value={continueDraft} onChange={(event) => onContinueDraft(event.target.value)} placeholder="例如：从上次断点继续，先完成未闭合的测试。" /></label>
          {!continuePreview && <div className="write-actions"><button disabled={!continueDraft.trim()} onClick={onPreviewContinue}>生成继续确认</button></div>}
          {continuePreview && <div className="write-preview continue-preview" role="region" aria-label="继续任务确认">
            <strong>即将发送给原 Codex 任务</strong>
            <p className="continue-preview-text">{continuePreview}</p>
            <p>这会立即启动新一轮执行，不只是保存草稿。</p>
            <div className="write-actions"><button className="confirm-write" onClick={onConfirmContinue}>确认并继续</button><button className="secondary" onClick={onCancelContinue}>取消</button></div>
          </div>}
        </div>}
        {turn && <div className={`turn-status ${turn.state}`} role="status">
          <strong>{codexTurnStateLabel(turn.state)}</strong>
          <span>{turn.turnId ? `轮次 ${turn.turnId}` : "正在建立官方连接"}</span>
          {turn.errorMessage && <small>{turn.errorMessage}</small>}
          {turn.pendingApproval && <div className="approval-card" aria-label="Codex 操作授权">
            <strong>{turn.pendingApproval.label}</strong>
            {turn.pendingApproval.summary && <p>{turn.pendingApproval.summary}</p>}
            {turn.pendingApproval.reason && <small>原因：{turn.pendingApproval.reason}</small>}
            <div className="approval-actions">{turn.pendingApproval.availableDecisions.map((decision) => <button key={decision} className={decision === "accept" || decision === "acceptForSession" ? "approve" : "decline"} onClick={() => onApproval(decision)}>{codexApprovalDecisionLabel(decision)}</button>)}</div>
          </div>}
          {turnActive && turn.state !== "interrupting" && <button className="interrupt-turn" onClick={onInterrupt}>中断本轮</button>}
        </div>}
        {turnActive && <p className="turn-close-warning">关闭整个任务中心会中断它启动的本轮任务，并回收官方接口进程。</p>}
        {turnFailure && <div className="write-failure turn-poll-failure" role="alert"><strong>{turnFailure.message}</strong><small>{turnFailure.code}</small>{turnActive && <button onClick={onRetryStatus}>重试状态</button>}</div>}
      </section>
      <section><h3>Codex 历史 <span className="on-demand">按需读取</span></h3><CodexThreadHistory threadId={thread.threadId} view={history} onLoad={onLoad} /></section>
    </aside>
  </div>;
}

function Metric({ label, value, tone }: { label: string; value: number; tone: string }) {
  return <div className={`metric ${tone}`}><strong>{value}</strong><span>{label}</span></div>;
}

function TaskCard({ task, onOpen }: { task: TaskRecord; onOpen: () => void }) {
  const attention = deriveAttention(task);
  const threads = codexThreadIds(task);
  return <button className="task-card" onClick={onOpen}>
    <div className="badges"><span className={`priority ${task.priority}`}>{priorityLabels[task.priority]}</span>{task.recordStatus === "archived" && <span className="archived">已归档</span>}{attention.map((hint) => <span className="attention" key={hint}>{hint}</span>)}{task.tags.slice(0, 3).map((tag) => <span className="tag" key={tag}>{tag}</span>)}</div>
    <h3>{task.title}</h3>
    {task.workflowStatus && <p>{task.workflowStatus}</p>}
    <footer><span>{task.domain}</span><span>{threads.length ? `Codex × ${threads.length}` : task.assignee ?? "未分配"}</span></footer>
  </button>;
}

function DetailPanel({
  task, allTasks, body, events, loading, priorityDraft, writePreview, writeReceipt, writeFailure, writing,
  fieldDraft, fieldPreview, fieldReceipt, noteDraft, notePreview, noteReceipt, noteFailure, codexHistory,
  onBeginPriority, onPriorityDraft, onPreviewPriority, onConfirmPriority, onCancelPriority,
  onBeginField, onFieldChange, onFieldValue, onPreviewField, onConfirmField, onCancelField,
  onNoteDraft, onPreviewNote, onConfirmNote, onCancelNote, onOpenTask, onLoadCodexHistory, onClose,
}: {
  task: TaskRecord;
  allTasks: TaskRecord[];
  body?: string;
  events: TaskEvent[];
  loading: boolean;
  priorityDraft?: "high" | "medium" | "low";
  writePreview?: PriorityEditPreview;
  writeReceipt?: PriorityEditReceipt;
  writeFailure?: TaskWriteFailure;
  writing: boolean;
  fieldDraft?: { field: WritableTaskField; rawValue: string };
  fieldPreview?: TaskFieldEditPreview;
  fieldReceipt?: TaskFieldEditReceipt;
  noteDraft: { kind: TaskNoteKind; text: string; author: string };
  notePreview?: TaskNotePreview;
  noteReceipt?: TaskNoteReceipt;
  noteFailure?: TaskWriteFailure;
  codexHistory: Record<string, CodexHistoryView>;
  onBeginPriority: () => void;
  onPriorityDraft: (value: "high" | "medium" | "low") => void;
  onPreviewPriority: () => void;
  onConfirmPriority: () => void;
  onCancelPriority: () => void;
  onBeginField: () => void;
  onFieldChange: (field: WritableTaskField) => void;
  onFieldValue: (value: string) => void;
  onPreviewField: () => void;
  onConfirmField: () => void;
  onCancelField: () => void;
  onNoteDraft: (draft: { kind: TaskNoteKind; text: string; author: string }) => void;
  onPreviewNote: () => void;
  onConfirmNote: () => void;
  onCancelNote: () => void;
  onOpenTask: (task: TaskRecord) => void;
  onLoadCodexHistory: (threadId: string, cursor?: string) => void;
  onClose: () => void;
}) {
  const threads = codexThreadIds(task);
  const taskById = new Map(allTasks.map((item) => [item.id, item]));
  const children = allTasks.filter((item) => item.parentId === task.id);
  const blocks = allTasks.filter((item) => item.blockedByIds.includes(task.id));
  useEffect(() => {
    const closeOnEscape = (event: KeyboardEvent) => event.key === "Escape" && onClose();
    window.addEventListener("keydown", closeOnEscape);
    return () => window.removeEventListener("keydown", closeOnEscape);
  }, [onClose]);
  return <div className="scrim" onMouseDown={(event) => event.target === event.currentTarget && onClose()}>
    <aside className="detail" role="dialog" aria-modal="true" aria-labelledby="detail-title">
      <div className="detail-head"><div><p className="eyebrow">{task.id}</p><h2 id="detail-title">{task.title}</h2></div><button autoFocus aria-label="关闭详情" onClick={onClose}>×</button></div>
      <dl className="facts"><div><dt>状态</dt><dd>{statusLabels[task.status]}</dd></div><div><dt>优先级</dt><dd>{priorityLabels[task.priority]}</dd></div><div><dt>负责人</dt><dd>{task.assignee ?? "—"}</dd></div><div><dt>截止</dt><dd>{task.deadline ?? "—"}</dd></div></dl>
      {task.tags.length > 0 && <div className="detail-tags" aria-label="任务标签">{task.tags.map((tag) => <span key={tag}>{tag}</span>)}</div>}
      <section className="safe-write" aria-label="安全编辑">
        <div className="section-title"><h3>安全编辑</h3>{!priorityDraft && !fieldDraft && <div className="edit-entry-actions"><button onClick={onBeginPriority}>快速改优先级</button><button onClick={onBeginField}>编辑其他字段</button></div>}</div>
        <p className="write-boundary">标签和任务关系写入当前任务 Markdown；评论和人工记录只追加到事件历史。附件、重复任务、甘特图和工作流仍不写入。</p>
        {priorityDraft && <div className="write-editor">
          <label><span>优先级草稿</span><select value={priorityDraft} onChange={(event) => onPriorityDraft(event.target.value as "high" | "medium" | "low")}>
            <option value="high">高</option><option value="medium">中</option><option value="low">低</option>
          </select></label>
          {!writePreview && <div className="write-actions"><button disabled={writing || priorityDraft === task.priority} onClick={onPreviewPriority}>生成写入预览</button><button className="secondary" disabled={writing} onClick={onCancelPriority}>取消</button></div>}
        </div>}
        {writePreview && <div className="write-preview" role="region" aria-label="修改预览">
          <strong>确认修改前后</strong>
          <dl><div><dt>修改前</dt><dd>{priorityLabels[writePreview.beforePriority as TaskPriority] ?? writePreview.beforePriority}</dd></div><div><dt>修改后</dt><dd>{priorityLabels[writePreview.afterPriority as TaskPriority] ?? writePreview.afterPriority}</dd></div></dl>
          <p>确认时会再次检查文件版本；冲突不会覆盖他人修改。</p>
          <div className="write-actions"><button className="confirm-write" disabled={writing} onClick={onConfirmPriority}>{writing ? "正在安全写入…" : "确认写入"}</button><button className="secondary" disabled={writing} onClick={onCancelPriority}>取消</button></div>
        </div>}
        {fieldDraft && <div className="write-editor">
          <label><span>正式字段</span><select aria-label="正式字段" value={fieldDraft.field} onChange={(event) => onFieldChange(event.target.value as WritableTaskField)}>
            <option value="title">标题</option><option value="task_status">任务状态</option><option value="priority">优先级</option><option value="deadline">截止日期</option><option value="assignee">负责人</option><option value="tags">标签</option><option value="parent_id">父任务</option><option value="blocked_by_ids">被哪些任务阻塞</option><option value="related_ids">关联任务</option><option value="record_status">归档/恢复</option>
          </select></label>
          <FieldValueInput draft={fieldDraft} onValue={onFieldValue} />
          {!fieldPreview && <div className="write-actions"><button disabled={writing} onClick={onPreviewField}>生成写入预览</button><button className="secondary" disabled={writing} onClick={onCancelField}>取消</button></div>}
        </div>}
        {fieldPreview && <div className="write-preview" role="region" aria-label="字段修改预览">
          <strong>确认字段修改</strong>
          <dl><div><dt>修改前</dt><dd>{displayWriteValue(fieldPreview.beforeValue)}</dd></div><div><dt>修改后</dt><dd>{displayWriteValue(fieldPreview.afterValue)}</dd></div></dl>
          <p>仅替换该正式字段；未知字段、正文和原有顺序保持不变。</p>
          <div className="write-actions"><button className="confirm-write" disabled={writing} onClick={onConfirmField}>{writing ? "正在安全写入…" : "确认写入"}</button><button className="secondary" disabled={writing} onClick={onCancelField}>取消</button></div>
        </div>}
        {writeFailure && <div role="alert" className="write-failure"><strong>未写入</strong><span>{writeFailure.message}</span>{["conflict", "event_conflict"].includes(writeFailure.code) && <small>已重新读取当前任务；你的{fieldDraft ? "字段" : "优先级"}草稿仍保留。</small>}</div>}
        {writeReceipt && <div role="status" className="write-success"><strong>已写入并核对</strong><span>任务与事件均已回读一致。</span></div>}
        {fieldReceipt && <div role="status" className="write-success"><strong>字段已写入并核对</strong><span>任务与事件均已回读一致。</span></div>}
      </section>
      {task.nextAction && <section><h3>下一步</h3><p>{task.nextAction}</p></section>}
      <section><h3>任务关系 <span className="on-demand">反向关系由当前任务库推导</span></h3>
        {(task.parentId || children.length || task.blockedByIds.length || blocks.length || task.relatedIds.length) ? <div className="relation-groups">
          <RelationGroup label="父任务" ids={task.parentId ? [task.parentId] : []} taskById={taskById} onOpen={onOpenTask} />
          <RelationGroup label="子任务" ids={children.map((item) => item.id)} taskById={taskById} onOpen={onOpenTask} />
          <RelationGroup label="阻塞当前任务" ids={task.blockedByIds} taskById={taskById} onOpen={onOpenTask} />
          <RelationGroup label="当前任务正在阻塞" ids={blocks.map((item) => item.id)} taskById={taskById} onOpen={onOpenTask} />
          <RelationGroup label="相关任务" ids={task.relatedIds} taskById={taskById} onOpen={onOpenTask} />
        </div> : <p className="muted-text">尚未设置父子、阻塞或相关关系。</p>}
      </section>
      <section><h3>Codex 对话 <span className="on-demand">官方历史按需读取</span></h3>{threads.length ? threads.map((id) => <CodexThreadHistory
        key={id}
        threadId={id}
        view={codexHistory[id]}
        onLoad={onLoadCodexHistory}
      />) : <p className="muted-text">没有正式绑定记录。</p>}</section>
      <section><h3>正文 <span className="on-demand">按需读取</span></h3>{loading ? <p>正在读取…</p> : <pre className="body">{body ?? "正文不可用"}</pre>}</section>
      <section className="note-entry" aria-label="添加评论或人工记录"><h3>评论与人工记录 <span className="on-demand">只追加</span></h3>
        {!notePreview && !noteReceipt && <div className="note-form">
          <div className="create-grid"><label><span>记录类型</span><select aria-label="记录类型" value={noteDraft.kind} onChange={(event) => onNoteDraft({ ...noteDraft, kind: event.target.value as TaskNoteKind })}><option value="comment">评论</option><option value="activity">人工活动</option></select></label><label><span>记录人</span><input aria-label="记录人" maxLength={120} value={noteDraft.author} onChange={(event) => onNoteDraft({ ...noteDraft, author: event.target.value })} /></label></div>
          <label><span>内容</span><textarea aria-label="记录内容" maxLength={2000} value={noteDraft.text} onChange={(event) => onNoteDraft({ ...noteDraft, text: event.target.value })} placeholder="输入评论或已发生的人工活动…" /></label>
          <div className="write-actions"><button disabled={writing || !noteDraft.text.trim() || !noteDraft.author.trim()} onClick={onPreviewNote}>生成追加预览</button></div>
        </div>}
        {notePreview && <div className="write-preview" role="region" aria-label="记录追加预览"><strong>{notePreview.kind === "comment" ? "确认追加评论" : "确认追加人工活动"}</strong><p className="note-preview-text">{notePreview.text}</p><small>{notePreview.author} · {new Date(notePreview.occurredAt).toLocaleString("zh-CN")}</small><div className="write-actions"><button className="confirm-write" disabled={writing} onClick={onConfirmNote}>{writing ? "正在安全追加…" : "确认追加"}</button><button className="secondary" disabled={writing} onClick={onCancelNote}>取消</button></div></div>}
        {noteFailure && <div role="alert" className="write-failure"><strong>未追加</strong><span>{noteFailure.message}</span>{["conflict", "event_conflict"].includes(noteFailure.code) && <small>记录草稿仍保留，请重新生成预览。</small>}</div>}
        {noteReceipt && <div role="status" className="write-success"><strong>记录已追加并回读</strong><span>{noteReceipt.eventId}</span><button className="inline-reset" onClick={onCancelNote}>继续添加</button></div>}
      </section>
      <section><h3>活动时间线</h3>{events.length ? <ol className="timeline">{events.map((event) => <li key={event.id}><time>{new Date(event.occurredAt).toLocaleString("zh-CN")}</time><strong>{taskEventLabel(event.eventType)}</strong>{event.message ? <p className="timeline-message">{event.message}</p> : <span>{event.previousTaskStatus ?? "—"} → {event.newTaskStatus ?? "—"}</span>}{event.author && <small>{event.author}</small>}</li>)}</ol> : <p className="muted-text">没有可显示的正式事件。</p>}</section>
    </aside>
  </div>;
}

function RelationGroup({
  label, ids, taskById, onOpen,
}: {
  label: string;
  ids: string[];
  taskById: Map<string, TaskRecord>;
  onOpen: (task: TaskRecord) => void;
}) {
  if (!ids.length) return null;
  return <div className="relation-group"><strong>{label}</strong><div>{ids.map((id) => {
    const target = taskById.get(id);
    return target
      ? <button key={id} onClick={() => onOpen(target)}>{target.title}<small>{id}</small></button>
      : <span key={id}>{id}<small>当前任务库未读取到</small></span>;
  })}</div></div>;
}

function taskEventLabel(eventType: string): string {
  const labels: Record<string, string> = {
    created: "创建任务",
    started: "开始任务",
    status_changed: "状态变更",
    priority_changed: "优先级变更",
    title_changed: "标题变更",
    deadline_changed: "截止日期变更",
    assignee_changed: "负责人变更",
    tags_changed: "标签变更",
    parent_changed: "父任务变更",
    blockers_changed: "阻塞关系变更",
    relations_changed: "关联任务变更",
    archived: "已归档",
    restored: "已恢复",
    comment_added: "评论",
    manual_activity_added: "人工活动",
  };
  return labels[eventType] ?? eventType;
}

function CodexThreadHistory({
  threadId, view, onLoad,
}: {
  threadId: string;
  view?: CodexHistoryView;
  onLoad: (threadId: string, cursor?: string) => void;
}) {
  const page = view?.page;
  return <div className="thread-block">
    <div className="thread">
      <span>{threadId}</span>
      <span className="runtime history">官方历史 · 实时未知</span>
      <button disabled={view?.loading} onClick={() => onLoad(threadId)}>
        {view?.loading && !page ? "读取中…" : page || view?.failure ? "重新读取" : "读取历史"}
      </button>
    </div>
    {view?.failure && <div className="thread-history-error" role="alert">{view.failure.message}</div>}
    {page && <div className="thread-history" aria-label={`Codex 历史 ${threadId}`}>
      <dl>
        <div><dt>来源</dt><dd>{page.sourceLabel}</dd></div>
        <div><dt>接口状态</dt><dd>{codexReportedStatus(page.reportedStatus)}</dd></div>
        <div><dt>最近更新</dt><dd>{formatUnixTime(page.updatedAt)}</dd></div>
        <div><dt>历史模式</dt><dd>{page.historyState === "paged" ? "按需分页" : "分页不可用"}</dd></div>
      </dl>
      <p>分页请求不加载轮次内容；应用不展示或保存对话正文。该短期接口不能代表桌面版实时运行状态。</p>
      {page.historyMessage && <div className="thread-history-note">{page.historyMessage}</div>}
      {view.turns.length ? <ol className="turn-list">{view.turns.map((turn) => <li key={turn.id}>
        <span>{codexTurnStatus(turn.status)}</span>
        <strong>{formatUnixTime(turn.completedAt ?? turn.startedAt)}</strong>
        <small>{formatDuration(turn.durationMs)}</small>
      </li>)}</ol> : page.historyState === "paged" && <p className="muted-text">没有可显示的轮次元数据。</p>}
      {page.nextCursor && <button className="load-more" disabled={view.loading} onClick={() => onLoad(threadId, page.nextCursor)}>
        {view.loading ? "正在加载…" : "加载更早记录"}
      </button>}
    </div>}
  </div>;
}

function codexReportedStatus(status: string): string {
  const labels: Record<string, string> = {
    notLoaded: "未载入（实时未知）",
    idle: "接口进程空闲（实时未知）",
    active: "接口进程活动（不代表桌面版）",
    systemError: "接口进程异常",
  };
  return labels[status] ?? "未知状态";
}

function codexTurnStatus(status: string): string {
  const labels: Record<string, string> = {
    completed: "已完成",
    inProgress: "进行中",
    interrupted: "已中断",
    failed: "失败",
  };
  return labels[status] ?? "未知";
}

function codexTurnStateLabel(state: CodexTurnSnapshot["state"]): string {
  const labels: Record<CodexTurnSnapshot["state"], string> = {
    starting: "正在连接原 Codex 任务",
    running: "Codex 正在执行",
    waitingApproval: "等待操作授权",
    interrupting: "正在中断本轮",
    completed: "本轮已完成",
    failed: "本轮执行失败",
    interrupted: "本轮已中断",
    timedOut: "本轮等待超时",
  };
  return labels[state];
}

function codexApprovalDecisionLabel(decision: CodexApprovalDecision): string {
  const labels: Record<CodexApprovalDecision, string> = {
    accept: "本次允许",
    acceptForSession: "本次任务期间允许",
    decline: "拒绝",
    cancel: "取消操作",
  };
  return labels[decision];
}

function formatUnixTime(value?: number): string {
  if (!value) return "—";
  const milliseconds = value > 10_000_000_000 ? value : value * 1000;
  const date = new Date(milliseconds);
  return Number.isNaN(date.getTime()) ? "—" : date.toLocaleString("zh-CN");
}

function formatDuration(value?: number): string {
  if (value === undefined || value < 0) return "时长未知";
  const seconds = Math.round(value / 1000);
  if (seconds < 60) return `${seconds}秒`;
  const minutes = Math.floor(seconds / 60);
  const remainder = seconds % 60;
  return remainder ? `${minutes}分${remainder}秒` : `${minutes}分钟`;
}

function FieldValueInput({
  draft,
  onValue,
}: {
  draft: { field: WritableTaskField; rawValue: string };
  onValue: (value: string) => void;
}) {
  if (draft.field === "task_status") {
    return <label><span>字段值</span><select aria-label="字段值" value={draft.rawValue} onChange={(event) => onValue(event.target.value)}>
      <option value="todo">待处理</option><option value="doing">进行中</option><option value="long_term">长期</option><option value="done">已完成</option><option value="cancelled">已取消</option>
    </select></label>;
  }
  if (draft.field === "priority") {
    return <label><span>字段值</span><select aria-label="字段值" value={draft.rawValue} onChange={(event) => onValue(event.target.value)}>
      <option value="high">高</option><option value="medium">中</option><option value="low">低</option>
    </select></label>;
  }
  if (draft.field === "record_status") {
    return <label><span>字段值</span><select aria-label="字段值" value={draft.rawValue} onChange={(event) => onValue(event.target.value)}>
      <option value="current">当前有效（恢复）</option><option value="archived">已归档</option>
    </select></label>;
  }
  return <label><span>字段值</span><input
    aria-label="字段值"
    type={draft.field === "deadline" ? "date" : "text"}
    value={draft.rawValue}
    placeholder={draft.field === "tags" ? "多个标签用英文逗号分隔" : ["blocked_by_ids", "related_ids"].includes(draft.field) ? "多个任务编号用英文逗号分隔" : draft.field === "parent_id" ? "一个父任务编号，可留空" : undefined}
    onChange={(event) => onValue(event.target.value)}
  /></label>;
}

function displayWriteValue(value: unknown): string {
  if (Array.isArray(value)) return value.length ? value.join("、") : "（空）";
  if (value === "" || value === null || value === undefined) return "（空）";
  const labels: Record<string, string> = {
    todo: "待处理", doing: "进行中", long_term: "长期", done: "已完成", cancelled: "已取消",
    high: "高", medium: "中", low: "低", current: "当前有效", archived: "已归档",
  };
  return labels[String(value)] ?? String(value);
}

function CreateTaskDialog({
  draft, preview, receipt, failure, writing, onDraft, onPreview, onConfirm, onClose,
}: {
  draft: NewTaskDraft;
  preview?: CreateTaskPreview;
  receipt?: CreateTaskReceipt;
  failure?: TaskWriteFailure;
  writing: boolean;
  onDraft: (draft: NewTaskDraft) => void;
  onPreview: () => void;
  onConfirm: () => void;
  onClose: () => void;
}) {
  useEffect(() => {
    const closeOnEscape = (event: KeyboardEvent) => event.key === "Escape" && !writing && onClose();
    window.addEventListener("keydown", closeOnEscape);
    return () => window.removeEventListener("keydown", closeOnEscape);
  }, [onClose, writing]);
  const update = <K extends keyof NewTaskDraft>(key: K, value: NewTaskDraft[K]) => onDraft({ ...draft, [key]: value });
  return <div className="scrim create-scrim">
    <aside className="create-dialog" role="dialog" aria-modal="true" aria-labelledby="create-title">
      <div className="detail-head"><div><p className="eyebrow">P2 SAFE CREATE</p><h2 id="create-title">新建正式任务</h2></div><button aria-label="关闭新建任务" disabled={writing} onClick={onClose}>×</button></div>
      <p className="write-boundary">只创建正式模板支持的最小字段；来源、通用隐私和人工确认状态固定写入，不建立第二套数据库。</p>
      {!receipt && <div className="create-form">
        <label><span>标题</span><input aria-label="新任务标题" value={draft.title} onChange={(event) => update("title", event.target.value)} /></label>
        <label><span>归属领域</span><input aria-label="新任务归属领域" value={draft.domain} onChange={(event) => update("domain", event.target.value)} /></label>
        <div className="create-grid"><label><span>任务状态</span><select aria-label="新任务状态" value={draft.taskStatus} onChange={(event) => update("taskStatus", event.target.value as NewTaskDraft["taskStatus"])}>
          <option value="todo">待处理</option><option value="doing">进行中</option><option value="long_term">长期</option><option value="done">已完成</option><option value="cancelled">已取消</option>
        </select></label><label><span>优先级</span><select aria-label="新任务优先级" value={draft.priority} onChange={(event) => update("priority", event.target.value as NewTaskDraft["priority"])}>
          <option value="high">高</option><option value="medium">中</option><option value="low">低</option>
        </select></label></div>
        <div className="create-grid"><label><span>负责人</span><input aria-label="新任务负责人" value={draft.assignee} onChange={(event) => update("assignee", event.target.value)} /></label><label><span>截止日期</span><input aria-label="新任务截止日期" type="date" value={draft.deadline} onChange={(event) => update("deadline", event.target.value)} /></label></div>
        <label><span>标签</span><input aria-label="新任务标签" value={draft.tags.join(", ")} placeholder="多个标签用英文逗号分隔" onChange={(event) => update("tags", event.target.value.split(",").map((value) => value.trim()).filter(Boolean))} /></label>
        <label><span>父任务</span><input aria-label="新任务父任务" value={draft.parentId} placeholder="可留空" onChange={(event) => update("parentId", event.target.value.trim())} /></label>
        <label><span>阻塞当前任务的任务</span><input aria-label="新任务阻塞关系" value={draft.blockedByIds.join(", ")} placeholder="多个任务编号用英文逗号分隔" onChange={(event) => update("blockedByIds", event.target.value.split(",").map((value) => value.trim()).filter(Boolean))} /></label>
        <label><span>关联任务</span><input aria-label="新任务关联任务" value={draft.relatedIds.join(", ")} placeholder="多个任务编号用英文逗号分隔" onChange={(event) => update("relatedIds", event.target.value.split(",").map((value) => value.trim()).filter(Boolean))} /></label>
      </div>}
      {preview && <div className="write-preview create-preview" role="region" aria-label="新建任务预览">
        <strong>确认新建内容</strong>
        <dl><div><dt>任务编号</dt><dd>{preview.taskId}</dd></div><div><dt>标题</dt><dd>{preview.draft.title}</dd></div><div><dt>状态</dt><dd>{displayWriteValue(preview.draft.taskStatus)}</dd></div><div><dt>优先级</dt><dd>{displayWriteValue(preview.draft.priority)}</dd></div><div><dt>归属</dt><dd>{preview.draft.domain}</dd></div><div><dt>负责人</dt><dd>{preview.draft.assignee || "（空）"}</dd></div><div><dt>截止</dt><dd>{preview.draft.deadline || "（空）"}</dd></div><div><dt>标签</dt><dd>{preview.draft.tags.length ? preview.draft.tags.join("、") : "（空）"}</dd></div><div><dt>父任务</dt><dd>{preview.draft.parentId || "（空）"}</dd></div><div><dt>被阻塞于</dt><dd>{preview.draft.blockedByIds.length ? preview.draft.blockedByIds.join("、") : "（空）"}</dd></div><div><dt>关联任务</dt><dd>{preview.draft.relatedIds.length ? preview.draft.relatedIds.join("、") : "（空）"}</dd></div><div><dt>隐私 / 访问</dt><dd>general / proposal_only</dd></div><div><dt>来源 / 核验</dt><dd>task-center-ui / human_confirmed</dd></div></dl>
        <p>确认后才会原子创建任务并追加 created 事件；若编号冲突或事件失败，不留下新任务。</p>
      </div>}
      {failure && <div role="alert" className="write-failure"><strong>未写入</strong><span>{failure.message}</span>{["conflict", "event_conflict"].includes(failure.code) && <small>当前新建草稿仍保留，请重新生成预览。</small>}</div>}
      {receipt && <div role="status" className="write-success"><strong>新任务已创建并核对</strong><span>{receipt.taskId} · 任务与事件回读一致</span></div>}
      <div className="write-actions dialog-actions">
        {!preview && !receipt && <button disabled={writing || !draft.title.trim() || !draft.domain.trim()} onClick={onPreview}>生成新建预览</button>}
        {preview && <button className="confirm-write" disabled={writing} onClick={onConfirm}>{writing ? "正在安全创建…" : "确认创建"}</button>}
        <button className="secondary" disabled={writing} onClick={onClose}>{receipt ? "完成" : "取消"}</button>
      </div>
    </aside>
  </div>;
}
