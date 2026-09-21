/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef LABWC_WORKSPACES_H
#define LABWC_WORKSPACES_H

#include <stdbool.h>
#include <wayland-util.h>
#include <wayland-server-core.h>

struct seat;
struct server;
struct output;
struct view;
struct wlr_scene_tree;

struct workspace {
	struct wl_list link; /* struct server.workspaces */

	char *name;
	struct wlr_scene_tree *tree;
	struct wlr_scene_tree *view_trees[3];

	struct wlr_ext_workspace_handle_v1 *ext_workspace;
};

void workspaces_init(void);
/* Switch on the output selected by the current focus/cursor context. */
void workspaces_switch_to(struct workspace *target, bool update_focus);
void workspaces_switch_to_without_osd(struct workspace *target, bool update_focus);
/* Switch only the specified output; used when the target view is elsewhere. */
void workspaces_switch_to_on_output(struct output *output,
	struct workspace *target, bool update_focus);
/* Initialize output-local state after an output becomes usable. */
void workspaces_output_init(struct output *output);
/* Resolve the output and workspace used by keyboard-driven actions. */
struct output *workspaces_get_active_output(void);
struct workspace *workspaces_current(void);
struct workspace *workspaces_current_for_output(struct output *output);
/* Shared workspace trees stay enabled; visibility is output-local. */
bool workspaces_view_is_visible(struct view *view);
void workspaces_destroy(void);
void workspaces_osd_hide(struct seat *seat);
struct workspace *workspaces_find(struct workspace *anchor, const char *name,
	bool wrap);
void workspaces_reconfigure(void);

#endif /* LABWC_WORKSPACES_H */
