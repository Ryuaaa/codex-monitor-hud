import { useEffect, useMemo, useRef, useState } from "react";
import { codexThreadIds, deriveAttention, parseTask } from "../domain/task";
import type {
  PriorityEditPreview,
  PriorityEditReceipt,
  CodexHistoryFailure,
  CodexThreadListPage,
  CodexThreadPage,
  CodexThreadSummary,
  CodexTurnSummary,
  CreateTaskPreview,
  CreateTaskReceipt,
  NewTaskDraft,
  ProjectMapping,
  TaskEvent,
  TaskLoadIssue,
  TaskPriority,
  TaskRecord,
  TaskStatus,
  TaskFieldEditPreview,
  TaskFieldEditReceipt,
  TaskWriteFailure,
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
  const [statusFilter, setStatusFilter] = useState<TaskStatus | "all">("all");
  const [compact, setCompact] = useState(false);
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
  const [priorityDraft, setPriorityDraft] = useState<"high" | "medium" | "low">();
  const [writePreview, setWritePreview] = useState<PriorityEditPreview>();
  const [writeReceipt, setWriteReceipt] = useState<PriorityEditReceipt>();
  const [writeFailure, setWriteFailure] = useState<TaskWriteFailure>();
  const [writing, setWriting] = useState(false);
  const [fieldDraft, setFieldDraft] = useState<{ field: WritableTaskField; rawValue: string }>();
  const [fieldPreview, setFieldPreview] = useState<TaskFieldEditPreview>();
  const [fieldReceipt, setFieldReceipt] = useState<TaskFieldEditReceipt>();
  const [createOpen, setCreateOpen] = useState(false);
  const [createDraft, setCreateDraft] = useState<NewTaskDraft>({
    title: "", domain: "task_hub", taskStatus: "todo", priority: "medium",
    assignee: "本人", deadline: "", relatedIds: [],
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
    return () => {
      codexListGeneration.current += 1;
      codexHistoryGeneration.current += 1;
      managedLoadGeneration.current += 1;
      taskDetailGeneration.current += 1;
    };
  }, [provider]);

  useEffect(() => {
    if (section === "managed" && managedLoadState === "idle") {
      void loadManagedTasks();
    }
  }, [section, managedLoadState]);

  const filtered = useMemo(() => {
    const normalized = query.trim().toLowerCase();
    return tasks.filter((task) => {
      const projectMatch = selectedProject === "all" || task.projectId === selectedProject || task.domain === selectedProject;
      const statusMatch = statusFilter === "all" || task.status === statusFilter;
      const archiveMatch = showArchived || task.recordStatus !== "archived";
      const searchMatch = !normalized || [task.title, task.domain, task.category, task.assignee, task.workflowStatus]
        .filter(Boolean).join(" ").toLowerCase().includes(normalized);
      return projectMatch && statusMatch && archiveMatch && searchMatch;
    });
  }, [tasks, query, selectedProject, statusFilter, showArchived]);

  const filteredCodexThreads = useMemo(() => {
    const normalized = codexQuery.trim().toLowerCase();
    if (!normalized) return codexThreads;
    return codexThreads.filter((thread) => [thread.name, thread.workspaceName, thread.sourceLabel, thread.threadId]
      .filter(Boolean).join(" ").toLowerCase().includes(normalized));
  }, [codexThreads, codexQuery]);

  const projectOptions = useMemo(() => {
    const mapped = projects.map((project) => ({ id: project.id, name: project.name, workdirs: project.workdirs }));
    const mappedIds = new Set(mapped.map((project) => project.id));
    const domains = [...new Set(tasks.filter((task) => !task.projectId || !mappedIds.has(task.projectId)).map((task) => task.domain))]
      .map((domain) => ({ id: domain, name: domain, workdirs: [] as string[] }));
    return [...mapped, ...domains];
  }, [projects, tasks]);

  async function loadCodexThreads(cursor?: string, replace = false) {
    const generation = codexListGeneration.current + 1;
    codexListGeneration.current = generation;
    setCodexListLoading(true);
    setCodexListFailure(undefined);
    if (replace) {
      setCodexThreads([]);
      setCodexListPage(undefined);
    }
    try {
      const page = await provider.loadCodexThreadList(cursor);
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

  function openCodexThread(thread: CodexThreadSummary) {
    codexHistoryGeneration.current += 1;
    setCodexHistory({});
    setSelectedCodex(thread);
  }

  function closeDetails() {
    taskDetailGeneration.current += 1;
    codexHistoryGeneration.current += 1;
    resetWriteFlow();
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
    if (field === "related_ids") return task.relatedIds.join(", ");
    return task.recordStatus === "unknown" ? "current" : task.recordStatus;
  }

  function fieldValue(draft: { field: WritableTaskField; rawValue: string }): unknown {
    if (draft.field === "related_ids") {
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

  function resetCreateFlow() {
    setCreateOpen(false);
    setCreatePreview(undefined);
    setCreateReceipt(undefined);
    setCreateFailure(undefined);
    setCreateDraft({ title: "", domain: "task_hub", taskStatus: "todo", priority: "medium", assignee: "本人", deadline: "", relatedIds: [] });
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

  return (
    <div className={compact ? "app compact" : "app"}>
      <header className="topbar">
        <div>
          <p className="eyebrow">CODEX MONITOR</p>
          <h1>任务中心 <span>{section === "codex" ? "官方只读" : "安全写入预览"}</span></h1>
        </div>
        <div className="top-actions" aria-label="视图设置">
          {section === "codex" ? <button disabled={codexListLoading} onClick={() => loadCodexThreads(undefined, true)}>{codexListLoading ? "读取中…" : "刷新"}</button> : <>
            {managedLoadState === "ready" && <button className="create-entry" onClick={() => { resetCreateFlow(); setCreateOpen(true); }}>新建任务</button>}
            <button className={view === "board" ? "active" : ""} onClick={() => setView("board")}>看板</button>
            <button className={view === "list" ? "active" : ""} onClick={() => setView("list")}>列表</button>
            <button aria-pressed={showArchived} onClick={() => setShowArchived((value) => !value)}>归档</button>
          </>}
          <button aria-pressed={compact} onClick={() => setCompact((value) => !value)}>紧凑</button>
        </div>
      </header>

      <div className="workspace">
        <aside className="sidebar" aria-label="任务中心导航">
          <nav className="source-nav" aria-label="数据来源">
            <button className={section === "codex" ? "source-entry active" : "source-entry"} onClick={() => { closeDetails(); setLoadError(undefined); setSection("codex"); }}>
              <span>Codex 活动<small>官方任务列表 · 自动读取</small></span><strong>{codexThreads.length}</strong>
            </button>
            <button className={section === "managed" ? "source-entry active" : "source-entry"} onClick={() => { closeDetails(); setLoadError(undefined); setSection("managed"); }}>
              <span>管理任务<small>可选本地任务库</small></span><strong>{managedLoadState === "ready" ? tasks.length : "—"}</strong>
            </button>
          </nav>
          {section === "managed" && <div className="project-nav" aria-label="项目">
            <button className={selectedProject === "all" ? "project active" : "project"} onClick={() => setSelectedProject("all")}>
              <span>全项目</span><strong>{tasks.length}</strong>
            </button>
            {projectOptions.map((project) => (
              <button key={project.id} className={selectedProject === project.id ? "project active" : "project"} onClick={() => setSelectedProject(project.id)}>
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
          {section === "codex" ? <CodexActivity
            threads={filteredCodexThreads}
            allThreads={codexThreads}
            page={codexListPage}
            loading={codexListLoading}
            failure={codexListFailure}
            query={codexQuery}
            onQuery={setCodexQuery}
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
                <label><span>状态</span><select value={statusFilter} onChange={(event) => setStatusFilter(event.target.value as TaskStatus | "all")}>
                  <option value="all">全部状态</option>
                  {columns.map((status) => <option key={status} value={status}>{statusLabels[status]}</option>)}
                </select></label>
                <div className="result-count">{filtered.length} 项</div>
              </section>

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
        onLoadCodexHistory={loadCodexHistory}
        onClose={closeDetails}
      />}
      {selectedCodex && <CodexActivityDetail
        thread={selectedCodex}
        history={codexHistory[selectedCodex.threadId]}
        onLoad={loadCodexHistory}
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
    </div>
  );
}

function CodexActivity({
  threads, allThreads, page, loading, failure, query,
  onQuery, onOpen, onRetry, onMore,
}: {
  threads: CodexThreadSummary[];
  allThreads: CodexThreadSummary[];
  page?: CodexThreadListPage;
  loading: boolean;
  failure?: CodexHistoryFailure;
  query: string;
  onQuery: (value: string) => void;
  onOpen: (thread: CodexThreadSummary) => void;
  onRetry: () => void;
  onMore: (cursor: string) => void;
}) {
  const workspaces = new Set(allThreads.map((thread) => thread.workspaceName).filter(Boolean));
  return <>
    <section className="source-intro">
      <div><p className="eyebrow">DEFAULT DATA SOURCE</p><h2>Codex 活动</h2></div>
      <p>直接读取本机官方任务列表，不需要个人任务目录。只显示任务名称与必要元数据，不使用对话预览作为标题。</p>
    </section>
    <section className="summary" aria-label="Codex 活动概况">
      <Metric label="已读取任务" value={allThreads.length} tone="blue" />
      <Metric label="已有名称" value={allThreads.filter((thread) => thread.name).length} tone="green" />
      <Metric label="置顶任务" value={allThreads.filter((thread) => thread.isPinned).length} tone="orange" />
      <Metric label="工作目录" value={workspaces.size} tone="muted" />
    </section>
    <section className="toolbar" aria-label="Codex 活动搜索">
      <label className="search"><span>搜索</span><input value={query} onChange={(event) => onQuery(event.target.value)} placeholder="任务名称、项目、来源…" /></label>
      <div className="result-count">{threads.length} 项</div>
    </section>
    {failure && <div role="alert" className="alert codex-list-error"><span>{failure.message}</span><button disabled={loading} onClick={onRetry}>重新读取</button></div>}
    {loading && !allThreads.length && <div className="empty-state" role="status"><strong>正在读取 Codex 官方任务…</strong><p>完成后接口进程会立即退出。</p></div>}
    {!loading && !failure && !allThreads.length && <div className="empty-state"><strong>没有可显示的 Codex 任务</strong><p>Codex 未安装、未登录或尚无任务时都可能出现此状态。</p></div>}
    {allThreads.length > 0 && <section className="codex-activity-list" aria-label="Codex 官方任务列表">
      <div className="codex-list-head"><span>任务</span><span>来源</span><span>项目</span><span>最近活动</span><span>属性</span></div>
      {threads.map((thread) => <button key={thread.threadId} className="codex-activity-row" onClick={() => onOpen(thread)}>
        <span><strong>{thread.name ?? "未命名 Codex 任务"}</strong><small>{thread.threadId}</small></span>
        <span>{thread.sourceLabel}</span>
        <span>{thread.workspaceName ?? "—"}</span>
        <span>{formatUnixTime(thread.updatedAt ?? thread.createdAt)}</span>
        <span>{thread.isPinned ? "已置顶" : "普通"}</span>
      </button>)}
      {!threads.length && <p className="no-search-result">没有匹配当前搜索的任务。</p>}
    </section>}
    {page?.nextCursor && <button className="load-more codex-list-more" disabled={loading} onClick={() => onMore(page.nextCursor!)}>{loading ? "正在加载…" : "加载更多官方任务"}</button>}
    {page && <p className="observed-at">本轮读取：{formatUnixTime(page.observedAt)} · 官方记录不等于桌面版实时运行状态</p>}
  </>;
}

function CodexActivityDetail({
  thread, history, onLoad, onClose,
}: {
  thread: CodexThreadSummary;
  history?: CodexHistoryView;
  onLoad: (threadId: string, cursor?: string) => void;
  onClose: () => void;
}) {
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
        <div><dt>记录状态</dt><dd>{codexReportedStatus(thread.reportedStatus)}</dd></div>
      </dl>
      <section><h3>任务编号</h3><p className="thread-id-copy">{thread.threadId}</p></section>
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
    <div className="badges"><span className={`priority ${task.priority}`}>{priorityLabels[task.priority]}</span>{task.recordStatus === "archived" && <span className="archived">已归档</span>}{attention.map((hint) => <span className="attention" key={hint}>{hint}</span>)}</div>
    <h3>{task.title}</h3>
    {task.workflowStatus && <p>{task.workflowStatus}</p>}
    <footer><span>{task.domain}</span><span>{threads.length ? `Codex × ${threads.length}` : task.assignee ?? "未分配"}</span></footer>
  </button>;
}

function DetailPanel({
  task, body, events, loading, priorityDraft, writePreview, writeReceipt, writeFailure, writing,
  fieldDraft, fieldPreview, fieldReceipt, codexHistory,
  onBeginPriority, onPriorityDraft, onPreviewPriority, onConfirmPriority, onCancelPriority,
  onBeginField, onFieldChange, onFieldValue, onPreviewField, onConfirmField, onCancelField,
  onLoadCodexHistory, onClose,
}: {
  task: TaskRecord;
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
  onLoadCodexHistory: (threadId: string, cursor?: string) => void;
  onClose: () => void;
}) {
  const threads = codexThreadIds(task);
  useEffect(() => {
    const closeOnEscape = (event: KeyboardEvent) => event.key === "Escape" && onClose();
    window.addEventListener("keydown", closeOnEscape);
    return () => window.removeEventListener("keydown", closeOnEscape);
  }, [onClose]);
  return <div className="scrim" onMouseDown={(event) => event.target === event.currentTarget && onClose()}>
    <aside className="detail" role="dialog" aria-modal="true" aria-labelledby="detail-title">
      <div className="detail-head"><div><p className="eyebrow">{task.id}</p><h2 id="detail-title">{task.title}</h2></div><button autoFocus aria-label="关闭详情" onClick={onClose}>×</button></div>
      <dl className="facts"><div><dt>状态</dt><dd>{statusLabels[task.status]}</dd></div><div><dt>优先级</dt><dd>{priorityLabels[task.priority]}</dd></div><div><dt>负责人</dt><dd>{task.assignee ?? "—"}</dd></div><div><dt>截止</dt><dd>{task.deadline ?? "—"}</dd></div></dl>
      <section className="safe-write" aria-label="安全编辑">
        <div className="section-title"><h3>安全编辑</h3>{!priorityDraft && !fieldDraft && <div className="edit-entry-actions"><button onClick={onBeginPriority}>快速改优先级</button><button onClick={onBeginField}>编辑其他字段</button></div>}</div>
        <p className="write-boundary">仅写正式支持字段；评论、附件、重复任务、甘特图与工作流等“正式结构暂不支持”，也不会保存到缓存。</p>
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
            <option value="title">标题</option><option value="task_status">任务状态</option><option value="priority">优先级</option><option value="deadline">截止日期</option><option value="assignee">负责人</option><option value="related_ids">关联任务</option><option value="record_status">归档/恢复</option>
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
      <section><h3>Codex 对话 <span className="on-demand">官方历史按需读取</span></h3>{threads.length ? threads.map((id) => <CodexThreadHistory
        key={id}
        threadId={id}
        view={codexHistory[id]}
        onLoad={onLoadCodexHistory}
      />) : <p className="muted-text">没有正式绑定记录。</p>}</section>
      <section><h3>正文 <span className="on-demand">按需读取</span></h3>{loading ? <p>正在读取…</p> : <pre className="body">{body ?? "正文不可用"}</pre>}</section>
      <section><h3>活动时间线</h3>{events.length ? <ol className="timeline">{events.map((event) => <li key={event.id}><time>{new Date(event.occurredAt).toLocaleString("zh-CN")}</time><strong>{event.eventType}</strong><span>{event.previousTaskStatus ?? "—"} → {event.newTaskStatus ?? "—"}</span></li>)}</ol> : <p className="muted-text">没有可显示的正式事件。</p>}</section>
    </aside>
  </div>;
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
    placeholder={draft.field === "related_ids" ? "多个任务编号用英文逗号分隔" : undefined}
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
        <label><span>关联任务</span><input aria-label="新任务关联任务" value={draft.relatedIds.join(", ")} placeholder="多个任务编号用英文逗号分隔" onChange={(event) => update("relatedIds", event.target.value.split(",").map((value) => value.trim()).filter(Boolean))} /></label>
      </div>}
      {preview && <div className="write-preview create-preview" role="region" aria-label="新建任务预览">
        <strong>确认新建内容</strong>
        <dl><div><dt>任务编号</dt><dd>{preview.taskId}</dd></div><div><dt>标题</dt><dd>{preview.draft.title}</dd></div><div><dt>状态</dt><dd>{displayWriteValue(preview.draft.taskStatus)}</dd></div><div><dt>优先级</dt><dd>{displayWriteValue(preview.draft.priority)}</dd></div><div><dt>归属</dt><dd>{preview.draft.domain}</dd></div><div><dt>负责人</dt><dd>{preview.draft.assignee || "（空）"}</dd></div><div><dt>截止</dt><dd>{preview.draft.deadline || "（空）"}</dd></div><div><dt>关联任务</dt><dd>{preview.draft.relatedIds.length ? preview.draft.relatedIds.join("、") : "（空）"}</dd></div><div><dt>隐私 / 访问</dt><dd>general / proposal_only</dd></div><div><dt>来源 / 核验</dt><dd>task-center-ui / human_confirmed</dd></div></dl>
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
