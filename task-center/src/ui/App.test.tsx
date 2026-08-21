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
});
