# labwc Application Launcher 调查与设计方案

> labwc-plus 开发调查笔记，内容可能随上游和实现变化而过时。
>
> 调查日期: 2026-06-13
> 目标: 评估在 labwc 桌面环境中实现 Pop!_OS 风格应用启动器（支持图标网格、拼音/英文检索、右键 Pin 钉选应用）的技术路径与可行性。

---

## 1. 需求概述

* **图标网格布局**：横向流式排列（Grid Flow），每个单元格上方为应用图标，下方为应用名称文本。
* **搜索过滤**：顶部自带输入框，随输入实时过滤下方的应用网格。
* **右键 Pin（钉选）**：右键点击应用图标时弹出菜单，可选择 "Pin to front" / "Unpin" 将其固定在最前列。
* **交互体验**：唤起速度快，全屏或半透明悬浮窗覆盖，按 Esc 或点击外部空白处自动安全退出。

---

## 2. 方案对比与评估

为了实现此功能，有两个主要的技术路线：

### 方案 A: 集成在 labwc 内部（Compositor 内置）

直接在 compositor 内部扩展 `overview.c` 或新建 `launcher.c`，利用 wlroots scene graph 进行场景绘制和输入捕获。

* **优势**：
  * **极致性能与零延迟**：与 compositor 同进程，无 IPC 通信开销。
  * **按键与焦点管理完美**：无需依赖外部协议即可获得绝对的键盘/鼠标独占抓取权。
* **劣势**：
  * **高级 UI 控件缺失**：wlroots 场景图极为底层。实现一个带光标闪烁、输入法支持（IME）、退格删除、中文拼音检索的输入框需要编写大量的底层字符渲染和状态管理代码。
  * **维护成本极高**：GUI 界面与窗口管理器内核强耦合，若启动器逻辑发生崩溃，会导致整个显示服务（Xwayland / Wayland 会话）中断。

### 方案 B: 单独开发启动器程序（基于 GTK3 / gtk-layer-shell）—— 推荐 ⭐⭐⭐

开发一个独立的轻量级 Wayland 客户端程序，利用 `gtk-layer-shell` 协议将自己渲染在 `overlay` 层级。

* **优势**：
  * **高级控件开箱即用**：GTK3 提供完善的 `GtkFlowBox`（网格流式布局）、`GtkSearchEntry`（带搜索事件的输入框）、`GtkScrolledWindow`（滚动容器）等，极大降低开发复杂度。
  * **现代化视觉外观**：可以通过标准 CSS 轻松配置阴影、圆角、半透明毛玻璃等特效。
  * **健壮与解耦**：客户端程序与 compositor 完全隔离，单体崩溃不影响桌面环境本身。
  * **数据持久化简单**：可以直接使用 glib / json-c 读写 `~/.config/` 目录下的配置文件，来保存用户的 Pin 状态。
* **劣势**：
  * **冷启动延迟**：作为独立客户端启动需要耗费数毫秒时间加载 GTK 库，不过可通过预读或守护进程常驻来消除延迟。

---

## 3. 推荐架构设计（方案 B）

### 3.1 核心技术栈
* **开发语言**：C (与项目保持一致，或采用 Rust)
* **GUI 框架**：GTK 3
* **Wayland 扩展支持**：`gtk-layer-shell` (提供层级置顶、全屏覆盖和键盘交互绑定)

### 3.2 业务逻辑模块

#### 3.2.1 应用加载器 (.desktop Parser)
* 使用 `GAppInfo` (GIO 库内置) 或扫描 `/usr/share/applications` 及 `~/.local/share/applications` 下的 `.desktop` 文件。
* 过滤掉含有 `NoDisplay=true` 的应用。
* 提取关键信息：
  * `Name`: 用于界面展示。
  * `Exec`: 用于启动应用程序。
  * `Icon`: 传递给 GTK 图标主题引擎加载。

#### 3.2.2 钉选数据管理 (Pin List Data Manager)
* 配置文件位置：`~/.config/labwc-launcher/pinned.json`
* 结构：保存已钉选应用的 `.desktop` 文件名数组。
* 排序逻辑：
  1. 优先从配置文件中读取 Pinned 列表，将其放置在最前列；
  2. 随后按照字母排序展示其余非钉选应用。

#### 3.2.3 交互实现细节
* **搜索实现**：监听 `GtkSearchEntry` 的 `search-changed` 信号，调用 `gtk_flow_box_set_filter_func()` 动态重绘网格项。
* **右键菜单**：对单元格部件绑定 `button-press-event`，若是右键（Button 3）则弹出 GtkMenu，选项包含 "Pin to Top" 和 "Unpin"。点击后写入配置文件并刷新布局。
* **退出机制**：
  * 窗口创建时，调用 `gtk_layer_set_keyboard_interactivity(window, TRUE)` 抢占焦点。
  * 监听窗口的 `focus-out-event` (失去焦点) 以及 `key-press-event` (按键 Esc)，触发 `gtk_main_quit()` 退出销毁。
