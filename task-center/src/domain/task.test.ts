import { describe, expect, it } from "vitest";
import { deriveAttention, normalizeStatus, parseTask, runtimeDisplayState } from "./task";
import { runtimeFixtures } from "../data/fixtures";

const source = (body: string) => ({ fileToken: "fixture.md", frontmatter: body });

describe("正式任务契约", () => {
  it("映射全部正式状态并保留未知值", () => {
    expect(normalizeStatus("doing").status).toBe("doing");
    expect(normalizeStatus("in_review")).toEqual({ status: "unknown", raw: "in_review" });
  });

  it("默认拒绝非 general、explicit_only 和 forbidden", () => {
    for (const [privacy, access] of [["private", "proposal_only"], ["general", "explicit_only"], ["general", "forbidden"]]) {
      const result = parseTask(source(`task_id: tsk_1\ntitle: 私密\ntask_status: todo\nprivacy: ${privacy}\ncodex_access: ${access}`));
      expect(result.issue?.code).toBe("restricted");
    }
  });

  it("坏 YAML、缺失编号和坏日期只隔离单文件", () => {
    expect(parseTask(source("x: [")).issue?.code).toBe("bad_yaml");
    expect(parseTask(source("title: no id\nprivacy: general\ncodex_access: proposal_only")).issue?.code).toBe("missing_id");
    expect(parseTask(source("task_id: x\nprivacy: general\ncodex_access: proposal_only\ndeadline: definitely-not-a-date")).issue?.code).toBe("bad_date");
    expect(parseTask({ fileToken: "broken.md", frontmatter: "", error: "internal" }).issue?.code).toBe("io_error");
  });

  it("在截止日边界派生可解释提示", () => {
    const result = parseTask(source("task_id: x\ntitle: 测试\ntask_status: doing\nprivacy: general\ncodex_access: proposal_only\ndeadline: 2026-08-25\nworkflow_status: 等待确认"));
    expect(deriveAttention(result.task!, new Date("2026-08-23T00:00:00+08:00"))).toEqual(["即将到期", "需要关注"]);
    expect(deriveAttention(result.task!, new Date("2026-08-26T00:00:00+08:00"))).toContain("已逾期");
  });

  it("读取标签、父任务和阻塞关系，并由阻塞关系派生提示", () => {
    const result = parseTask(source([
      "task_id: tsk_child",
      "title: 子任务",
      "task_status: doing",
      "privacy: general",
      "codex_access: proposal_only",
      "tags: [\"接口\", \"P1\"]",
      "parent_id: tsk_parent",
      "blocked_by_ids: [\"tsk_blocker\"]",
      "related_ids: [\"tsk_related\"]",
    ].join("\n")));
    expect(result.task).toMatchObject({
      tags: ["接口", "P1"],
      parentId: "tsk_parent",
      blockedByIds: ["tsk_blocker"],
      relatedIds: ["tsk_related"],
    });
    expect(deriveAttention(result.task!)).toContain("被阻塞");
  });
});

describe("Codex 运行状态降级", () => {
  it("覆盖四个固定样例", () => {
    expect(runtimeFixtures.map(runtimeDisplayState)).toEqual([
      "proven_active",
      "known_not_loaded",
      "external_unknown",
      "provider_unavailable",
    ]);
  });

  it("过期证据不能冒充实时状态", () => {
    expect(runtimeDisplayState({ ...runtimeFixtures[0], stale: true })).toBe("stale");
  });
});
