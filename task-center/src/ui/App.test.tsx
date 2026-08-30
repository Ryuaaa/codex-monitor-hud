import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";
import { fixtureEvents, fixtureProjects, fixtureSources } from "../data/fixtures";
import type { TaskDataProvider } from "../data/provider";
import { App } from "./App";

function provider(): TaskDataProvider {
  const savedFilters = new Map<string, {
    id: string;
    name: string;
    projectId: string;
    status: "all" | "todo" | "doing" | "long_term" | "done" | "cancelled" | "unknown";
    tag: string;
    showArchived: boolean;
    view: "board" | "list";
  }>();
  return {
    loadMetadata: vi.fn().mockResolvedValue(fixtureSources),
    loadProjectMappings: vi.fn().mockResolvedValue(fixtureProjects),
    loadBody: vi.fn().mockResolvedValue("# 按需正文"),
    loadEvents: vi.fn().mockResolvedValue(fixtureEvents),
    loadCodexThreadList: vi.fn().mockImplementation((cursor) => Promise.resolve({
      threads: cursor ? [{
        threadId: "019f-demo-older",
        name: "较早的 Codex 任务",
        sourceKind: "cli",
        sourceLabel: "Codex 命令行",
        reportedStatus: "notLoaded",
        createdAt: 50,
        updatedAt: 60,
        workspaceName: "older-project",
        isPinned: false,
      }] : [{
        threadId: "019f-demo-running",
        name: "合成 Codex 任务",
        sourceKind: "vscode",
        sourceLabel: "Codex 桌面版或编辑器",
        reportedStatus: "notLoaded",
        createdAt: 100,
        updatedAt: 200,
        workspaceName: "codex-monitor",
        isPinned: true,
      }],
      nextCursor: cursor ? undefined : "next-list-page",
      observedAt: 201,
    })),
    loadCodexThreadPage: vi.fn().mockImplementation((threadId, cursor) => Promise.resolve({
      threadId,
      name: "合成 Codex 任务",
      sourceKind: "vscode",
      sourceLabel: "Codex 桌面版或编辑器",
      reportedStatus: "notLoaded",
      createdAt: 100,
      updatedAt: 200,
      historyMode: "paginated",
      turns: [{
        id: cursor ? "turn_older" : "turn_latest",
        status: "completed",
        startedAt: cursor ? 150 : 190,
        completedAt: cursor ? 160 : 200,
        durationMs: 10_000,
      }],
      nextCursor: cursor ? undefined : "older-cursor",
      historyState: "paged",
      observedAt: 201,
    })),
    initializeLocalTaskLibrary: vi.fn().mockResolvedValue(undefined),
    loadSavedFilters: vi.fn().mockImplementation(() => Promise.resolve([...savedFilters.values()])),
    saveTaskFilter: vi.fn().mockImplementation((draft) => {
      const saved = { ...draft, id: draft.id ?? `filter-${savedFilters.size + 1}` };
      savedFilters.set(saved.id, saved);
      return Promise.resolve(saved);
    }),
    deleteTaskFilter: vi.fn().mockImplementation((id) => {
      savedFilters.delete(id);
      return Promise.resolve();
    }),
    previewPriorityEdit: vi.fn().mockImplementation((fileToken, newPriority) => Promise.resolve({
      fileToken,
      taskId: "tsk_demo_governance",
      beforePriority: "high",
      afterPriority: newPriority,
      expectedHash: "synthetic-sha256",
    })),
    applyPriorityEdit: vi.fn().mockImplementation((request) => Promise.resolve({
      fileToken: request.fileToken,
      taskId: "tsk_demo_governance",
      previousPriority: "high",
      newPriority: request.newPriority,
      fileHash: "updated-synthetic-sha256",
      eventId: "evt_synthetic_priority",
      eventFile: "2026-08.jsonl",
      verified: true,
    })),
    previewTaskFieldEdit: vi.fn().mockImplementation((fileToken, field, newValue) => Promise.resolve({
      fileToken,
      taskId: "tsk_demo_governance",
      field,
      beforeValue: field === "task_status" ? "doing" : "before",
      afterValue: newValue,
      expectedHash: "field-synthetic-sha256",
    })),
    applyTaskFieldEdit: vi.fn().mockImplementation((request) => Promise.resolve({
      fileToken: request.fileToken,
      taskId: "tsk_demo_governance",
      field: request.field,
      previousValue: "before",
      newValue: request.newValue,
      fileHash: "field-updated-synthetic-sha256",
      eventId: "evt_synthetic_field",
      eventFile: "2026-08.jsonl",
      verified: true,
    })),
    previewCreateTask: vi.fn().mockImplementation((draft) => Promise.resolve({
      draft,
      taskId: "tsk_synthetic_new",
      fileToken: "tsk_synthetic_new.md",
      createdAt: "2026-08-24",
      occurredAt: "2026-08-24T08:00:00Z",
      expectedHash: "create-synthetic-sha256",
    })),
    applyCreateTask: vi.fn().mockImplementation((preview) => Promise.resolve({
      taskId: preview.taskId,
      fileToken: preview.fileToken,
      fileHash: preview.expectedHash,
      eventId: "evt_synthetic_created",
      eventFile: "2026-08.jsonl",
      verified: true,
    })),
    previewTaskNote: vi.fn().mockImplementation((fileToken, kind, text, author) => Promise.resolve({
      fileToken,
      taskId: "tsk_demo_governance",
      kind,
      text,
      author,
      occurredAt: "2026-08-24T08:00:00Z",
      expectedTaskHash: "note-task-synthetic-sha256",
      expectedEventHash: "note-event-synthetic-sha256",
    })),
    applyTaskNote: vi.fn().mockImplementation((preview) => Promise.resolve({
      taskId: preview.taskId,
      eventId: "evt_synthetic_note",
      eventFile: "2026-08.jsonl",
      verified: true,
    })),
    checkTaskCenterUpdate: vi.fn().mockResolvedValue({ currentVersion: "1.2.0", available: false }),
    installTaskCenterUpdate: vi.fn().mockResolvedValue(undefined),
  };
}

function switchToManagedTasks() {
  fireEvent.click(screen.getByRole("button", { name: /管理任务/ }));
}

async function enterManagedTasks() {
  switchToManagedTasks();
  await screen.findByText("统一个人 AI 规则与能力边界");
}

describe("任务中心核心流程", () => {
  it("每天至多自动检查一次，并在安装前重新绑定用户看到的版本", async () => {
    localStorage.setItem("codex-monitor-task-center.last-update-check", String(Date.now()));
    const mock = provider();
    vi.mocked(mock.checkTaskCenterUpdate).mockResolvedValue({ currentVersion: "1.2.0", available: true, version: "1.3.0" });
    render(<App provider={mock} />);
    expect(mock.checkTaskCenterUpdate).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole("button", { name: "检查更新" }));
    expect(await screen.findByRole("status")).toHaveTextContent("发现任务中心 1.3.0");
    fireEvent.click(screen.getByRole("button", { name: "安装 1.3.0" }));
    await waitFor(() => expect(mock.installTaskCenterUpdate).toHaveBeenCalledWith("1.3.0"));
  });

  it("默认自动读取官方 Codex 任务且不访问个人任务目录", async () => {
    const mock = provider();
    render(<App provider={mock} />);
    expect(await screen.findByText("合成 Codex 任务")).toBeInTheDocument();
    expect(screen.getByText("直接读取本机官方任务列表，不需要个人任务目录。只显示任务名称与必要元数据，不使用对话预览作为标题。")).toBeInTheDocument();
    expect(mock.loadCodexThreadList).toHaveBeenCalledWith(undefined);
    expect(mock.loadMetadata).not.toHaveBeenCalled();
    expect(mock.loadProjectMappings).not.toHaveBeenCalled();
  });

  it("Codex 官方任务列表通过游标加载更多", async () => {
    const mock = provider();
    render(<App provider={mock} />);
    await screen.findByText("合成 Codex 任务");
    fireEvent.click(screen.getByRole("button", { name: "加载更多官方任务" }));
    expect(await screen.findByText("较早的 Codex 任务")).toBeInTheDocument();
    expect(mock.loadCodexThreadList).toHaveBeenNthCalledWith(2, "next-list-page");
  });

  it("Codex 活动详情仍需用户点击才读取轮次历史", async () => {
    const mock = provider();
    render(<App provider={mock} />);
    fireEvent.click(await screen.findByRole("button", { name: /合成 Codex 任务/ }));
    expect(mock.loadMetadata).not.toHaveBeenCalled();
    expect(mock.loadCodexThreadPage).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole("button", { name: "读取历史" }));
    expect(await screen.findByText("按需分页")).toBeInTheDocument();
    expect(mock.loadCodexThreadPage).toHaveBeenCalledWith("019f-demo-running", undefined);
  });

  it("Codex 列表失败不会触发个人任务目录读取", async () => {
    const mock = provider();
    vi.mocked(mock.loadCodexThreadList).mockRejectedValue({ code: "provider_unavailable", message: "未找到本机 Codex 官方接口" });
    render(<App provider={mock} />);
    expect(await screen.findByRole("alert")).toHaveTextContent("未找到本机 Codex 官方接口");
    expect(mock.loadMetadata).not.toHaveBeenCalled();
    expect(mock.loadProjectMappings).not.toHaveBeenCalled();
  });

  it("管理任务库仅在用户明确点击后创建并重新读取", async () => {
    const mock = provider();
    vi.mocked(mock.loadMetadata)
      .mockRejectedValueOnce(new Error("missing library"))
      .mockResolvedValue(fixtureSources);
    render(<App provider={mock} />);
    switchToManagedTasks();
    expect(await screen.findByRole("alert")).toHaveTextContent("正式任务库尚未连接或不可用");
    expect(mock.initializeLocalTaskLibrary).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole("button", { name: "创建本地任务库" }));
    expect(await screen.findByText("统一个人 AI 规则与能力边界")).toBeInTheDocument();
    expect(mock.initializeLocalTaskLibrary).toHaveBeenCalledTimes(1);
    expect(mock.loadMetadata).toHaveBeenCalledTimes(2);
  });

  it("项目切换、看板、列表、搜索共享同一任务数据", async () => {
    render(<App provider={provider()} />);
    await enterManagedTasks();
    expect(await screen.findByText("统一个人 AI 规则与能力边界")).toBeInTheDocument();
    fireEvent.click(within(screen.getByLabelText("项目")).getByRole("button", { name: /日常生活/ }));
    expect(screen.getByText("完成 Mac 键盘决策级研究")).toBeInTheDocument();
    expect(screen.queryByText("完成独立备份与恢复演练")).not.toBeInTheDocument();
    fireEvent.click(screen.getByText("全项目"));
    fireEvent.click(screen.getByRole("button", { name: "列表" }));
    expect(screen.getByLabelText("任务列表")).toBeInTheDocument();
    fireEvent.change(screen.getByPlaceholderText("标题、领域、负责人…"), { target: { value: "键盘" } });
    expect(screen.getByText("完成 Mac 键盘决策级研究")).toBeInTheDocument();
    expect(screen.queryByText("建立安全同步")).not.toBeInTheDocument();
  });

  it("首页不加载正文，打开详情后才加载正文和事件", async () => {
    const mock = provider();
    render(<App provider={mock} />);
    await enterManagedTasks();
    await screen.findByText("统一个人 AI 规则与能力边界");
    const card = screen.getByRole("button", { name: /统一个人 AI 规则与能力边界/ });
    expect(mock.loadBody).not.toHaveBeenCalled();
    expect(mock.loadEvents).not.toHaveBeenCalled();
    fireEvent.click(card);
    await waitFor(() => expect(mock.loadBody).toHaveBeenCalledWith("tsk_demo_governance.md"));
    expect(await screen.findByText("# 按需正文")).toBeInTheDocument();
    expect(screen.getByText("官方历史 · 实时未知")).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "读取历史" })).toBeInTheDocument();
    expect(mock.loadCodexThreadPage).not.toHaveBeenCalled();
    fireEvent.keyDown(window, { key: "Escape" });
    expect(screen.queryByRole("dialog")).not.toBeInTheDocument();
  });

  it("Codex 历史仅由用户按需读取，并通过游标加载更早轮次", async () => {
    const mock = provider();
    render(<App provider={mock} />);
    await enterManagedTasks();
    fireEvent.click(await screen.findByRole("button", { name: /统一个人 AI 规则与能力边界/ }));
    expect(mock.loadCodexThreadPage).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole("button", { name: "读取历史" }));
    expect(await screen.findByText("Codex 桌面版或编辑器")).toBeInTheDocument();
    expect(screen.getByText("分页请求不加载轮次内容；应用不展示或保存对话正文。该短期接口不能代表桌面版实时运行状态。")).toBeInTheDocument();
    expect(mock.loadCodexThreadPage).toHaveBeenNthCalledWith(1, "019f-demo-running", undefined);
    fireEvent.click(screen.getByRole("button", { name: "加载更早记录" }));
    await waitFor(() => expect(mock.loadCodexThreadPage).toHaveBeenNthCalledWith(2, "019f-demo-running", "older-cursor"));
    expect(screen.getAllByText("已完成").length).toBeGreaterThanOrEqual(2);
  });

  it("关闭详情后丢弃尚未返回的 Codex 历史", async () => {
    const mock = provider();
    let resolveHistory: ((value: Awaited<ReturnType<TaskDataProvider["loadCodexThreadPage"]>>) => void) | undefined;
    vi.mocked(mock.loadCodexThreadPage).mockReturnValue(new Promise((resolve) => { resolveHistory = resolve; }));
    render(<App provider={mock} />);
    await enterManagedTasks();
    fireEvent.click(await screen.findByRole("button", { name: /统一个人 AI 规则与能力边界/ }));
    fireEvent.click(screen.getByRole("button", { name: "读取历史" }));
    fireEvent.click(screen.getByRole("button", { name: "关闭详情" }));
    resolveHistory?.({
      threadId: "019f-demo-running",
      name: "延迟返回",
      sourceKind: "cli",
      sourceLabel: "Codex 命令行",
      reportedStatus: "notLoaded",
      turns: [],
      historyState: "paged",
      observedAt: 201,
    });
    await Promise.resolve();
    expect(screen.queryByText("延迟返回")).not.toBeInTheDocument();
    expect(screen.queryByRole("dialog")).not.toBeInTheDocument();
  });

  it("元数据接口失败时显示降级信息且不读取正文", async () => {
    const mock = provider();
    vi.mocked(mock.loadMetadata).mockRejectedValue(new Error("provider unavailable"));
    render(<App provider={mock} />);
    switchToManagedTasks();
    expect(await screen.findByRole("alert")).toHaveTextContent("正式任务库尚未连接或不可用");
    expect(mock.loadBody).not.toHaveBeenCalled();
    expect(mock.loadEvents).not.toHaveBeenCalled();
  });

  it("单个任务详情失败不会移除其他任务", async () => {
    const mock = provider();
    vi.mocked(mock.loadBody).mockRejectedValue(new Error("bad file"));
    render(<App provider={mock} />);
    await enterManagedTasks();
    const card = await screen.findByRole("button", { name: /统一个人 AI 规则与能力边界/ });
    fireEvent.click(card);
    expect(await screen.findByRole("alert")).toHaveTextContent("该任务详情读取失败");
    expect(screen.getAllByText("完成 Mac 键盘决策级研究").length).toBeGreaterThanOrEqual(1);
  });

  it("标签筛选与保存方案只持久化结构条件，不保存搜索文字", async () => {
    const mock = provider();
    render(<App provider={mock} />);
    await enterManagedTasks();
    fireEvent.change(screen.getByPlaceholderText("标题、领域、负责人…"), { target: { value: "键盘" } });
    fireEvent.change(screen.getByLabelText("标签"), { target: { value: "采购研究" } });
    expect(screen.getByText("完成 Mac 键盘决策级研究")).toBeInTheDocument();
    fireEvent.click(screen.getByRole("button", { name: "保存当前筛选" }));
    fireEvent.change(screen.getByLabelText("筛选方案名称"), { target: { value: "采购任务" } });
    fireEvent.click(screen.getByRole("button", { name: "确认保存" }));
    await waitFor(() => expect(mock.saveTaskFilter).toHaveBeenCalledTimes(1));
    const savedDraft = vi.mocked(mock.saveTaskFilter).mock.calls[0][0];
    expect(savedDraft).toMatchObject({ name: "采购任务", tag: "采购研究", status: "all", view: "board" });
    expect(savedDraft).not.toHaveProperty("query");
    expect(screen.getByLabelText("已保存筛选")).toHaveValue("filter-1");

    fireEvent.change(screen.getByLabelText("标签"), { target: { value: "" } });
    fireEvent.change(screen.getByLabelText("已保存筛选"), { target: { value: "filter-1" } });
    expect(screen.getByLabelText("标签")).toHaveValue("采购研究");
    fireEvent.click(screen.getByRole("button", { name: "删除方案" }));
    await waitFor(() => expect(mock.deleteTaskFilter).toHaveBeenCalledWith("filter-1"));
  });

  it("详情显示正向和反向任务关系，并可跳转到已读取任务", async () => {
    render(<App provider={provider()} />);
    await enterManagedTasks();
    fireEvent.click(await screen.findByRole("button", { name: /统一个人 AI 规则与能力边界/ }));
    const dialog = screen.getByRole("dialog");
    expect(within(dialog).getByText("子任务")).toBeInTheDocument();
    expect(within(dialog).getByText("阻塞当前任务")).toBeInTheDocument();
    fireEvent.click(within(dialog).getByRole("button", { name: /完成 Mac 键盘决策级研究/ }));
    await waitFor(() => expect(within(screen.getByRole("dialog")).getByRole("heading", { level: 2, name: "完成 Mac 键盘决策级研究" })).toBeInTheDocument());
    expect(within(screen.getByRole("dialog")).getByText("父任务")).toBeInTheDocument();
  });

  it("评论预览可取消且不写入，明确确认后只追加并重新读取时间线", async () => {
    const mock = provider();
    vi.mocked(mock.loadEvents)
      .mockResolvedValueOnce(fixtureEvents)
      .mockResolvedValueOnce([...fixtureEvents, {
        id: "evt_synthetic_note",
        taskId: "tsk_demo_governance",
        eventType: "comment_added",
        occurredAt: "2026-08-24T08:00:00Z",
        message: "需要保留的评论",
        author: "本人",
      }]);
    render(<App provider={mock} />);
    await enterManagedTasks();
    fireEvent.click(await screen.findByRole("button", { name: /统一个人 AI 规则与能力边界/ }));
    await screen.findByText("这是一条合成评论。");
    fireEvent.change(screen.getByLabelText("记录内容"), { target: { value: "需要保留的评论" } });
    fireEvent.click(screen.getByRole("button", { name: "生成追加预览" }));
    expect(await screen.findByRole("region", { name: "记录追加预览" })).toHaveTextContent("需要保留的评论");
    fireEvent.click(screen.getByRole("button", { name: "取消" }));
    expect(mock.applyTaskNote).not.toHaveBeenCalled();
    fireEvent.change(screen.getByLabelText("记录内容"), { target: { value: "需要保留的评论" } });
    fireEvent.click(screen.getByRole("button", { name: "生成追加预览" }));
    fireEvent.click(await screen.findByRole("button", { name: "确认追加" }));
    expect(await screen.findByRole("status")).toHaveTextContent("记录已追加并回读");
    expect(mock.applyTaskNote).toHaveBeenCalledWith(expect.objectContaining({
      kind: "comment",
      text: "需要保留的评论",
      expectedTaskHash: "note-task-synthetic-sha256",
      expectedEventHash: "note-event-synthetic-sha256",
    }));
    expect(screen.getByText("需要保留的评论")).toBeInTheDocument();
  });

  it("评论事件并发冲突时不覆盖且保留草稿", async () => {
    const mock = provider();
    vi.mocked(mock.applyTaskNote).mockRejectedValue({ code: "event_conflict", message: "事件历史已变化" });
    render(<App provider={mock} />);
    await enterManagedTasks();
    fireEvent.click(await screen.findByRole("button", { name: /统一个人 AI 规则与能力边界/ }));
    fireEvent.change(screen.getByLabelText("记录内容"), { target: { value: "冲突后仍保留" } });
    fireEvent.click(screen.getByRole("button", { name: "生成追加预览" }));
    fireEvent.click(await screen.findByRole("button", { name: "确认追加" }));
    expect(await screen.findByRole("alert")).toHaveTextContent("记录草稿仍保留");
    expect(screen.getByLabelText("记录内容")).toHaveValue("冲突后仍保留");
  });

  it("取消写入预览不会调用确认写接口", async () => {
    const mock = provider();
    render(<App provider={mock} />);
    await enterManagedTasks();
    fireEvent.click(await screen.findByRole("button", { name: /统一个人 AI 规则与能力边界/ }));
    fireEvent.click(screen.getByRole("button", { name: "快速改优先级" }));
    fireEvent.change(screen.getByLabelText("优先级草稿"), { target: { value: "low" } });
    fireEvent.click(screen.getByRole("button", { name: "生成写入预览" }));
    expect(await screen.findByRole("region", { name: "修改预览" })).toBeInTheDocument();
    fireEvent.click(screen.getByRole("button", { name: "取消" }));
    expect(screen.queryByRole("region", { name: "修改预览" })).not.toBeInTheDocument();
    expect(mock.applyPriorityEdit).not.toHaveBeenCalled();
  });

  it("明确确认后调用带并发令牌的写接口并显示回读成功", async () => {
    const mock = provider();
    render(<App provider={mock} />);
    await enterManagedTasks();
    fireEvent.click(await screen.findByRole("button", { name: /统一个人 AI 规则与能力边界/ }));
    fireEvent.click(screen.getByRole("button", { name: "快速改优先级" }));
    fireEvent.change(screen.getByLabelText("优先级草稿"), { target: { value: "low" } });
    fireEvent.click(screen.getByRole("button", { name: "生成写入预览" }));
    fireEvent.click(await screen.findByRole("button", { name: "确认写入" }));
    expect(await screen.findByRole("status")).toHaveTextContent("已写入并核对");
    expect(mock.applyPriorityEdit).toHaveBeenCalledWith({
      fileToken: "tsk_demo_governance.md",
      newPriority: "low",
      expectedHash: "synthetic-sha256",
      confirmed: true,
    });
  });

  it("并发冲突不覆盖且保留用户草稿", async () => {
    const mock = provider();
    vi.mocked(mock.applyPriorityEdit).mockRejectedValue({ code: "conflict", message: "任务已被其他操作修改，请重新读取后确认" });
    render(<App provider={mock} />);
    await enterManagedTasks();
    fireEvent.click(await screen.findByRole("button", { name: /统一个人 AI 规则与能力边界/ }));
    fireEvent.click(screen.getByRole("button", { name: "快速改优先级" }));
    fireEvent.change(screen.getByLabelText("优先级草稿"), { target: { value: "low" } });
    fireEvent.click(screen.getByRole("button", { name: "生成写入预览" }));
    fireEvent.click(await screen.findByRole("button", { name: "确认写入" }));
    expect(await screen.findByRole("alert")).toHaveTextContent("已重新读取当前任务；你的优先级草稿仍保留");
    expect(screen.getByLabelText("优先级草稿")).toHaveValue("low");
  });

  it("正式状态编辑必须经过字段预览和明确确认", async () => {
    const mock = provider();
    render(<App provider={mock} />);
    await enterManagedTasks();
    fireEvent.click(await screen.findByRole("button", { name: /统一个人 AI 规则与能力边界/ }));
    fireEvent.click(screen.getByRole("button", { name: "编辑其他字段" }));
    fireEvent.change(screen.getByLabelText("正式字段"), { target: { value: "task_status" } });
    fireEvent.change(screen.getByLabelText("字段值"), { target: { value: "done" } });
    fireEvent.click(screen.getByRole("button", { name: "生成写入预览" }));
    expect(await screen.findByRole("region", { name: "字段修改预览" })).toHaveTextContent("已完成");
    fireEvent.click(screen.getByRole("button", { name: "确认写入" }));
    expect(await screen.findByRole("status")).toHaveTextContent("字段已写入并核对");
    expect(mock.applyTaskFieldEdit).toHaveBeenCalledWith({
      fileToken: "tsk_demo_governance.md",
      field: "task_status",
      newValue: "done",
      expectedHash: "field-synthetic-sha256",
      confirmed: true,
    });
  });

  it("取消通用字段预览不会写入", async () => {
    const mock = provider();
    render(<App provider={mock} />);
    await enterManagedTasks();
    fireEvent.click(await screen.findByRole("button", { name: /统一个人 AI 规则与能力边界/ }));
    fireEvent.click(screen.getByRole("button", { name: "编辑其他字段" }));
    fireEvent.change(screen.getByLabelText("正式字段"), { target: { value: "assignee" } });
    fireEvent.change(screen.getByLabelText("字段值"), { target: { value: "新负责人" } });
    fireEvent.click(screen.getByRole("button", { name: "生成写入预览" }));
    expect(await screen.findByRole("region", { name: "字段修改预览" })).toHaveTextContent("新负责人");
    fireEvent.click(screen.getByRole("button", { name: "取消" }));
    expect(screen.queryByRole("region", { name: "字段修改预览" })).not.toBeInTheDocument();
    expect(mock.applyTaskFieldEdit).not.toHaveBeenCalled();
  });

  it("通用字段冲突不覆盖并保留对应草稿", async () => {
    const mock = provider();
    vi.mocked(mock.applyTaskFieldEdit).mockRejectedValue({ code: "conflict", message: "任务已被其他操作修改，请重新读取后确认" });
    render(<App provider={mock} />);
    await enterManagedTasks();
    fireEvent.click(await screen.findByRole("button", { name: /统一个人 AI 规则与能力边界/ }));
    fireEvent.click(screen.getByRole("button", { name: "编辑其他字段" }));
    fireEvent.change(screen.getByLabelText("正式字段"), { target: { value: "assignee" } });
    fireEvent.change(screen.getByLabelText("字段值"), { target: { value: "冲突中的负责人草稿" } });
    fireEvent.click(screen.getByRole("button", { name: "生成写入预览" }));
    fireEvent.click(await screen.findByRole("button", { name: "确认写入" }));
    expect(await screen.findByRole("alert")).toHaveTextContent("字段草稿仍保留");
    expect(screen.getByLabelText("字段值")).toHaveValue("冲突中的负责人草稿");
  });

  it("新建任务取消不写入，确认后才调用创建接口", async () => {
    const mock = provider();
    render(<App provider={mock} />);
    await enterManagedTasks();
    fireEvent.click(screen.getByRole("button", { name: "新建任务" }));
    let dialog = screen.getByRole("dialog", { name: "新建正式任务" });
    fireEvent.change(within(dialog).getByLabelText("新任务标题"), { target: { value: "合成新任务" } });
    fireEvent.click(within(dialog).getByRole("button", { name: "生成新建预览" }));
    const createPreview = await within(dialog).findByRole("region", { name: "新建任务预览" });
    expect(createPreview).toHaveTextContent("general / proposal_only");
    expect(createPreview).toHaveTextContent("task-center-ui / human_confirmed");
    fireEvent.click(within(dialog).getByRole("button", { name: "取消" }));
    expect(mock.applyCreateTask).not.toHaveBeenCalled();

    fireEvent.click(screen.getByRole("button", { name: "新建任务" }));
    dialog = screen.getByRole("dialog", { name: "新建正式任务" });
    fireEvent.change(within(dialog).getByLabelText("新任务标题"), { target: { value: "合成新任务" } });
    fireEvent.click(within(dialog).getByRole("button", { name: "生成新建预览" }));
    fireEvent.click(await within(dialog).findByRole("button", { name: "确认创建" }));
    expect(await within(dialog).findByRole("status")).toHaveTextContent("新任务已创建并核对");
    expect(mock.applyCreateTask).toHaveBeenCalledTimes(1);
  });

  it("新建冲突后保留草稿并要求重新预览", async () => {
    const mock = provider();
    vi.mocked(mock.applyCreateTask).mockRejectedValue({ code: "conflict", message: "同名任务已经存在，请重新生成预览" });
    render(<App provider={mock} />);
    await enterManagedTasks();
    fireEvent.click(screen.getByRole("button", { name: "新建任务" }));
    const dialog = screen.getByRole("dialog", { name: "新建正式任务" });
    fireEvent.change(within(dialog).getByLabelText("新任务标题"), { target: { value: "必须保留的创建草稿" } });
    fireEvent.click(within(dialog).getByRole("button", { name: "生成新建预览" }));
    fireEvent.click(await within(dialog).findByRole("button", { name: "确认创建" }));
    expect(await within(dialog).findByRole("alert")).toHaveTextContent("当前新建草稿仍保留");
    expect(within(dialog).getByLabelText("新任务标题")).toHaveValue("必须保留的创建草稿");
    expect(within(dialog).getByRole("button", { name: "生成新建预览" })).toBeInTheDocument();
  });
});
