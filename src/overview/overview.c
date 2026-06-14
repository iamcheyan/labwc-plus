// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include "overview.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include "common/font.h"
#include "common/lab-scene-rect.h"
#include "common/list.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "common/string-helpers.h"
#include "config/rcxml.h"
#include "labwc.h"
#include "node.h"
#include "output.h"
#include "scaled-buffer/scaled-font-buffer.h"
#include "scaled-buffer/scaled-icon-buffer.h"
#include "theme.h"
#include "view.h"
#include "workspaces.h"

/* ── tunables ─────────────────────────────────────────── */

#define WS_INNER_PAD    8
#define WS_BORDER_W     2
#define WIN_GAP         4
#define WIN_ICON_SZ     24
#define WIN_LABEL_H     20
#define SCREEN_MARGIN   40
#define GRID_GAP        16
#define DRAG_THRESHOLD  6

/* ── colours ──────────────────────────────────────────── */

#define ov_bg_color       (rc.theme->overview.bg_color)
#define ov_ws_bg          (rc.theme->overview.workspace_bg_color)
#define ov_ws_border_c    (rc.theme->overview.workspace_border_color)
#define ov_ws_hover_c     (rc.theme->overview.workspace_hover_color)
#define ov_ws_current_c   (rc.theme->overview.workspace_active_color)
#define ov_win_bg         (rc.theme->overview.window_bg_color)
#define ov_text_c         (rc.theme->overview.text_color)

/* ── internal types ───────────────────────────────────── */

struct ws_item {
	struct workspace *workspace;
	struct output *output;
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *bg_fill; /* for hover colour changes */
	int x, y, w, h;
	struct wl_list link;
};

struct win_item {
	struct view *view;
	struct wlr_scene_tree *tree;
	struct wlr_scene_buffer *sbuf;
	struct output *output;
	int max_w, max_h;
	struct wl_list link;
};

struct overview_state {
	bool active;
	struct wlr_scene_tree *tree;

	struct wl_list ws_items;  /* struct ws_item */
	struct wl_list win_items; /* struct win_item */

	/* drag tracking */
	struct win_item *pressed_win;
	double press_x, press_y;
	bool dragging;
	struct wlr_scene_buffer *drag_icon;
	struct ws_item *hovered_ws;

	bool pressed;

	int cols, rows;
};

static struct overview_state ov = {0};

/* ── thumbnail rendering (from osd-thumbnail.c) ───────── */

static void
render_node(struct wlr_render_pass *pass,
		struct wlr_scene_node *node, int x, int y)
{
	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		wl_list_for_each(child, &tree->children, link) {
			render_node(pass, child, x + node->x, y + node->y);
		}
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *sbuf = wlr_scene_buffer_from_node(node);
		if (!sbuf->buffer) {
			break;
		}
		struct wlr_texture *texture = NULL;
		struct wlr_client_buffer *cbuf =
			wlr_client_buffer_get(sbuf->buffer);
		if (cbuf) {
			texture = cbuf->texture;
		}
		if (!texture) {
			break;
		}
		wlr_render_pass_add_texture(pass,
			&(struct wlr_render_texture_options){
				.texture = texture,
				.src_box = sbuf->src_box,
				.dst_box = {
					.x = x, .y = y,
					.width = sbuf->dst_width,
					.height = sbuf->dst_height,
				},
				.transform = sbuf->transform,
			});
		break;
	}
	case WLR_SCENE_NODE_RECT:
		break;
	}
}

static struct wlr_buffer *
render_thumb(struct output *output, struct view *view)
{
	if (!view->content_tree) {
		return NULL;
	}
	struct wlr_buffer *buffer = wlr_allocator_create_buffer(
		server.allocator,
		view->current.width, view->current.height,
		&output->wlr_output->swapchain->format);
	if (!buffer) {
		return NULL;
	}
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server.renderer, buffer, NULL);
	render_node(pass, &view->content_tree->node, 0, 0);
	if (!wlr_render_pass_submit(pass)) {
		wlr_buffer_drop(buffer);
		return NULL;
	}
	return buffer;
}

static struct wlr_buffer *
render_bg_thumb(struct output *output)
{
	if (!output || !output->layer_tree[0]) {
		return NULL;
	}
	int out_w, out_h;
	wlr_output_effective_resolution(output->wlr_output, &out_w, &out_h);
	struct wlr_buffer *buffer = wlr_allocator_create_buffer(
		server.allocator,
		out_w, out_h,
		&output->wlr_output->swapchain->format);
	if (!buffer) {
		return NULL;
	}
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server.renderer, buffer, NULL);
	render_node(pass, &output->layer_tree[0]->node, 0, 0);
	if (!wlr_render_pass_submit(pass)) {
		wlr_buffer_drop(buffer);
		return NULL;
	}
	return buffer;
}

/* ── helpers ──────────────────────────────────────────── */

static void
compute_grid(int n, int *cols, int *rows)
{
	if (n <= 0) {
		*cols = *rows = 0;
		return;
	}
	*cols = (int)ceilf(sqrtf((float)n));
	*rows = (n + *cols - 1) / *cols;
}

static int
count_views_on_ws(struct workspace *ws)
{
	int n = 0;
	struct view *v;
	wl_list_for_each(v, &server.views, link) {
		if (v->workspace == ws && v->foreign_toplevel
				&& !string_null_or_empty(v->title)) {
			n++;
		}
	}
	return n;
}

/* ── scene construction ───────────────────────────────── */

static struct ws_item *
create_ws_item(struct wlr_scene_tree *parent, struct workspace *ws,
		struct output *output, int cell_w, int cell_h, int x, int y)
{
	struct ws_item *item = znew(*item);
	item->workspace = ws;
	item->output = output;
	item->w = cell_w;
	item->h = cell_h;
	item->x = x;
	item->y = y;
	item->tree = lab_wlr_scene_tree_create(parent);
	wlr_scene_node_set_position(&item->tree->node, x, y);

	/* border + fill */
	struct lab_scene_rect *rect = lab_scene_rect_create(item->tree,
		&(struct lab_scene_rect_options){
			.border_colors = (float *[1]) { ov_ws_border_c },
			.nr_borders = 1,
			.border_width = WS_BORDER_W,
			.bg_color = ov_ws_bg,
			.width = cell_w,
			.height = cell_h,
		});
	item->bg_fill = rect->fill;
	node_descriptor_create(&item->tree->node,
		LAB_NODE_OVERVIEW_ITEM, NULL, item);

	/* wallpaper inside workspace thumbnail */
	struct wlr_buffer *bg_thumb = render_bg_thumb(output);
	if (bg_thumb) {
		struct wlr_scene_buffer *bg_sbuf =
			lab_wlr_scene_buffer_create(item->tree, bg_thumb);
		wlr_scene_buffer_set_dest_size(bg_sbuf,
			cell_w - 2 * WS_BORDER_W,
			cell_h - 2 * WS_BORDER_W);
		wlr_scene_node_set_position(&bg_sbuf->node, WS_BORDER_W, WS_BORDER_W);
		wlr_buffer_drop(bg_thumb);
	}

	/* current-workspace indicator bar */
	if (ws == server.workspaces.current) {
		struct lab_scene_rect *bar = lab_scene_rect_create(item->tree,
			&(struct lab_scene_rect_options){
				.bg_color = ov_ws_current_c,
				.width = cell_w - 2 * WS_BORDER_W,
				.height = 3,
			});
		wlr_scene_node_set_position(&bar->tree->node, WS_BORDER_W, 0);
	}

	/* workspace name label at bottom */
	int font_h = font_height(&rc.font_osd);
	struct scaled_font_buffer *label = scaled_font_buffer_create(item->tree);
	scaled_font_buffer_update(label, ws->name,
		cell_w - 2 * WS_INNER_PAD,
		&rc.font_osd, ov_text_c, ov_ws_bg);
	wlr_scene_node_set_position(&label->scene_buffer->node,
		(cell_w - label->width) / 2,
		cell_h - WS_INNER_PAD - font_h);

	return item;
}

static struct win_item *
create_win_item(struct wlr_scene_tree *parent, struct view *view,
		struct output *output, int max_w, int max_h)
{
	struct win_item *item = znew(*item);
	item->view = view;
	item->output = output;
	item->max_w = max_w;
	item->max_h = max_h;
	item->tree = lab_wlr_scene_tree_create(parent);
	node_descriptor_create(&item->tree->node,
		LAB_NODE_OVERVIEW_ITEM, NULL, item);

	/* background */
	lab_wlr_scene_rect_create(item->tree, max_w, max_h, ov_win_bg);

	/* thumbnail */
	struct wlr_buffer *thumb = render_thumb(output, view);
	if (thumb) {
		item->sbuf = lab_wlr_scene_buffer_create(item->tree, thumb);
		wlr_buffer_drop(thumb);

		int avail_h = max_h - WIN_LABEL_H - 2;
		int tw = thumb->width;
		int th = thumb->height;
		if (tw > 0 && th > 0 && avail_h > 0) {
			float scale = fminf((float)(max_w - 4) / tw,
				(float)avail_h / th);
			if (scale < 1.0f) {
				int sw = (int)(tw * scale);
				int sh = (int)(th * scale);
				wlr_scene_buffer_set_dest_size(item->sbuf, sw, sh);
				wlr_scene_node_set_position(&item->sbuf->node,
					(max_w - sw) / 2, 2);
			} else {
				wlr_scene_node_set_position(&item->sbuf->node,
					(max_w - tw) / 2, 2);
			}
		}
	}

	/* icon */
	struct scaled_icon_buffer *icon = scaled_icon_buffer_create(
		item->tree, WIN_ICON_SZ, WIN_ICON_SZ);
	scaled_icon_buffer_set_view(icon, view);
	wlr_scene_node_set_position(&icon->scene_buffer->node,
		4, max_h - WIN_LABEL_H);

	/* title */
	struct scaled_font_buffer *title = scaled_font_buffer_create(item->tree);
	const char *name = string_null_or_empty(view->title)
		? view->app_id : view->title;
	int label_w = max_w - WIN_ICON_SZ - 8;
	if (label_w < 10) label_w = 10;
	scaled_font_buffer_update(title, name, label_w,
		&rc.font_menuitem, ov_text_c, ov_win_bg);
	wlr_scene_node_set_position(&title->scene_buffer->node,
		WIN_ICON_SZ + 6,
		max_h - WIN_LABEL_H + (WIN_LABEL_H - title->height) / 2);

	return item;
}

static void
build_overview(void)
{
	int ws_count = wl_list_length(&server.workspaces.all);
	if (ws_count == 0) {
		return;
	}

	/* root tree */
	ov.tree = lab_wlr_scene_tree_create(&server.scene->tree);

	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (!output_is_usable(output)) {
			continue;
		}

		int out_w, out_h;
		wlr_output_effective_resolution(output->wlr_output, &out_w, &out_h);

		int avail_w = out_w - 2 * SCREEN_MARGIN;
		int avail_h = out_h - 2 * SCREEN_MARGIN;

		compute_grid(ws_count, &ov.cols, &ov.rows);
		int cell_w = (avail_w - (ov.cols - 1) * GRID_GAP) / ov.cols;
		int cell_h = (avail_h - (ov.rows - 1) * GRID_GAP) / ov.rows;
		if (cell_w < 120 || cell_h < 100) {
			wlr_log(WLR_ERROR, "overview: output too small");
			continue;
		}

		struct wlr_scene_tree *out_tree = lab_wlr_scene_tree_create(ov.tree);
		wlr_scene_node_set_position(&out_tree->node,
			output->scene_output->x, output->scene_output->y);

		/* background */
		struct wlr_buffer *bg_thumb = render_bg_thumb(output);
		if (bg_thumb) {
			struct wlr_scene_buffer *bg_sbuf =
				lab_wlr_scene_buffer_create(out_tree, bg_thumb);
			wlr_scene_buffer_set_dest_size(bg_sbuf, out_w, out_h);
			wlr_buffer_drop(bg_thumb);
		} else {
			lab_wlr_scene_rect_create(out_tree, out_w, out_h, ov_bg_color);
		}

		/* dimming layer */
		float dim_color[4] = {0.0f, 0.0f, 0.0f, 0.60f};
		lab_wlr_scene_rect_create(out_tree, out_w, out_h, dim_color);

		/* grid origin (centered) */
		int grid_w = ov.cols * cell_w + (ov.cols - 1) * GRID_GAP;
		int grid_h = ov.rows * cell_h + (ov.rows - 1) * GRID_GAP;
		int ox = SCREEN_MARGIN + (avail_w - grid_w) / 2;
		int oy = SCREEN_MARGIN + (avail_h - grid_h) / 2;

		struct workspace *ws;
		int idx = 0;
		wl_list_for_each(ws, &server.workspaces.all, link) {
			int col = idx % ov.cols;
			int row = idx / ov.cols;
			int x = ox + col * (cell_w + GRID_GAP);
			int y = oy + row * (cell_h + GRID_GAP);

			struct ws_item *wi = create_ws_item(out_tree, ws,
				output, cell_w, cell_h, x, y);
			wl_list_append(&ov.ws_items, &wi->link);

			/* windows inside this workspace cell */
			int win_count = count_views_on_ws(ws);
			if (win_count > 0) {
				int area_w = cell_w - 2 * WS_INNER_PAD;
				int area_h = cell_h - 2 * WS_INNER_PAD
					- font_height(&rc.font_osd) - 8;
				int wc = (int)ceilf(sqrtf((float)win_count));
				int wr = (win_count + wc - 1) / wc;
				int ww = (area_w - (wc - 1) * WIN_GAP) / wc;
				int wh = (area_h - (wr - 1) * WIN_GAP) / wr;
				if (ww < 40) ww = 40;
				if (wh < 40) wh = 40;

				int wi_idx = 0;
				struct view *view;
				wl_list_for_each(view, &server.views, link) {
					if (view->workspace != ws
							|| !view->foreign_toplevel
							|| string_null_or_empty(view->title)) {
						continue;
					}
					int c = wi_idx % wc;
					int r = wi_idx / wc;
					int wx = WS_INNER_PAD + c * (ww + WIN_GAP);
					int wy = WS_INNER_PAD + r * (wh + WIN_GAP);

					struct win_item *witem = create_win_item(
						wi->tree, view, output, ww, wh);
					wlr_scene_node_set_position(
						&witem->tree->node, wx, wy);
					wl_list_append(&ov.win_items, &witem->link);
					wi_idx++;
				}
			}
			idx++;
		}
	}

	wlr_scene_node_raise_to_top(&ov.tree->node);
}

/* ── hit testing ──────────────────────────────────────── */

static struct ws_item *
ws_at(double lx, double ly)
{
	struct ws_item *item;
	wl_list_for_each(item, &ov.ws_items, link) {
		int ox = item->output->scene_output->x;
		int oy = item->output->scene_output->y;
		if (lx >= ox + item->x && lx < ox + item->x + item->w
				&& ly >= oy + item->y && ly < oy + item->y + item->h) {
			return item;
		}
	}
	return NULL;
}

static struct win_item *
win_at_node(struct wlr_scene_node *node)
{
	if (!node || !node->data) {
		return NULL;
	}
	struct node_descriptor *desc = node->data;
	if (desc->type != LAB_NODE_OVERVIEW_ITEM || !desc->data) {
		return NULL;
	}
	/* check if it's a win_item by scanning (small list) */
	struct win_item *wi;
	wl_list_for_each(wi, &ov.win_items, link) {
		if (wi == desc->data) {
			return wi;
		}
	}
	return NULL;
}

static void
set_hover(struct ws_item *target)
{
	if (ov.hovered_ws == target) {
		return;
	}
	if (ov.hovered_ws) {
		wlr_scene_rect_set_color(ov.hovered_ws->bg_fill, ov_ws_bg);
	}
	if (target) {
		wlr_scene_rect_set_color(target->bg_fill, ov_ws_hover_c);
	}
	ov.hovered_ws = target;
}

/* ── drag ─────────────────────────────────────────────── */

static void
drag_begin(struct win_item *wi, double x, double y)
{
	ov.dragging = true;
	ov.pressed_win = wi;

	struct output *out = output_nearest_to_cursor();
	if (!out) {
		return;
	}

	struct wlr_buffer *thumb = render_thumb(out, wi->view);
	if (!thumb) {
		return;
	}

	ov.drag_icon = lab_wlr_scene_buffer_create(ov.tree, thumb);
	wlr_buffer_drop(thumb);

	int tw = ov.drag_icon->dst_width > 0 ? ov.drag_icon->dst_width : 100;
	int th = ov.drag_icon->dst_height > 0 ? ov.drag_icon->dst_height : 80;
	wlr_scene_buffer_set_dest_size(ov.drag_icon, tw, th);
	wlr_scene_buffer_set_opacity(ov.drag_icon, 0.65f);
	wlr_scene_node_set_position(&ov.drag_icon->node,
		(int)x - tw / 2, (int)y - th / 2);
	wlr_scene_node_raise_to_top(&ov.drag_icon->node);
}

static void
drag_update(double x, double y)
{
	if (!ov.drag_icon) {
		return;
	}
	int tw = ov.drag_icon->dst_width;
	int th = ov.drag_icon->dst_height;
	wlr_scene_node_set_position(&ov.drag_icon->node,
		(int)x - tw / 2, (int)y - th / 2);

	struct ws_item *target = ws_at(x, y);
	set_hover(target);
}

static void
drag_finish(double x, double y)
{
	if (ov.drag_icon) {
		wlr_scene_node_destroy(&ov.drag_icon->node);
		ov.drag_icon = NULL;
	}
	set_hover(NULL);

	struct ws_item *target = ws_at(x, y);
	wlr_log(WLR_DEBUG, "overview: drag_finish at %.1f, %.1f. target: %s, pressed_win: %p",
		x, y, target ? target->workspace->name : "NULL", (void *)ov.pressed_win);

	if (target && ov.pressed_win) {
		struct view *view = ov.pressed_win->view;
		wlr_log(WLR_DEBUG, "overview: moving view '%s' from '%s' to '%s'",
			view ? view->title : "NULL",
			(view && view->workspace) ? view->workspace->name : "NULL",
			target->workspace->name);
		if (view && target->workspace != view->workspace) {
			view_move_to_workspace(view, target->workspace);

			struct ws_item *ws, *ws_tmp;
			wl_list_for_each_safe(ws, ws_tmp, &ov.ws_items, link) {
				wl_list_remove(&ws->link);
				free(ws);
			}
			struct win_item *wi, *wi_tmp;
			wl_list_for_each_safe(wi, wi_tmp, &ov.win_items, link) {
				wl_list_remove(&wi->link);
				free(wi);
			}
			if (ov.tree) {
				wlr_scene_node_destroy(&ov.tree->node);
				ov.tree = NULL;
			}
			wl_list_init(&ov.ws_items);
			wl_list_init(&ov.win_items);
			build_overview();
			wlr_log(WLR_DEBUG, "overview: rebuilt successfully");
		}
	}
	ov.pressed_win = NULL;
	ov.dragging = false;
}

/* ── public API ───────────────────────────────────────── */

void
overview_show(void)
{
	if (ov.active) {
		return;
	}

	bool has_usable_output = false;
	struct output *output;
	wl_list_for_each(output, &server.outputs, link) {
		if (output_is_usable(output)) {
			has_usable_output = true;
			break;
		}
	}
	if (!has_usable_output) {
		return;
	}

	wl_list_init(&ov.ws_items);
	wl_list_init(&ov.win_items);

	build_overview();
	ov.active = true;
	ov.pressed = false;

	seat_focus_override_begin(&server.seat,
		LAB_INPUT_STATE_OVERVIEW, LAB_CURSOR_DEFAULT);
}

void
overview_hide(void)
{
	if (!ov.active) {
		return;
	}

	if (ov.drag_icon) {
		wlr_scene_node_destroy(&ov.drag_icon->node);
		ov.drag_icon = NULL;
	}
	ov.dragging = false;
	ov.pressed_win = NULL;
	set_hover(NULL);

	struct ws_item *ws, *ws_tmp;
	wl_list_for_each_safe(ws, ws_tmp, &ov.ws_items, link) {
		wl_list_remove(&ws->link);
		free(ws);
	}
	struct win_item *wi, *wi_tmp;
	wl_list_for_each_safe(wi, wi_tmp, &ov.win_items, link) {
		wl_list_remove(&wi->link);
		free(wi);
	}

	if (ov.tree) {
		wlr_scene_node_destroy(&ov.tree->node);
		ov.tree = NULL;
	}

	ov.active = false;
	ov.pressed = false;
	seat_focus_override_end(&server.seat, /*restore_focus*/ true);
	cursor_update_focus();
}

void
overview_on_cursor_press(struct cursor_context *ctx)
{
	assert(ov.active);
	ov.pressed = true;
	ov.pressed_win = NULL;

	if (!ctx || ctx->type != LAB_NODE_OVERVIEW_ITEM || !ctx->node) {
		return;
	}

	struct win_item *wi = win_at_node(ctx->node);
	if (wi) {
		ov.pressed_win = wi;
		ov.press_x = server.seat.cursor->x;
		ov.press_y = server.seat.cursor->y;
	}
}

void
overview_on_cursor_motion(double x, double y)
{
	assert(ov.active);

	if (ov.dragging) {
		drag_update(x, y);
		return;
	}

	/* check if we should start dragging */
	if (ov.pressed_win) {
		double dx = x - ov.press_x;
		double dy = y - ov.press_y;
		if (dx * dx + dy * dy > DRAG_THRESHOLD * DRAG_THRESHOLD) {
			drag_begin(ov.pressed_win, ov.press_x, ov.press_y);
			drag_update(x, y);
			return;
		}
	}

	/* hover highlight */
	struct ws_item *target = ws_at(x, y);
	set_hover(target);
}

void
overview_on_cursor_release(struct cursor_context *ctx)
{
	assert(ov.active);

	if (!ov.pressed) {
		return;
	}
	ov.pressed = false;

	if (ov.dragging) {
		drag_finish(server.seat.cursor->x, server.seat.cursor->y);
		return;
	}

	ov.pressed_win = NULL;

	/* click on workspace → switch */
	if (ctx && ctx->type == LAB_NODE_OVERVIEW_ITEM && ctx->node) {
		struct node_descriptor *desc = ctx->node->data;
		if (desc && desc->type == LAB_NODE_OVERVIEW_ITEM && desc->data) {
			struct ws_item *wi;
			wl_list_for_each(wi, &ov.ws_items, link) {
				if (wi == desc->data) {
					workspaces_switch_to(wi->workspace,
						/*update_focus*/ true);
					overview_hide();
					return;
				}
			}
			/* click on window without drag → switch to its ws */
			struct win_item *witem;
			wl_list_for_each(witem, &ov.win_items, link) {
				if (witem == desc->data) {
					workspaces_switch_to(witem->view->workspace,
						/*update_focus*/ true);
					desktop_focus_view(witem->view, true);
					overview_hide();
					return;
				}
			}
		}
	}

	/* click on empty space → close */
	overview_hide();
}
