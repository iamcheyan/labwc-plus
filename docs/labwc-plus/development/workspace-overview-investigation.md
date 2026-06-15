# labwc Workspace Overview 调查文档

> labwc-plus 开发调查笔记，内容可能随上游和实现变化而过时。
>
> 调查日期: 2026-06-13
> 目标: 评估在 labwc 中实现 GNOME Shell 风格的 workspace 概览（带缩略图 + 拖拽移动窗口）的可行性

---

## 1. labwc 现有 Workspace 系统架构

### 1.1 核心数据结构

**`struct workspace`** (`include/workspaces.h:13`)

```c
struct workspace {
    struct wl_list link;           // 挂在 server.workspaces.all 链表上
    char *name;                    // workspace 名称
    struct wlr_scene_tree *tree;   // scene tree 根节点
    struct wlr_scene_tree *view_trees[3];  // 三个层级子树
    struct wlr_ext_workspace_handle_v1 *ext_workspace;  // ext-workspace protocol
};
```

**`server.workspaces`** (`include/labwc.h:256`)

```c
struct {
    struct wl_list all;        // 所有 workspace 的双向链表
    struct workspace *current; // 当前活跃的 workspace
    struct workspace *last;    // 上一个 workspace
    struct wlr_ext_workspace_manager_v1 *ext_manager;
    struct wlr_ext_workspace_group_handle_v1 *ext_group;
    struct {
        struct wl_listener commit;
    } on_ext_manager;
} workspaces;
```

**`struct view` 关联** (`include/view.h:171`)

```c
struct workspace *workspace;  // 每个 view 都知道自己属于哪个 workspace
```

### 1.2 Scene Tree 层级结构

渲染顺序从上到下（`server.c:590`）：

```
server.scene
├── output->session_lock_tree          # 锁屏 (swaylock)
├── output->cycle_osd_tree             # Alt-Tab OSD
├── server.cycle_preview_tree          # Alt-Tab 预览窗口
├── server.menu_tree                   # 菜单
├── output->layer_popup_tree           # layer shell 弹窗
├── output->layer_tree[3]              # overlay 层 (rofi)
├── output->layer_tree[2]              # top 层 (waybar)
├── server.unmanaged_tree              # 非托管 X11 窗口
├── server.xdg_popup_tree              # XDG 弹窗
├── server.workspace_tree              # 所有 workspace 的容器
│   ├── workspace1->tree
│   │   ├── view_trees[1]              # always-on-top
│   │   ├── view_trees[0]              # normal
│   │   └── view_trees[2]              # always-on-bottom
│   ├── workspace2->tree
│   │   └── ...
│   └── ...
├── output->layer_tree[1]              # bottom 层
└── output->layer_tree[0]              # background 层 (swaybg)
```

View Layer 枚举 (`include/view.h:86`)：
- `VIEW_LAYER_NORMAL` (0)
- `VIEW_LAYER_ALWAYS_ON_TOP` (1)
- `VIEW_LAYER_ALWAYS_ON_BOTTOM` (2)

### 1.3 Workspace 切换流程

`workspaces_switch_to()` (`workspaces.c:435`)：

1. **Disable 旧 workspace**: `wlr_scene_node_set_enabled(old->tree, false)`
2. **移动 omnipresent 窗口**: 遍历 `visible_on_all_workspaces` 的 view，reparent 到新 workspace
3. **Enable 新 workspace**: `wlr_scene_node_set_enabled(new->tree, true)`
4. **更新 current/last 指针**
5. **焦点处理**: `desktop_focus_topmost_view()`
6. **显示 OSD**: workspace 切换指示器
7. **通知 ext-workspace protocol**

关键：窗口不会被移动，只是通过 scene tree 的 enable/disable 切换可见性。

### 1.4 Workspace 查找

`workspaces_find()` (`workspaces.c:522`) 支持：

| 名称 | 说明 |
|------|------|
| `"current"` | 当前 workspace |
| `"last"` | 上一个 workspace |
| `"left"` / `"right"` | 相邻 workspace |
| `"left-occupied"` / `"right-occupied"` | 相邻有窗口的 workspace |
| 数字（如 `"2"`） | 按索引查找 |
| 任意名称 | 按名字精确匹配 |

### 1.5 View 移动到 Workspace

`view_move_to_workspace()` (`view.c:1587`)：

```c
void view_move_to_workspace(struct view *view, struct workspace *workspace) {
    view->workspace = workspace;
    wlr_scene_node_reparent(&view->scene_tree->node,
        workspace->view_trees[view->layer]);
}
```

通过 `wlr_scene_node_reparent` 将 view 的 scene node 转移到目标 workspace 的 layer tree。

### 1.6 配置与动态重配

- Workspace 在 `workspaces_init()` 中从 `rc.workspace_config.workspaces` 链表读取配置创建
- 支持 `initial_workspace_name` 指定启动时的默认 workspace
- 支持 `workspaces_reconfigure()` 运行时动态增删改 workspace

### 1.7 ext-workspace-v1 Protocol 支持

通过 `wlr_ext_workspace_handle_v1` 实现。支持的操作：

- 查询 workspace 列表（名称、状态：active/urgent/hidden）
- 感知当前活跃 workspace
- 请求切换 workspace（activate/deactivate）
- 创建/删除 workspace
- 将 workspace assign 到 group

**不支持**：查询 workspace 内有哪些窗口、移动窗口到 workspace。

---

## 2. client-list-combined-menu 实现分析

### 2.1 概述

这是 labwc 内置的"所有窗口列表"菜单，完全在 compositor 内部实现，直接遍历内部数据结构。

位置：`src/menu/menu.c:946`

### 2.2 核心函数 `update_client_list_combined_menu()`

```c
static void update_client_list_combined_menu(void)
{
    struct menu *menu = menu_get_by_id("client-list-combined-menu");
    reset_menu(menu);

    struct workspace *workspace;
    struct view *view;

    // 遍历所有 workspace
    wl_list_for_each(workspace, &server.workspaces.all, link) {
        // 1. separator 标记 workspace 名称
        //    当前 workspace 用 >name< 标记
        separator_create(menu,
            workspace == server.workspaces.current ? ">name<" : "name");

        // 2. 遍历所有 view，筛选属于该 workspace 的
        wl_list_for_each(view, &server.views, link) {
            if (view->workspace == workspace) {
                if (!view->foreign_toplevel || string_null_or_empty(view->title))
                    continue;

                // 活跃窗口加 *，最小化加 ()
                if (view == server.active_view) buf_add(&buffer, "*");
                if (view->minimized) buf_add_fmt(&buffer, "(%s)", view->title);
                else buf_add(&buffer, view->title);

                item = item_create(menu, buffer.data, NULL, false);
                item->client_list_view = view;  // 存储 view 指针
                item_add_action(item, "Focus");
                item_add_action(item, "Raise");
            }
        }

        // 3. 每个 workspace 末尾加 "Go there..." 项
        item = item_create(menu, _("Go there..."), NULL, false);
        struct action *action = item_add_action(item, "GoToDesktop");
        action_arg_add_str(action, "to", workspace->name);
    }
    menu_create_scene(menu);
}
```

### 2.3 菜单结构示意

```
┌──────────────────────────────────┐
│  >Desktop 1<                     │  ← separator，当前 workspace
│  [icon] Firefox - YouTube        │    点击 → Focus + Raise
│  [icon] *Terminal                │    * = 活跃窗口
│  [icon] (Minimized App)          │    () = 最小化
│  Go there...                     │    点击 → GoToDesktop
│──────────────────────────────────│
│  Desktop 2                       │  ← separator，非当前 workspace
│  [icon] VS Code                  │
│  Go there...                     │
└──────────────────────────────────┘
```

### 2.4 关键设计细节

**实时更新 (lazy rebuild)**：每次打开菜单时才重建内容（`menu.c:1250`）

```c
if (!strcmp(menu->id, "client-list-combined-menu")) {
    update_client_list_combined_menu();
}
```

**直接存储 view 指针**（`include/menu/menu.h:36`）

```c
struct menuitem {
    struct view *client_list_view;  // 指向真实 view
};
```

**点击时直接操作 view**（`menu.c:1525`）

```c
if (!strcmp(item->parent->id, "client-list-combined-menu")
        && item->client_list_view) {
    if (item->client_list_view->shaded) {
        view_set_shade(item->client_list_view, false);
    }
    actions_run(item->client_list_view, &item->actions, NULL);
}
```

**view 销毁时清理**（`menu.c:1158`）

```c
if (item->client_list_view == view) {
    item->client_list_view = NULL;
    action_list_free(&item->actions);
}
```

**应用图标**（`menu.c:284`）

```c
bool show_app_icon = !strcmp(item->parent->id, "client-list-combined-menu")
            && item->client_list_view;
if (show_app_icon) {
    scaled_icon_buffer_set_view(icon_buffer, item->client_list_view);
}
```

### 2.5 client-send-to-menu（发送窗口到其他 workspace）

`client-menu` 里的子菜单（`menu.c:900`）：

```c
wl_list_for_each(workspace, &server.workspaces.all, link) {
    struct action *action = item_add_action(item, "SendToDesktop");
    action_arg_add_str(action, "to", workspace->name);
}
item_add_action(item, "ToggleOmnipresent");
```

---

## 3. 现有可复用的基础设施

### 3.1 窗口缩略图渲染

**文件**: `src/cycle/osd-thumbnail.c:render_thumb()`

```c
static struct wlr_buffer *render_thumb(struct output *output, struct view *view)
{
    struct wlr_buffer *buffer = wlr_allocator_create_buffer(
        server.allocator,
        view->current.width, view->current.height,
        &output->wlr_output->swapchain->format);
    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
        server.renderer, buffer, NULL);
    render_node(pass, &view->content_tree->node, 0, 0);
    wlr_render_pass_submit(pass);
    return buffer;
}
```

**可以直接复用来渲染 workspace 内每个窗口的缩略图。**

### 3.2 OSD 全屏覆盖层框架

**文件**: `src/cycle/cycle.c`

- 使用 `server.cycle_osd_tree` 作为 scene root
- 每个 output 独立渲染 OSD
- 居中定位逻辑
- show/hide 机制

### 3.3 Scene Graph 点击交互

**文件**: `include/node.h`

```c
// 已有的 node descriptor 类型
LAB_NODE_CYCLE_OSD_ITEM  // cycle OSD 中的可点击项
LAB_NODE_LAYER_SURFACE
LAB_NODE_LAYER_POPUP
LAB_NODE_MENUITEM
LAB_NODE_BUTTON_*
```

可以新增 `LAB_NODE_OVERVIEW_WORKSPACE` 和 `LAB_NODE_OVERVIEW_WINDOW`。

### 3.4 滚动条

**文件**: `src/cycle/osd-scroll.c` (94 行)

支持 OSD 内容的滚动显示和滚动条绘制。

### 3.5 输入状态机

**文件**: `include/labwc.h:18`

```c
enum input_state {
    LAB_INPUT_STATE_PASSTHROUGH = 0,
    LAB_INPUT_STATE_MOVE,
    LAB_INPUT_STATE_RESIZE,
    LAB_INPUT_STATE_MENU,
    LAB_INPUT_STATE_CYCLE,
    // 需要新增: LAB_INPUT_STATE_OVERVIEW
};
```

### 3.6 相关代码行数统计

| 文件 | 行数 | 说明 |
|------|------|------|
| `src/cycle/osd-thumbnail.c` | 326 | 缩略图 OSD（可大量复用） |
| `src/cycle/cycle.c` | 503 | cycle OSD 框架 |
| `src/cycle/osd-classic.c` | 257 | 经典 OSD 样式 |
| `src/cycle/osd-scroll.c` | 94 | 滚动条 |
| `src/menu/menu.c` | 1679 | 菜单系统 |
| `src/workspaces.c` | 637 | workspace 管理 |

---

## 4. Wayland Protocol 现状与局限

### 4.1 已支持的 Protocol

| Protocol | labwc 支持 | 外部可用能力 |
|----------|-----------|-------------|
| `ext-workspace-v1` | ✅ | 列出 workspace、切换 workspace |
| `wlr-foreign-toplevel-management` | ✅ | 列出窗口、聚焦/关闭/最大化窗口 |
| `ext-foreign-toplevel-list` | ✅ | 列出窗口 |

### 4.2 Protocol 的关键缺陷

**没有协议能连接 toplevel 和 workspace**：

- `ext-workspace-v1` 不暴露 workspace 内有哪些窗口
- `wlr-foreign-toplevel-management` 不知道窗口属于哪个 workspace
- 没有协议支持"移动窗口到指定 workspace"

**结论**：纯外部程序无法实现完整的 workspace 概览功能。必须修改 labwc 源码。

### 4.3 ext-workspace-v1 可支持的操作

Protocol 中的 request 类型：

```c
enum wlr_ext_workspace_v1_request_type {
    WLR_EXT_WORKSPACE_V1_REQUEST_CREATE_WORKSPACE,
    WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE,
    WLR_EXT_WORKSPACE_V1_REQUEST_DEACTIVATE,
    WLR_EXT_WORKSPACE_V1_REQUEST_ASSIGN,      // workspace → group，不是 window
    WLR_EXT_WORKSPACE_V1_REQUEST_REMOVE,
};
```

`ASSIGN` 是把 workspace 分配到 group，不是把 window 分配到 workspace。

---

## 5. GNOME Shell 风格 Workspace Overview 实现方案

### 5.1 目标效果

```
┌──────────────────────────────────────────────────┐
│  [dimmed desktop background]                      │
│                                                    │
│   ┌──────────────┐   ┌──────────────┐            │
│   │  Desktop 1   │   │  Desktop 2   │            │
│   │ ┌───┐ ┌────┐ │   │ ┌────┐       │            │
│   │ │ FF│ │Term│ │   │ │Code│       │            │
│   │ └───┘ └────┘ │   │ └────┘       │            │
│   └──────────────┘   └──────────────┘            │
│                                                    │
│   ┌──────────────┐   ┌──────────────┐            │
│   │  Desktop 3   │   │  Desktop 4   │            │
│   │  (empty)     │   │ ┌───┐        │            │
│   │              │   │ │Sp │        │            │
│   └──────────────┘   └──────────────┘            │
│                                                    │
│  [drag window thumbnail between workspaces]       │
└──────────────────────────────────────────────────┘
```

### 5.2 功能点

1. **显示 workspace 网格** — N 个 workspace 缩略图排成网格
2. **每个 workspace 内显示窗口缩略图** — 缩小版的窗口截图
3. **点击 workspace** — 切换到该 workspace
4. **点击窗口** — 切换到该 workspace 并聚焦该窗口
5. **拖拽窗口** — 从一个 workspace 拖到另一个 workspace
6. **关闭 overview** — 点击空白处或按 Esc

### 5.3 需要新增的代码

#### 新文件

| 文件 | 预计行数 | 说明 |
|------|---------|------|
| `src/overview/overview.c` | ~300 | 主入口：显示/隐藏、workspace 网格布局 |
| `src/overview/overview-item.c` | ~200 | 单个 workspace 缩略图容器 |
| `src/overview/overview-drag.c` | ~200 | 拖拽逻辑：开始/移动/释放 |
| `include/overview.h` | ~50 | 接口定义 |

#### 修改文件

| 文件 | 改动量 | 说明 |
|------|--------|------|
| `include/labwc.h` | ~5 行 | +`LAB_INPUT_STATE_OVERVIEW` |
| `src/input/cursor.c` | ~30 行 | overview 状态下的光标处理 |
| `src/input/cursor_context.c` | ~15 行 | overview hit-test |
| `src/workspaces.c` | ~5 行 | 暴露 `workspaces_switch_to` 等 |
| `src/action.c` | ~20 行 | 新增 `ToggleOverview` action |
| `src/config/rcxml.c` | ~10 行 | 配置项 |
| `include/node.h` | ~5 行 | +`LAB_NODE_OVERVIEW_*` |

#### 总计

- **新代码**: ~750 行
- **改动**: ~90 行
- **合计**: ~840 行

### 5.4 各模块详细设计

#### 5.4.1 Workspace 网格布局 (`overview.c`)

```
输入: output 尺寸、workspace 数量
输出: 每个 workspace 缩略图的位置和尺寸

算法:
1. 计算网格行列数 (类似 osd-thumbnail.c 的 get_items_geometry)
2. 每个 cell 的尺寸 = output 尺寸 / 网格数 (留 padding)
3. workspace 缩略图按比例缩小到 cell 内
4. 窗口缩略图按比例缩小到 workspace 缩略图内
```

可复用: `osd-thumbnail.c:204` 的 `get_items_geometry()` 布局算法。

#### 5.4.2 窗口缩略图渲染 (`overview-item.c`)

直接复用 `osd-thumbnail.c:render_thumb()`：

```c
// 对每个 workspace 内的每个 view 调用
struct wlr_buffer *thumb = render_thumb(output, view);
// 缩放到 workspace 缩略图内的合适尺寸
struct wlr_scene_buffer *scene_buf = lab_wlr_scene_buffer_create(tree, thumb);
wlr_scene_buffer_set_dest_size(scene_buf, scaled_width, scaled_height);
```

#### 5.4.3 拖拽逻辑 (`overview-drag.c`)

状态机：

```
IDLE
  │ mousedown on window thumbnail
  ▼
DRAGGING
  │ cursor motion → update drag preview position
  │ 检测光标在哪个 workspace 缩略图上
  │ mouseup
  ▼
DROP
  │ view_move_to_workspace(view, target_workspace)
  │ 更新 overview 显示
  ▼
IDLE
```

需要：
- `LAB_INPUT_STATE_OVERVIEW` 输入状态
- 拖拽时创建半透明缩略图跟随光标
- 光标移动时 hit-test workspace 区域
- 松手时执行 `view_move_to_workspace()`

可复用: labwc 已有的 `LAB_INPUT_STATE_MOVE` 中的光标跟踪逻辑。

#### 5.4.4 输入处理

```c
// cursor.c 中新增
case LAB_INPUT_STATE_OVERVIEW:
    // 如果正在拖拽，更新拖拽预览位置
    // 否则，更新 hover 高亮
    break;
```

#### 5.4.5 Action 集成

```c
// action.c 中新增
case ACTION_TOGGLE_OVERVIEW:
    if (server.overview.active) {
        overview_hide();
    } else {
        overview_show();
    }
    break;
```

配置:

```xml
<keybind key="W-Tab">
  <action name="ToggleOverview"/>
</keybind>
```

### 5.5 难点与风险

| 部分 | 难度 | 风险 |
|------|------|------|
| workspace 网格布局 | ⭐⭐ | 低，有成熟参考 |
| 窗口缩略图渲染 | ⭐ | 低，直接复用 |
| 拖拽状态机 | ⭐⭐⭐ | 中，labwc 没有通用拖拽框架 |
| 多 output 支持 | ⭐⭐ | 中，需要决定 overview 在哪个 output 显示 |
| 动画/过渡效果 | ⭐⭐⭐ | 高，labwc 没有动画系统 |
| scene graph 层级管理 | ⭐⭐ | 中，需要把 overview 放在正确的 z-order |

### 5.6 开发时间估算

| 阶段 | 时间 | 内容 |
|------|------|------|
| 骨架搭建 | 0.5 天 | 新建文件、input state、action、基本显示 |
| workspace 网格 | 1 天 | 布局算法 + workspace 缩略图渲染 |
| 窗口缩略图嵌入 | 0.5 天 | 在 workspace 缩略图内显示窗口 |
| 拖拽基础 | 1 天 | 拖拽状态机 + 移动窗口 |
| 交互完善 | 0.5 天 | 点击切换、高亮、Esc 关闭 |
| **合计（不含动画）** | **3.5 天** | 可用的原型 |
| 动画效果 | 2-3 天 | 平滑过渡、缩放动画 |
| 打磨 | 1-2 天 | 边缘情况、多 output、配置项 |

---

## 6. 替代方案对比

### 方案 A: 内置 Workspace Overview（推荐）

- 直接在 labwc 中实现
- 能访问所有内部数据结构
- 工作量: ~840 行代码，3-5 天

### 方案 B: IPC Socket + 外部程序

- 给 labwc 加 Unix socket IPC
- 外部程序用 Python/Rust/GUI 框架写
- 优点: 前端灵活、可独立更新
- 缺点: 两层代码、延迟、需要 IPC 协议设计
- 工作量: labwc 侧 ~300 行 + 外部程序

### 方案 C: 扩展 Wayland Protocol

- 给 ext-workspace-v1 打补丁，支持 toplevel ↔ workspace 关联
- 最"正确"的方案，但需要上游共识
- 工作量: 大，且依赖社区接受度

### 方案 D: 增强现有菜单

- 在 `client-list-combined-menu` 基础上加功能
- 无法实现缩略图和拖拽（菜单系统不支持）
- 工作量: 小，但效果有限

---

## 7. 关键源码位置速查

| 功能 | 文件 | 行号 |
|------|------|------|
| workspace struct 定义 | `include/workspaces.h` | 13 |
| workspace 初始化 | `src/workspaces.c` | 389 |
| workspace 切换 | `src/workspaces.c` | 435 |
| workspace 查找 | `src/workspaces.c` | 522 |
| workspace 动态重配 | `src/workspaces.c` | 558 |
| view 移动到 workspace | `src/view.c` | 1587 |
| scene tree 层级说明 | `src/server.c` | 590 |
| client-list-combined-menu | `src/menu/menu.c` | 946 |
| client-send-to-menu | `src/menu/menu.c` | 900 |
| 菜单点击处理 | `src/menu/menu.c` | 1525 |
| view 销毁时清理菜单 | `src/menu/menu.c` | 1158 |
| 缩略图渲染 | `src/cycle/osd-thumbnail.c` | 79 |
| OSD 布局算法 | `src/cycle/osd-thumbnail.c` | 204 |
| cycle OSD 框架 | `src/cycle/cycle.c` | - |
| node descriptor | `include/node.h` | 10 |
| input state 枚举 | `include/labwc.h` | 18 |
| ext-workspace protocol 实现 | `subprojects/wlroots/types/wlr_ext_workspace_v1.c` | - |
| foreign-toplevel 实现 | `src/foreign-toplevel/wlr-foreign.c` | - |

---

## 8. 实现记录（已完成）

> 实现日期: 2026-06-13
> 分支: `scroll-wheel-cycle`
> 构建: `builddir-dev` → 安装至 `/opt/labwc-dev/`
> 测试 session: GDM 中的 `labwc (dev)`

### 8.1 实现了两个独立 Feature

#### Feature 1: Workspace Overview

功能：
- 按 `Win+Space` 或右键菜单 "Workspace Overview" 打开
- 全屏覆盖层，显示所有 workspace 的网格
- 每个 workspace 内显示窗口缩略图 + 应用图标 + 标题
- 当前 workspace 有蓝色指示条
- 点击 workspace → 切换并关闭
- 点击窗口 → 切换到该窗口的 workspace 并聚焦
- 按住窗口拖拽到其他 workspace → 移动窗口
- Esc / 点空白处 → 关闭

#### Feature 2: Hot Corner（热区）

功能：
- 鼠标移到屏幕左上角停留指定时间 → 自动打开 Workspace Overview
- 触发延迟可在 `rc.xml` 中配置（毫秒）
- 可启用/禁用

### 8.2 文件改动清单

```
新增文件:
  include/overview.h              +46 行   # workspace overview API
  src/overview/overview.c        +642 行   # overview 核心实现
  src/overview/meson.build         +3 行   # 构建配置

修改文件:
  include/common/node-type.h       +1 行   # LAB_NODE_OVERVIEW_ITEM 枚举值
  include/labwc.h                  +8 行   # LAB_INPUT_STATE_OVERVIEW + hot_corner 状态
  include/config/rcxml.h           +6 行   # hot_corner 配置结构体
  src/action.c                     +9 行   # ToggleWorkspaceOverview action
  src/config/rcxml.c               +7 行   # 解析 hotCorner.enabled / hotCorner.delay
  src/desktop.c                    +4 行   # cursor context 识别 OVERVIEW_ITEM
  src/input/cursor.c              +92 行   # overview 输入路由 + hot corner 检测/定时器
  src/input/keyboard.c            +11 行   # Esc 退出 overview
  src/meson.build                   +1 行   # subdir('overview')
  src/seat.c                        +3 行   # hot corner timer 清理

总计: +830 行（新增 691 行，修改 139 行）
```

### 8.3 rc.xml 配置

```xml
<!-- 快捷键 -->
<keybind key="W-Space">
  <action name="ToggleWorkspaceOverview"/>
</keybind>

<!-- 原来 Win+Space 的功能移到 Alt+Win+Space -->
<keybind key="A-W-Space">
  <action name="ShowMenu" menu="client-list-combined-menu" />
</keybind>

<!-- 热区配置 -->
<hotCorner>
  <enabled>yes</enabled>
  <delay>300</delay>   <!-- 触发延迟，单位毫秒 -->
</hotCorner>
```

### 8.4 menu.xml 配置

```xml
<!-- 在 root-menu 中添加入口 -->
<item label="Workspace Overview">
  <action name="ToggleWorkspaceOverview" />
</item>
```

### 8.5 构建与安装

```bash
# 配置（仅首次）
meson setup builddir-dev --prefix=/opt/labwc-dev -Dxwayland=disabled

# 编译
ninja -C builddir-dev

# 安装
sudo ninja -C builddir-dev install

# 链接到 /usr/local/bin（GDM session 需要）
sudo ln -sf /opt/labwc-dev/bin/labwc /usr/local/bin/labwc-dev
```

### 8.6 架构设计

#### Workspace Overview 状态机

```
PASSTHROUGH
  │ Win+Space / 右键菜单 / hot corner
  ▼
OVERVIEW (LAB_INPUT_STATE_OVERVIEW)
  │
  ├─ 点击 workspace → workspaces_switch_to() → 回到 PASSTHROUGH
  ├─ 点击窗口 → 切换 + 聚焦 → 回到 PASSTHROUGH
  ├─ 按住窗口拖拽
  │   ├─ motion: 超过阈值 → drag_begin()，创建浮动缩略图
  │   ├─ motion: 更新拖拽位置 + 高亮目标 workspace
  │   └─ release: view_move_to_workspace() → 回到 PASSTHROUGH
  └─ Esc / 点空白处 → 回到 PASSTHROUGH
```

#### Hot Corner 检测流程

```
cursor_process_motion()
  │
  ├─ hot_corner_update(seat)
  │   ├─ 获取光标所在 output 的位置
  │   ├─ 检查光标是否在 output 左上角 2x2 像素内
  │   ├─ 进入角落: 启动 timer (delay_ms)
  │   ├─ timer 到期: overview_show()
  │   └─ 离开角落: 取消 timer
  │
  └─ 正常的 motion 处理...
```

#### Scene Tree 结构

```
server.scene
├── ... (existing layers)
├── overview.tree  ← 新增，位于最顶层
│   ├── bg_rect (全屏半透明背景)
│   ├── ws_item[0].tree (Desktop 1)
│   │   ├── bg_rect + border
│   │   ├── indicator_bar (当前 workspace)
│   │   ├── label (workspace 名称)
│   │   ├── win_item[0].tree (Firefox)
│   │   │   ├── bg_rect
│   │   │   ├── thumbnail_buffer (缩略图)
│   │   │   ├── icon_buffer
│   │   │   └── title_font_buffer
│   │   └── win_item[1].tree (Terminal)
│   │       └── ...
│   ├── ws_item[1].tree (Desktop 2)
│   │   └── ...
│   └── drag_icon (拖拽时的浮动缩略图)
└── ...
```

### 8.7 关键 API 使用

| 功能 | API | 来源 |
|------|-----|------|
| 窗口缩略图 | `render_thumb()` (从 osd-thumbnail.c 移植) | 内部渲染 |
| 场景图矩形 | `lab_scene_rect_create()` | `common/lab-scene-rect.h` |
| 场景图 buffer | `lab_wlr_scene_buffer_create()` | `common/scene-helpers.h` |
| 应用图标 | `scaled_icon_buffer_set_view()` | `scaled-buffer/` |
| 文字渲染 | `scaled_font_buffer_update()` | `scaled-buffer/` |
| 节点描述符 | `node_descriptor_create()` | `node.h` |
| Workspace 切换 | `workspaces_switch_to()` | `workspaces.h` |
| 窗口移动 | `view_move_to_workspace()` | `view.h` |
| 焦点管理 | `desktop_focus_view()` | `labwc.h` |
| 输入状态覆盖 | `seat_focus_override_begin/end()` | `labwc.h` |
| 定时器 | `wl_event_loop_add_timer()` | wayland-server |

### 8.8 待改进

- [ ] 拖拽时的半透明效果（需设置 scene buffer opacity）
- [ ] 支持配置热区角落位置（当前硬编码左上角）
- [ ] 支持配置热区触发的 action（当前硬编码 ToggleWorkspaceOverview）
- [ ] 多 output 支持（当前只在光标所在的 output 显示）
- [ ] 动画过渡效果（labwc 无动画系统，实现成本高）
- [ ] 窗口缩略图的实时更新（当前是静态快照）
- [ ] `render_thumb` 提取为共享工具函数（消除与 osd-thumbnail.c 的代码重复）
