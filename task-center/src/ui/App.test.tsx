import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import { describe, expect, it, vi } from "vitest";
import { fixtureEvents, fixtureProjects, fixtureSources } from "../data/fixtures";
import type { TaskDataProvider } from "../data/provider";
import { App } from "./App";

function provider(): TaskDataProvider {
  return {
    loadMetadata: vi.fn().mockResolvedValue(fixtureSources),
    loadProjectMappings: vi.fn().mockResolvedValue(fixtureProjects),
    loadBody: vi.fn().mockResolvedValue("# 按需正文"),
    loadEvents: vi.fn().mockResolvedValue(fixtureEvents),
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
  };
}

describe("任务中心核心流程", () => {
  it("项目切换、看板、列表、搜索共享同一任务数据", async () => {
    render(<App provider={provider()} />);
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
    await screen.findByText("统一个人 AI 规则与能力边界");
    const card = screen.getByRole("button", { name: /统一个人 AI 规则与能力边界/ });
    expect(mock.loadBody).not.toHaveBeenCalled();
    expect(mock.loadEvents).not.toHaveBeenCalled();
    fireEvent.click(card);
    await waitFor(() => expect(mock.loadBody).toHaveBeenCalledWith("tsk_demo_governance.md"));
    expect(await screen.findByText("# 按需正文")).toBeInTheDocument();
    expect(screen.getByText("外部任务 · 状态未知")).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "打开任务" })).toBeDisabled();
    fireEvent.keyDown(window, { key: "Escape" });
    expect(screen.queryByRole("dialog")).not.toBeInTheDocument();
  });

  it("元数据接口失败时显示降级信息且不读取正文", async () => {
    const mock = provider();
    vi.mocked(mock.loadMetadata).mockRejectedValue(new Error("provider unavailable"));
    render(<App provider={mock} />);
    expect(await screen.findByRole("alert")).toHaveTextContent("任务元数据暂时不可用");
    expect(mock.loadBody).not.toHaveBeenCalled();
    expect(mock.loadEvents).not.toHaveBeenCalled();
  });

  it("单个任务详情失败不会移除其他任务", async () => {
    const mock = provider();
    vi.mocked(mock.loadBody).mockRejectedValue(new Error("bad file"));
    render(<App provider={mock} />);
    const card = await screen.findByRole("button", { name: /统一个人 AI 规则与能力边界/ });
    fireEvent.click(card);
    expect(await screen.findByRole("alert")).toHaveTextContent("该任务详情读取失败");
    expect(screen.getByText("完成 Mac 键盘决策级研究")).toBeInTheDocument();
  });

  it("取消写入预览不会调用确认写接口", async () => {
    const mock = provider();
    render(<App provider={mock} />);
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
