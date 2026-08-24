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
    fireEvent.click(screen.getByRole("button", { name: "编辑优先级" }));
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
    fireEvent.click(screen.getByRole("button", { name: "编辑优先级" }));
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
    fireEvent.click(screen.getByRole("button", { name: "编辑优先级" }));
    fireEvent.change(screen.getByLabelText("优先级草稿"), { target: { value: "low" } });
    fireEvent.click(screen.getByRole("button", { name: "生成写入预览" }));
    fireEvent.click(await screen.findByRole("button", { name: "确认写入" }));
    expect(await screen.findByRole("alert")).toHaveTextContent("已重新读取当前任务；你的优先级草稿仍保留");
    expect(screen.getByLabelText("优先级草稿")).toHaveValue("low");
  });
});
