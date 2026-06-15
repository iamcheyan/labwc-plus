# labwc 上游 PR 分析

> labwc-plus 开发调查笔记，内容可能随上游和实现变化而过时。
>
> 写于 2026-06-14，基于 Consolatis 对 scroll-wheel-cycle PR 的 review 意见，
> 分析 dev 分支上其他 commit 是否适合提 PR 给上游。

---

## 当前状态

fork 的 `dev` 分支包含以下 commit（基于 upstream master）：

| # | Commit | 说明 |
|---|--------|------|
| 1 | fd224aca | hot-corner: 屏幕角落触发 |
| 2 | d5c00fca | 滚轮切换 workspace |
| 3 | 2de50fe0 | workspace overview |
| 4 | 30476d66 | seat: pointer focus 修复 |
| 5 | d0c2efb3 | 滚轮窗口切换（已提 PR ✅） |

---

## 1. hot-corner (`fd224aca`)

### 改动概要
- 5 个文件，+190 行
- 光标进入屏幕角落 2x2 像素区域后，延迟 300ms 触发用户配置的动作
- 支持四个角落独立配置

### 上游会接受吗：❌ 大概率被拒

### 问题分析

**不是 Openbox 特性**
labwc 的定位是 Openbox 兼容的 Wayland 合成器。Hot corner 是 GNOME/macOS 的东西，
上游不太可能接受这种"非标"功能加到核心代码里。

**应该用现有机制实现**
labwc 有完善的 mousebind/action 系统。更合理的做法是做一个 `HotCorner` action，
让用户自己绑定，而不是硬编码在 `cursor.c` 里。

**2x2 像素硬编码**
没有考虑 HiDPI（高分屏下 2x2 可能太小），也没有做成可配置的触发区域大小。

**代码组织问题**
- `fill_title_layout()` 里混入了 hot corner 的清理代码 — 逻辑位置不对
- `LAB_INPUT_STATE_OVERVIEW` 被放在了 hot-corner commit 里，但实际是给 overview 用的，
  commit 职责不清
- 103 行代码直接放在 `cursor.c`，应该独立成文件

### 如果要提 PR 需要改什么
1. 把 hot corner 做成一个 action（如 `HotCorner`），通过 mousebind 绑定
2. 触发区域大小做成可配置
3. 独立成 `src/input/hot-corner.c`
4. 去掉 `LAB_INPUT_STATE_OVERVIEW`（不属于这个 commit）

---

## 2. workspace switching (`d5c00fca`)

### 改动概要
- 5 个文件，+97 行
- 按住 Super 键 + 滚轮切换 workspace，不显示 OSD
- 新增 `winScrollWorkspace` 和 `winScrollWorkspaceModifier` 配置项

### 上游会接受吗：⚠️ 大概率被要求大改

### 问题分析

**绕过了 mousebind 系统**
直接在 `process_cursor_axis()` 里硬编码了 `rc.win_scroll_workspace` 的逻辑。
上游刚刚要求我们（scroll-wheel-cycle PR）用 mousebind context 代替硬编码，
这个 commit 犯了完全一样的问题。

**代码重复**
`workspaces_switch_to_without_osd()` 是 `workspaces_switch_to()` 的完整复制（50+ 行），
只去掉了 OSD 显示。正确做法是给 `workspaces_switch_to()` 加个 `show_osd` 参数，
或者复用现有逻辑。

**自定义配置格式**
`winScrollWorkspace` + `winScrollWorkspaceModifier` 是独立于 mousebind 体系的
自定义配置，上游不太可能接受这种"另起炉灶"的做法。

### 如果要提 PR 需要改什么
1. 用标准 mousebind 实现，例如：
   ```xml
   <context name="Client">
     <mousebind button="W-Up" action="Scroll">
       <action name="GoToDesktop" to="left" />
     </mousebind>
   </context>
   ```
2. 如果确实需要"不显示 OSD"的行为，给 `GoToDesktop` action 加个 `showOsd="no"` 参数
3. 不要复制 `workspaces_switch_to()`，复用现有逻辑

---

## 3. overview (`2de50fe0`)

### 改动概要
- 10 个文件，+891 行（其中 `overview.c` 758 行）
- 实现 workspace overview 模式：网格显示 workspace 缩略图 + 窗口预览
- 支持点击切换 workspace、拖拽窗口到其他 workspace
- 新增 `ToggleWorkspaceOverview` 和 `ShowWorkspaceOverview` 两个 action

### 上游会接受吗：❌ 几乎肯定被拒

### 问题分析

**代码量太大**
758 行新代码，单文件。这是 labwc 整个项目的 ~2% 代码量，维护负担很大。
上游会非常谨慎，可能要求分多个 PR 逐步推进。

**硬编码 Escape 关闭**
在 `keyboard.c` 里直接检查 `XKB_KEY_Escape`，但 labwc 有 keybind 系统，
应该让用户配置。

**自成体系**
不使用 labwc 现有的任何框架（不走 cycle OSD 的路线，不走 menu 的路线），
完全从零造了一套渲染逻辑。上游会问"为什么不扩展现有的 workspace OSD？"

**和现有功能重叠**
labwc 已经有 workspace OSD（`workspaces_osd`），再做一个 workspace overview
会让功能边界模糊。

**style 问题严重**
3 个 ERROR（trailing statements）、12 个 style 问题，说明代码没有经过人工仔细审查。

### 如果要提 PR 需要改什么
1. 先在 Discussion 里和上游讨论设计方案，确认他们是否接受这个方向
2. 分步骤提 PR：先提基础框架，再提交互逻辑，再提主题支持
3. 修复所有 style 问题（行宽、trailing statements、空行等）
4. 考虑扩展现有 workspace OSD 而不是另起炉灶
5. 用 keybind 系统处理 Escape，不要硬编码

---

## 4. seat fix (`30476d66`)

### 改动概要
- 1 个文件，+3/-1 行
- 在 `seat_focus_override_begin()` 中，只有在没有按下按钮时才清除 pointer focus

### 上游会接受吗：⚠️ 依赖 overview

### 问题分析

**逻辑本身没问题**
3 行改动，逻辑清晰，是个合理的 bug fix。

**但依赖 overview 功能**
这个 fix 是为 overview 功能服务的。没有 overview，这个改动没有实际意义。
单独提 PR 时 reviewer 会问"你遇到了什么 bug？"，没有 overview 的话没法解释。

**可能影响其他 input mode**
改了 `seat_focus_override_begin()` 的通用行为，可能影响 MOVE、RESIZE、MENU
等其他 input mode。需要充分测试。

### 如果要提 PR 需要什么
1. 需要一个可复现的 bug 场景来说明改动的必要性
2. 需要确认不会影响 MOVE、RESIZE、MENU 等模式
3. 最好跟在 overview PR 后面作为一个 fixup commit

---

## 核心教训

### 上游的设计哲学

1. **用户可控 > 硬编码**。能用 mousebind/keybind/action 系统实现的，不要硬编码在代码里
2. **Openbox 兼容**。新功能最好能在 Openbox 里找到对应，或者是 Wayland 特有需求
3. **小步迭代**。一个 PR 只做一件事，代码量尽量小
4. **先讨论再动手**。大功能先在 Discussion/Issue 里和上游对齐方案

### 我们的问题模式

- 在 `cursor.c` 里直接硬编码 scroll/motion 行为，绕过 mousebind 系统
- 复制粘贴代码而不是复用现有逻辑
- 自定义配置格式而不是使用现有框架
- 大量代码未经人工审查就提交

### 建议的 PR 策略

1. **scroll-wheel-cycle** — 已提 PR，已按上游要求改好，等 merge
2. **workspace switching** — 用 mousebind 重写，可以作为第二个 PR
3. **seat fix** — 等 overview 方案确定后再提
4. **hot-corner** — 先在 Discussion 里问上游是否有兴趣，再决定是否实现
5. **overview** — 先开 Discussion 讨论设计方案，可能需要分 3-4 个 PR
