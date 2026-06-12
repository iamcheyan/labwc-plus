# Scroll Wheel Support for Window Cycle (Alt+Tab)

## Summary

Add mouse scroll wheel support to the Alt+Tab window switcher. When cycle mode is active (Alt+Tab held), scrolling the mouse wheel cycles through windows — scroll down for next, scroll up for previous.

## Usage

1. Press and hold **Alt+Tab** to enter cycle mode
2. **Scroll wheel down** → cycle to the next window
3. **Scroll wheel up** → cycle to the previous window
4. Release **Alt** to focus the selected window

No configuration needed — this behavior is always active during cycle mode.

## Technical Details

The patch intercepts `WL_POINTER_AXIS_VERTICAL_SCROLL` events in `handle_axis()` when `server.input_mode == LAB_INPUT_STATE_CYCLE`. It maps:

- Negative delta (scroll up) → `LAB_CYCLE_DIR_BACKWARD`
- Positive delta (scroll down) → `LAB_CYCLE_DIR_FORWARD`

These are the same direction constants used by the keyboard handler (`handle_cycle_view_key()` in `keyboard.c`), ensuring consistent behavior between keyboard and mouse navigation.

The event is consumed (not forwarded to clients or processed as mouse bindings) when cycle mode is active.

## Files Changed

- `src/input/cursor.c` — Added scroll wheel interception in `handle_axis()`
