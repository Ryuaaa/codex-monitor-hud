import { useEffect, useMemo, useState } from "react";
import { codexThreadIds, deriveAttention, parseTask } from "../domain/task";
import type {
  PriorityEditPreview,
  PriorityEditReceipt,
  ProjectMapping,
  TaskEvent,
  TaskLoadIssue,
  TaskPriority,
  TaskRecord,
  TaskStatus,
  TaskWriteFailure,
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

export function App({ provider = taskDataProvider }: { provider?: TaskDataProvider }) {
  const [tasks, setTasks] = useState<TaskRecord[]>([]);
  const [issues, setIssues] = useState<TaskLoadIssue[]>([]);
  const [projects, setProjects] = useState<ProjectMapping[]>([]);
  const [selectedProject, setSelectedProject] = useState("all");
  const [view, setView] = useState<"board" | "list">("board");
  const [query, setQuery] = useState("");
  const [statusFilter, setStatusFilter] = useState<TaskStatus | "all">("all");
  const [compact, setCompact] = useState(false);
  const [selected, setSelected] = useState<TaskRecord>();
  const [body, setBody] = useState<string>();
  const [events, setEvents] = useState<TaskEvent[]>([]);
  const [loadingDetail, setLoadingDetail] = useState(false);
  const [loadError, setLoadError] = useState<string>();
  const [priorityDraft, setPriorityDraft] = useState<"high" | "medium" | "low">();
  const [writePreview, setWritePreview] = useState<PriorityEditPreview>();
  const [writeReceipt, setWriteReceipt] = useState<PriorityEditReceipt>();
  const [writeFailure, setWriteFailure] = useState<TaskWriteFailure>();
  const [writing, setWriting] = useState(false);

  useEffect(() => {
    let live = true;
    Promise.all([provider.loadMetadata(), provider.loadProjectMappings()])
      .then(([sources, mappings]) => {
        if (!live) return;
        const parsed = sources.map(parseTask);
        setTasks(parsed.flatMap((result) => (result.task ? [result.task] : [])));
        setIssues(parsed.flatMap((result) => (result.issue ? [result.issue] : [])));
        setProjects(mappings);
      })
      .catch(() => live && setLoadError("任务元数据暂时不可用；未读取任何正文。"));
    return () => { live = false; };
  }, [provider]);

  const filtered = useMemo(() => {
    const normalized = query.trim().toLowerCase();
    return tasks.filter((task) => {
      const projectMatch = selectedProject === "all" || task.projectId === selectedProject || task.domain === selectedProject;
      const statusMatch = statusFilter === "all" || task.status === statusFilter;
      const searchMatch = !normalized || [task.title, task.domain, task.category, task.assignee, task.workflowStatus]
        .filter(Boolean).join(" ").toLowerCase().includes(normalized);
      return projectMatch && statusMatch && searchMatch;
    });
  }, [tasks, query, selectedProject, statusFilter]);

  const projectOptions = useMemo(() => {
    const mapped = projects.map((project) => ({ id: project.id, name: project.name, workdirs: project.workdirs }));
    const mappedIds = new Set(mapped.map((project) => project.id));
    const domains = [...new Set(tasks.filter((task) => !task.projectId || !mappedIds.has(task.projectId)).map((task) => task.domain))]
      .map((domain) => ({ id: domain, name: domain, workdirs: [] as string[] }));
    return [...mapped, ...domains];
  }, [projects, tasks]);

  async function openTask(task: TaskRecord) {
    setSelected(task);
    setBody(undefined);
    setEvents([]);
    setLoadingDetail(true);
    setLoadError(undefined);
    resetWriteFlow();
    try {
      const [nextBody, nextEvents] = await Promise.all([
        provider.loadBody(task.fileToken),
        provider.loadEvents(task.id),
      ]);
      setBody(nextBody);
      setEvents(nextEvents);
    } catch {
      setLoadError("该任务详情读取失败；其他任务仍可继续使用。 ");
    } finally {
      setLoadingDetail(false);
    }
  }

  function resetWriteFlow() {
    setPriorityDraft(undefined);
    setWritePreview(undefined);
    setWriteReceipt(undefined);
    setWriteFailure(undefined);
    setWriting(false);
  }

  function normalizeWriteError(error: unknown): TaskWriteFailure {
    if (error && typeof error === "object" && "code" in error && "message" in error) {
      return { code: String(error.code), message: String(error.message) };
    }
    return { code: "write_failed", message: "写入失败，原任务已保留。" };
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
      if (failure.code === "conflict") await refreshAfterConflict(selected.id);
    } finally {
      setWriting(false);
    }
  }

  return (
    <div className={compact ? "app compact" : "app"}>
      <header className="topbar">
        <div>
          <p className="eyebrow">CODEX MONITOR</p>
          <h1>任务中心 <span>安全写入预览</span></h1>
        </div>
        <div className="top-actions" aria-label="视图设置">
          <button className={view === "board" ? "active" : ""} onClick={() => setView("board")}>看板</button>
          <button className={view === "list" ? "active" : ""} onClick={() => setView("list")}>列表</button>
          <button aria-pressed={compact} onClick={() => setCompact((value) => !value)}>紧凑</button>
        </div>
      </header>

      <div className="workspace">
        <aside className="sidebar" aria-label="项目">
          <button className={selectedProject === "all" ? "project active" : "project"} onClick={() => setSelectedProject("all")}>
            <span>全项目</span><strong>{tasks.length}</strong>
          </button>
          {projectOptions.map((project) => (
            <button key={project.id} className={selectedProject === project.id ? "project active" : "project"} onClick={() => setSelectedProject(project.id)}>
              <span>{project.name}<small>{project.workdirs[0] ?? "按正式领域归类"}</small></span>
              <strong>{tasks.filter((task) => task.projectId === project.id || task.domain === project.id).length}</strong>
            </button>
          ))}
          <div className="boundary-note">
            <strong>运行边界</strong>
            <p>独立进程 · 无后台服务<br />关闭窗口即退出</p>
          </div>
        </aside>

        <main>
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
                <span><strong>{task.title}</strong><small>{task.id}</small></span>
                <span>{statusLabels[task.status]}</span><span>{priorityLabels[task.priority]}</span><span>{task.domain}</span><span>{task.updatedAt ?? "—"}</span>
              </button>)}
            </section>
          )}
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
        onClose={() => { resetWriteFlow(); setSelected(undefined); }}
      />}
    </div>
  );
}

function Metric({ label, value, tone }: { label: string; value: number; tone: string }) {
  return <div className={`metric ${tone}`}><strong>{value}</strong><span>{label}</span></div>;
}

function TaskCard({ task, onOpen }: { task: TaskRecord; onOpen: () => void }) {
  const attention = deriveAttention(task);
  const threads = codexThreadIds(task);
  return <button className="task-card" onClick={onOpen}>
    <div className="badges"><span className={`priority ${task.priority}`}>{priorityLabels[task.priority]}</span>{attention.map((hint) => <span className="attention" key={hint}>{hint}</span>)}</div>
    <h3>{task.title}</h3>
    {task.workflowStatus && <p>{task.workflowStatus}</p>}
    <footer><span>{task.domain}</span><span>{threads.length ? `Codex × ${threads.length}` : task.assignee ?? "未分配"}</span></footer>
  </button>;
}

function DetailPanel({
  task, body, events, loading, priorityDraft, writePreview, writeReceipt, writeFailure, writing,
  onBeginPriority, onPriorityDraft, onPreviewPriority, onConfirmPriority, onCancelPriority, onClose,
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
  onBeginPriority: () => void;
  onPriorityDraft: (value: "high" | "medium" | "low") => void;
  onPreviewPriority: () => void;
  onConfirmPriority: () => void;
  onCancelPriority: () => void;
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
        <div className="section-title"><h3>安全编辑</h3>{!priorityDraft && <button onClick={onBeginPriority}>编辑优先级</button>}</div>
        <p className="write-boundary">仅写正式支持字段；评论、附件、重复任务、甘特图与工作流等暂不支持，也不会保存到缓存。</p>
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
        {writeFailure && <div role="alert" className="write-failure"><strong>未写入</strong><span>{writeFailure.message}</span>{writeFailure.code === "conflict" && <small>已重新读取当前任务；你的优先级草稿仍保留。</small>}</div>}
        {writeReceipt && <div role="status" className="write-success"><strong>已写入并核对</strong><span>任务与事件均已回读一致。</span></div>}
      </section>
      {task.nextAction && <section><h3>下一步</h3><p>{task.nextAction}</p></section>}
      <section><h3>Codex 对话</h3>{threads.length ? threads.map((id) => <div className="thread" key={id}><span>{id}</span><span className="runtime unknown">外部任务 · 状态未知</span><button disabled title="尚无已验证的官方第三方打开方式">打开任务</button></div>) : <p className="muted-text">没有正式绑定记录。</p>}</section>
      <section><h3>正文 <span className="on-demand">按需读取</span></h3>{loading ? <p>正在读取…</p> : <pre className="body">{body ?? "正文不可用"}</pre>}</section>
      <section><h3>活动时间线</h3>{events.length ? <ol className="timeline">{events.map((event) => <li key={event.id}><time>{new Date(event.occurredAt).toLocaleString("zh-CN")}</time><strong>{event.eventType}</strong><span>{event.previousTaskStatus ?? "—"} → {event.newTaskStatus ?? "—"}</span></li>)}</ol> : <p className="muted-text">没有可显示的正式事件。</p>}</section>
    </aside>
  </div>;
}
