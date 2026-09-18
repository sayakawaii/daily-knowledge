# 每日一课

每天学一个我原本不知道的知识点，涵盖常识和数理化等学科知识。

## 怎么运作

1. 我说一声「开始」。
2. 导师列出知识分类，我从中选一个（也可以选「随机惊喜」交给导师挑）。
3. 导师按**深入模式**讲这一课：讲清原理，带推导、关键数据和历史脉络，末尾给 3 条记忆锚点和几道自测题。
4. 这一课追加进笔记，并在 `log.md` 里登记一行。

## 笔记在哪

完整笔记是一个 Cursor Canvas，存在本机的工作区目录里：

```
~/.cursor/projects/c-Users-mingheh-daily-knowledge/canvases/daily-knowledge.canvas.tsx
```

Canvas 不进这个仓库（它依赖本地 IDE 渲染）。仓库里的 [`log.md`](./log.md) 是纯文本的学习索引，
用来记录学过什么、避免重复选题，也方便定时任务读取。

## 每日提醒

企业账号未开放 Cursor Automations，所以提醒改用 Windows 计划任务实现。
工作日 10:00 弹一条系统通知，并把 Cursor 打开到这个工作区。

```powershell
# 安装 / 更新
powershell -ExecutionPolicy Bypass -File scripts\install-reminder.ps1

# 手动触发一次
Start-ScheduledTask -TaskName 'Daily Knowledge Reminder'

# 卸载
Unregister-ScheduledTask -TaskName 'Daily Knowledge Reminder' -Confirm:$false
```

`scripts/` 下的两个 `.ps1` 必须存成 **UTF-8 with BOM**——Windows PowerShell 5.1 否则会按 ANSI
读取，中文和续行反引号会一起损坏。

## 偏好

- **深度**：深入。愿意花 15 分钟，要推导、数据和历史脉络，不要只给结论。
- **选题**：偏好冷门但有扎实原理的题目，避免"天空为什么是蓝的"这类已经烂熟的科普。
- **语言**：中文。
