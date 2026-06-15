# labwc-plus

labwc-plus is an unofficial downstream fork of
[labwc](https://github.com/labwc/labwc). The project regularly rebases its
changes on top of upstream `master` to retain labwc fixes and improvements
while providing additional features.

labwc-plus is maintained independently and is not supported by the upstream
labwc project. Report issues that occur with this fork to the
[labwc-plus issue tracker](https://github.com/iamcheyan/labwc-plus/issues).

## Additional Features

### Hot Corners

Run one or more standard labwc actions after the pointer remains in a screen
corner for a configured delay.

Hot corners are disabled by default. Add a `hotCorner` section to `rc.xml`:

```xml
<labwc_config>
  <hotCorner>
    <enabled>yes</enabled>
    <delay>300</delay>
    <topLeft>
      <action name="ToggleWorkspaceOverview" />
    </topLeft>
    <topRight>
      <action name="ToggleShowDesktop" />
    </topRight>
  </hotCorner>
</labwc_config>
```

Supported corner names are `topLeft`, `topRight`, `bottomLeft`, and
`bottomRight`. Each corner may contain multiple actions.

### Workspace Overview

Display all workspaces and their windows in an interactive grid. Click a
workspace to switch to it, or drag a window preview to move that window to
another workspace.

Bind the overview to a key in `rc.xml`:

```xml
<labwc_config>
  <keyboard>
    <keybind key="W-Tab">
      <action name="ToggleWorkspaceOverview" />
    </keybind>
  </keyboard>
</labwc_config>
```

The available actions are `ToggleWorkspaceOverview` and
`ShowWorkspaceOverview`. Press `Escape` to close the overview.

The overview colors can be customized in `themerc-override`:

```text
overview.bg.color: #1f1f24eb
overview.workspace.bg.color: #2e2e38
overview.workspace.border.color: #595973
overview.workspace.hover.color: #47598c
overview.workspace.active.color: #4d8cd9
overview.window.bg.color: #383847
overview.text.color: #e6e6eb
```

### Mouse-Wheel Workspace Switching

Hold the configured modifier and scroll to switch between workspaces. This
feature is enabled by default with the `Super` modifier.

Configure it under `mouse` in `rc.xml`:

```xml
<labwc_config>
  <mouse>
    <winScrollWorkspace>yes</winScrollWorkspace>
    <winScrollWorkspaceModifier>W</winScrollWorkspaceModifier>
  </mouse>
</labwc_config>
```

Modifier values use the standard labwc abbreviations: `W` for Super/Logo,
`A` for Alt, `C` for Control, and `S` for Shift. Set the modifier to `none` to
switch workspaces without holding a modifier.

### Mouse-Wheel Window Cycling

While the window switcher is active, scrolling cycles through windows. The
default bindings are:

- Scroll up: previous window
- Scroll down: next window

The behavior uses the `WindowSwitcher` mouse context and can be overridden in
`rc.xml` with normal mouse bindings.

## Building

labwc-plus uses the same build process and dependencies as upstream labwc:

```sh
meson setup build/
meson compile -C build/
```

See [README.md](README.md) for the full upstream build, configuration, and
usage documentation.

## Relationship With Upstream

The repository layout is:

- `upstream/master`: upstream labwc
- `origin/master`: upstream labwc plus the labwc-plus feature commits

Upstream documentation and behavior apply unless this document states
otherwise. Features specific to labwc-plus may change as upstream evolves.

For upstream labwc bugs that can also be reproduced without the labwc-plus
changes, report them to the upstream project. For regressions or behavior
specific to the features above, report them to labwc-plus.

## Updating From Upstream

The maintenance script fetches upstream labwc, rebases the labwc-plus commits,
checks the resulting patch, and builds it:

```sh
scripts/update-upstream.sh
```

After manually testing the result, publish it with:

```sh
git push --force-with-lease origin master
```

To push automatically after a successful build:

```sh
scripts/update-upstream.sh --push
```

If Git finds a conflict, the script stops without pushing and prints the
commands needed to resolve or abort the rebase.
