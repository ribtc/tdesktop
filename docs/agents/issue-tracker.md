# Issue tracker: GitHub

工程技能的 issue 和 spec 存放在 `ribtc/tdesktop` 的 GitHub Issues。
使用 gh CLI，所有操作显式指定 `--repo ribtc/tdesktop`，避免误写上游仓库。

## 与现有任务队列的关系

GitHub issue 使用仓库 URL 和 issue 编号标识。
AGENTS.md 中的 task、队列和 task slug 仍指 ../ai-tdesktop 的记录。
操作已有任务时遵循 AGENTS.md 的 AI Tasks 流程。
只有用户明确要求时，才将 GitHub issue 转入该队列。
普通对话请求遵循 AGENTS.md 的直接实施规则。

## 常用操作

- 创建：`gh issue create --repo ribtc/tdesktop --title "..." --body-file <文件>`
- 读取：`gh issue view <编号> --repo ribtc/tdesktop --json number,title,body,labels,comments`
- 列表：`gh issue list --repo ribtc/tdesktop --state open`
- 评论：`gh issue comment <编号> --repo ribtc/tdesktop --body-file <文件>`
- 标签：`gh issue edit <编号> --repo ribtc/tdesktop --add-label <标签>`；移除用 --remove-label。
- 关闭：先发表评论说明原因，再执行 `gh issue close <编号> --repo ribtc/tdesktop`。

多行正文通过文件传入，保留实际换行。
技能要求发布到 issue tracker 时创建 GitHub issue；
要求获取 ticket 时读取对应 issue 及评论。

## 父子关系与依赖

优先使用 GitHub 原生 sub-issues 和 issue dependencies。
当前 CLI 不支持时使用 GitHub API。
仓库不支持原生关系时，在子 issue 正文写入 `Part of #<父编号>`，
在父 issue 中维护子 issue 清单，并用 `Blocked by: #<编号>` 记录依赖。
所有阻塞 issue 关闭后，才视为可开始。

## Wayfinding

地图使用标有 wayfinder:map 的 issue，正文包含 Notes、
Decisions-so-far 和 Fog。子 issue 链接到地图，
并使用 wayfinder:research、wayfinder:prototype、
wayfinder:grilling 或 wayfinder:task 标签。

按地图顺序选择未关闭、无未完成依赖且无负责人的子 issue。
开始前分配给当前执行者；完成后发表评论、关闭子 issue，
并在地图 Decisions-so-far 中补充结论和链接。

## Pull requests as a triage surface

**PRs as a request surface: no.**
