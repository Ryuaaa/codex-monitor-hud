# macOS HUD 1.2.0 验收记录

日期：2026-09-04。范围：HUD 1.2.0（build 20），不变更任务中心1.4.0或Windows1.1.0。

## 官方协议依据

核对官方仓库提交 `f3f6922519fa38487c8250c2b8a670a39a2cf9ff`：

- [app-server说明](https://github.com/openai/codex/blob/f3f6922519fa38487c8250c2b8a670a39a2cf9ff/codex-rs/app-server/README.md)：后台省略恢复额度明细，启动/手动完整读取，旧服务端无参数回退；线程卸载不代表完成。
- [额度请求参数](https://github.com/openai/codex/blob/f3f6922519fa38487c8250c2b8a670a39a2cf9ff/codex-rs/app-server-protocol/schema/typescript/v2/GetAccountRateLimitsParams.ts)：`excludeResetCreditDetails`。本工具不启用`supportsLunaReserve`实验能力。
- [额度响应](https://github.com/openai/codex/blob/f3f6922519fa38487c8250c2b8a670a39a2cf9ff/codex-rs/app-server-protocol/schema/typescript/v2/GetAccountRateLimitsResponse.ts)：`ordinaryUsageAllowed`只接受布尔值，null/缺失不推断。

## 已验证

| 检查 | 结果 |
| --- | --- |
| 原生双架构构建 | arm64、x86_64；编译器警告视为错误 |
| 协议固定样例与真实管道模拟 | 73项通过：回退一次、重复响应、空字段、未知字段、错误分类、保留旧值、线程卸载、缓冲上限、进程异常退出、超时、退避与恢复 |
| 现有产品回归 | 用量/费用/周提醒、设置与模块隐藏、缩放、完整主页布局、任务中心入口、更新通道、单实例、历史报告均通过 |
| 本机Codex三种读取 | 启动、后台、手动全部通过；只输出能力及错误分类，不输出账户值或任务名称 |
| 真实旧协议差异 | 本机Codex拒绝新参数时返回`-32600`而非`-32602`；两种都已纳入仅一次的无参数回退测试 |
| 当前普通用量许可字段 | 本机Codex未返回，正确保持不可用；不伪造可用/不可用判断 |
| 隐私与负担 | 无新增轮询器或常驻服务；不订阅/恢复线程；不提取错误原文、认证元数据、accountId或后端任意JSON |

运行固定检查：`sh tests/test-codex-protocol.sh`。已登录本机的自选实测：`sh tests/check-live-codex-protocol.sh`，不会调用本地会话扫描或写统计缓存。

## 发布门禁

- [x] Developer ID强化运行时签名、Apple公证、票据装订和Gatekeeper通过。最终公证提交：`43b5915c-055d-4817-a10a-bef1c61a2a88`，状态Accepted。
- [x] GitHub干净构建通过：[构建记录](https://github.com/Ryuaaa/codex-monitor-hud/actions/runs/33842126743)。发布前核对标签与源代码一致。
- [ ] 发布资产重新下载，核验SHA-256、双架构、版本和公证。
- [ ] 已发布1.1.0旧包到1.2.0的真实替换与重启验证。
- [ ] 本机只保留最新已安装HUD，设置/历史保留，旧应用可恢复。

## 不夸大的边界

- 新参数只在支持的Codex版本上减掉后端明细请求；旧版本自动回退，不声称在旧客户端也减少了该请求。
- 没有新的长期资源基准，不能把“未加轮询、缩减明细”换算为未经测量的CPU降幅。
- 线程列表仍为元数据快照，本机活动仍为趋势判断，不声称获得桌面版全局精确实时任务状态。
- Apple公证和本机验收不能代替所有硬件、Codex旧版本及网络环境覆盖。
