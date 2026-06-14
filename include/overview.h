/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_OVERVIEW_H
#define LABWC_OVERVIEW_H

#include <stdbool.h>

struct cursor_context;

/**
 * overview_show() - Display the workspace overview overlay.
 *
 * Shows a grid of workspace thumbnails with window previews.
 * Windows can be dragged between workspaces.
 * Workspaces can be clicked to switch.
 */
void overview_show(void);

/**
 * overview_hide() - Hide the workspace overview overlay.
 *
 * Cleans up all scene nodes and resets state.
 */
void overview_hide(void);

/**
 * overview_on_cursor_press() - Handle mouse button press in overview mode.
 */
void overview_on_cursor_press(struct cursor_context *ctx);

/**
 * overview_on_cursor_motion() - Handle cursor motion in overview mode.
 *
 * If dragging a window, updates the drag preview position and
 * highlights the workspace under the cursor.
 */
void overview_on_cursor_motion(double x, double y);

/**
 * overview_on_cursor_release() - Handle mouse button release in overview mode.
 *
 * If a window was being dragged over a workspace, moves the window
 * to that workspace. Otherwise, handles click-to-switch.
 */
void overview_on_cursor_release(struct cursor_context *ctx);

#endif /* LABWC_OVERVIEW_H */
