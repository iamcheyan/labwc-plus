# 按显示器隔离的工作区与任务切换器设计

## 目标

把工作区和任务切换器从“全局状态”调整为“显示器本地状态”：

- 在某个显示器上触发 `GoToDesktop`（例如 `Win+1`），只切换该显示器的工作区。
- 每个显示器可以独立停留在工作区 `1`、`2`、`3` 等编号；两个显示器可以同时处于同名工作区，但内容互不影响。
- 任务切换器（`NextWindow`、`PreviousWindow` 及其 immediate 版本）默认只列出焦点所在显示器上的窗口。
- 工作区 OSD 只显示触发操作的显示器，并显示该显示器自己的当前工作区。
- 保留显式的跨显示器操作能力，例如 `output="all"` 或移动窗口到其他输出。

这里的“焦点显示器”优先指当前键盘焦点窗口所在的显示器；没有焦点窗口时退回到鼠标所在显示器。

## 当前上游实现

当前基线是 `upstream/master`（本次梳理时为 `7cc64599`）。上游的工作区模型是全局的：

- `server.workspaces.all` 是一组全局 `struct workspace`。
- `server.workspaces.current` 和 `server.workspaces.last` 只有一份。
- 每个 `workspace` 拥有一个场景树：

  ```text
  server.workspace_tree
    +-- workspace->tree
          +-- view_trees[always-on-top]
          +-- view_trees[normal]
          +-- view_trees[always-on-bottom]
  ```

- 切换工作区时，`src/workspaces.c:workspaces_switch_to()` 禁用旧工作区树、启用新工作区树，因此所有显示器同时切换。
- 新窗口在 `src/xdg.c` 和 `src/xwayland.c` 中绑定到 `server.workspaces.current`。
- `desktop_focus_view()` 在聚焦窗口前调用工作区切换，因此聚焦另一工作区的窗口会改变全局工作区。
- `LAB_VIEW_CRITERIA_CURRENT_WORKSPACE` 在 `src/view.c:view_matches_criteria()` 中直接比较：

  ```c
  view->workspace == server.workspaces.current
  ```

- 工作区 OSD 在 `src/workspaces.c` 中使用同一个 `server.workspaces.current` 为所有输出绘制内容。

## 当前任务切换器实现

任务切换器已经有输出过滤能力，但默认行为仍然是全局：

- `struct cycle_filter` 同时支持工作区过滤、输出过滤和 app_id 过滤。
- `get_outputs_by_filter()` 支持 `all`、`cursor`、`focused` 三种输出范围。
- `init_cycle()` 和 `cycle_immediate()` 会依据输出 bitset 过滤窗口。
- `src/action.c` 为 `NextWindow`、`PreviousWindow` 及 immediate 版本构造过滤器时，输出默认值是 `CYCLE_OUTPUT_ALL`。
- `rc.window_switcher.osd.output_filter` 默认也是 `CYCLE_OUTPUT_ALL`，所以即使窗口列表被过滤，OSD 仍可能显示到所有输出。

因此任务切换器的第一部分改动相对独立：默认输出过滤改为 `focused`，并把 OSD 输出范围同步为焦点输出。

## 推荐的数据模型

不复制整套工作区对象，而是保留一份工作区名称/顺序列表，在 `struct output` 中保存每个输出的当前和上一个工作区：

```c
struct output {
    ...
    struct workspace *current_workspace;
    struct workspace *last_workspace;
};
```

窗口仍然只有一个 `view->workspace` 指针；同名工作区对象可以被多个输出共享。窗口的实际可见性由两个条件共同决定：

```text
view.visible_on_all_workspaces
    或
view.workspace == view.output.current_workspace
```

这个模型的优点：

- 不改变现有 `rc.xml` 工作区名称和编号配置。
- 不需要为每个显示器复制 ext-workspace 对象、菜单和配置解析逻辑。
- 同一窗口仍有明确的主输出；窗口移动到另一个输出时，可继续保留工作区编号。
- 场景树可以继续共享，但不能再通过启停整个工作区树控制可见性。

## 需要修改的核心路径

### 1. 工作区状态

在 `include/output.h` 增加输出本地的 `current_workspace` / `last_workspace`，并在输出创建时初始化为默认工作区。

在 `include/workspaces.h` 暴露以下概念：

- 获取焦点输出；
- 获取某个输出当前工作区；
- 获取当前操作目标输出的工作区；
- 在指定输出上切换工作区；
- 判断窗口在其输出上是否可见。

`workspaces_switch_to()` 保留为兼容包装函数，内部根据焦点窗口或鼠标选择输出；真正的实现使用显式的 `workspaces_switch_to_on_output(output, target, ...)`，避免聚焦另一个输出的窗口时切错显示器。

### 2. 场景树与窗口可见性

工作区树不能再“一次只启用一个”。共享工作区树全部保持启用，具体窗口由 `view_update_visibility()` 根据窗口所属输出的当前工作区启停。

切换某个输出的工作区后，应更新该输出上的窗口，而不是全局隐藏所有工作区。更新后继续调用：

- `desktop_focus_topmost_view()`；
- `desktop_update_top_layer_visibility()`；
- `cursor_update_focus()`；
- 工作区 OSD 更新。

需要特别注意：`view_update_visibility()` 还负责 fullscreen、layer-shell、strut 和延迟 raise 等副作用，批量更新时应避免引入递归焦点变化。

### 3. 新窗口与聚焦窗口

在 `src/xdg.c` 和 `src/xwayland.c` 中，新窗口应绑定到：

```c
workspaces_current_for_output(view->output)
```

而不是全局 `server.workspaces.current`。

`desktop_focus_view()` 在让窗口可见时，应使用该窗口的 `view->output` 调用显式输出版本的工作区切换。否则在显示器 A 上聚焦显示器 B 的窗口时，依赖当前键盘焦点的隐式选择会指向错误输出。

`LAB_VIEW_CRITERIA_CURRENT_WORKSPACE` 应改成按 `view->output` 判断；全局工作区筛选不再能直接比较 `server.workspaces.current`。

### 4. 工作区动作

`GoToDesktop`、`SendToDesktop` 的目标锚点应来自操作目标输出的当前工作区。这样：

- `to="1"` 在显示器 A 上只查找 A 的工作区序号；
- `left`、`right`、`left-occupied`、`right-occupied` 在每个显示器上独立计算；
- `last` 使用该输出自己的 `last_workspace`。

窗口移动到另一个输出的动作仍然应保留现有语义；移动后窗口是否跟随目标输出的当前工作区，需要在实现阶段明确测试。

### 5. 工作区 OSD

`_osd_update()` 绘制 OSD 时使用每个输出自己的当前工作区；触发切换时只在焦点输出显示 OSD。这样不会在另一台显示器上显示错误的工作区编号。

输出热插拔时，需要保证：

- 新输出初始化到默认工作区；
- 输出销毁后，窗口转移到其他输出时重新计算可见性；
- 不遗留指向已销毁输出的本地工作区状态。

### 6. 任务切换器

默认配置建议：

```c
rc.window_switcher.osd.output_filter = CYCLE_OUTPUT_FOCUSED;
```

`src/action.c` 中四个窗口切换动作的默认 `output` 也改为 `CYCLE_OUTPUT_FOCUSED`。显式写入 `output="all"` 或 `output="cursor"` 时仍然按用户配置覆盖默认值。

窗口切换器的工作区过滤要与新的按输出可见性判断一致，否则会出现列表中混入另一显示器当前工作区窗口的情况。

## 焦点、鼠标与键盘触发的边界

Wayland 键盘事件本身没有显示器字段，因此“在哪个显示器按快捷键”只能通过 compositor 状态推断：

1. 有 `server.active_view` 时，使用 `server.active_view->output`；
2. 没有活动窗口时，使用 `output_nearest_to_cursor()`；
3. 任务切换过程中不能用临时预览窗口改变目标输出；
4. OSD 关闭和工作区切换完成后，要重新计算 cursor focus。

这也意味着，用户若只移动鼠标到另一显示器但键盘焦点仍在原显示器，快捷键会优先作用于键盘焦点所在显示器。这符合“当前焦点所在显示器”的要求；如果希望严格按鼠标位置，需要另外增加配置选项。

## 兼容性与风险

- ext-workspace 协议目前由上游以全局 group 方式创建。按输出独立后，协议客户端看到的 active 状态未必能表达每个输出的独立当前工作区；第一阶段应优先保证本地快捷键和 OSD 行为。
- 菜单中的工作区列表目前也引用全局 current。菜单打开时应使用操作目标输出的 current，否则菜单高亮可能与实际快捷键状态不一致。
- `visible_on_all_workspaces` 是现有的全局语义。推荐保留为“对所有输出的当前工作区可见”，不要在第一阶段重定义成只在某一输出的所有工作区可见。
- 跨输出窗口、fullscreen、tiled/maximized 窗口和 layer-shell 覆盖层需要回归测试。
- 输出热插拔、没有活动窗口、窗口被移动到另一输出、窗口切换 OSD 预览期间切换工作区，都是必须覆盖的边界。

## 建议实现顺序

1. 先实现 `struct output` 的本地工作区状态和查询 API。
2. 将新窗口、焦点切换和 `CURRENT_WORKSPACE` 判断改成按输出查询。
3. 将工作区切换和 OSD 改成只更新目标输出。
4. 将任务切换器默认输出过滤改为 focused。
5. 修正菜单、debug 输出和 ext-workspace 相关的兼容行为。
6. 使用双输出 headless/nested 场景做编译和行为测试，再决定是否需要新的 rc.xml 配置项。

## 当前实现进度

第一阶段已经在当前分支实现：

- `struct output` 保存 `current_workspace` 和 `last_workspace`；
- 新窗口按其目标输出选择工作区；
- 窗口可见性按“窗口所属输出的当前工作区”判断；
- 工作区切换只更新目标输出上的窗口；
- 工作区 OSD 只显示在目标输出；
- `left-occupied` / `right-occupied` 只统计目标输出上的窗口；
- 任务切换器默认使用 `CYCLE_OUTPUT_FOCUSED`；
- 菜单和经典任务切换器 OSD 的工作区标识改为按输出查询；
- 输出工作区在配置缩减和窗口移动时会重新计算。

当前已经通过 Meson/Ninja 完整编译。构建使用了临时 Nix 环境补齐 Meson 及图形栈依赖，没有安装到系统全局。

仍需要在真实双显示器会话中验证：

- 两个显示器分别按 `Win+1`、`Win+2` 后是否保持独立；
- 窗口跨显示器移动后的工作区归属和可见性；
- fullscreen、maximized、tiled 窗口；
- 工作区 OSD、任务切换预览和鼠标焦点更新；
- 输出热插拔及没有活动窗口时的快捷键目标。

## 当前结论

这个需求不是只修改一个快捷键过滤条件：任务切换器的输出过滤上游已有基础，但工作区切换的全局场景树和全局 current 指针必须拆开。推荐先按上述共享工作区对象、输出本地 current 指针的模型实现，改动面可控，也最符合现有 labwc 代码结构。
