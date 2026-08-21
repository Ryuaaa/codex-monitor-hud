import { useEffect, useMemo, useState } from "react";
import { codexThreadIds, deriveAttention, parseTask } from "../domain/task";
import type { ProjectMapping, TaskEvent, TaskLoadIssue, TaskRecord, TaskStatus } from "../domain/types";
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

  return (
    <div className={compact ? "app compact" : "app"}>
      <header className="topbar">
        <div>
          <p className="eyebrow">CODEX MONITOR</p>
          <h1>任务中心 <span>只读预览</span></h1>
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

      {selected && <DetailPanel task={selected} body={body} events={events} loading={loadingDetail} onClose={() => setSelected(undefined)} />}
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

function DetailPanel({ task, body, events, loading, onClose }: { task: TaskRecord; body?: string; events: TaskEvent[]; loading: boolean; onClose: () => void }) {
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
      {task.nextAction && <section><h3>下一步</h3><p>{task.nextAction}</p></section>}
      <section><h3>Codex 对话</h3>{threads.length ? threads.map((id) => <div className="thread" key={id}><span>{id}</span><span className="runtime unknown">外部任务 · 状态未知</span><button disabled title="尚无已验证的官方第三方打开方式">打开任务</button></div>) : <p className="muted-text">没有正式绑定记录。</p>}</section>
      <section><h3>正文 <span className="on-demand">按需读取</span></h3>{loading ? <p>正在读取…</p> : <pre className="body">{body ?? "正文不可用"}</pre>}</section>
      <section><h3>活动时间线</h3>{events.length ? <ol className="timeline">{events.map((event) => <li key={event.id}><time>{new Date(event.occurredAt).toLocaleString("zh-CN")}</time><strong>{event.eventType}</strong><span>{event.previousTaskStatus ?? "—"} → {event.newTaskStatus ?? "—"}</span></li>)}</ol> : <p className="muted-text">没有可显示的正式事件。</p>}</section>
    </aside>
  </div>;
}
