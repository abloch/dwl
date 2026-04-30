/*
 * dwl-cli: one-shot dwl state query.
 *
 * Connects to the running dwl Wayland display, binds zdwl_ipc_manager_v2 +
 * ext_foreign_toplevel_list_v1, requests an ipc_output for every wl_output,
 * and prints a single JSON document describing the compositor's current state
 * to stdout. Then exits.
 *
 * Both protocols are push-only and event-based, but dwl emits a complete state
 * burst synchronously on every new bind:
 *   - zdwl_ipc_manager_v2: tags + layout names on bind; per-output state burst
 *     on get_output (dwl_ipc_output_printstatus_to).
 *   - ext_foreign_toplevel_list_v1: toplevel events (and per-handle title /
 *     app_id / identifier / done) on bind for every mapped toplevel.
 * So a query is just three roundtrips and a print -- no event loop.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <json-c/json.h>
#include <wayland-client.h>

#include "dwl-ipc-unstable-v2-client-protocol.h"
#include "ext-foreign-toplevel-list-v1-client-protocol.h"

#define MAX_TAGS    31
#define MAX_LAYOUTS 32

struct TagState {
	uint32_t state;
	uint32_t clients;
	uint32_t focused;
};

struct ClientInfo {
	struct wl_list link;
	char identifier[64];
	uint32_t tags;
	uint32_t focused;
	uint32_t urgent;
	uint32_t floating;
	uint32_t fullscreen;
};

struct Output {
	struct wl_list link;
	struct wl_output *wl_output;
	struct zdwl_ipc_output_v2 *ipc;
	uint32_t reg_name;
	char name[64];

	uint32_t active;
	uint32_t fullscreen;
	uint32_t floating;
	uint32_t layout_index;
	char layout_symbol[64];
	char title[512];
	char appid[256];
	struct TagState tags[MAX_TAGS];
	struct wl_list clients;
};

struct Toplevel {
	struct wl_list link;
	struct ext_foreign_toplevel_handle_v1 *handle;
	char identifier[64];
	char title[512];
	char appid[256];
};

static struct wl_display *display;
static struct zdwl_ipc_manager_v2 *manager;
static struct ext_foreign_toplevel_list_v1 *toplevel_list;
static struct wl_list outputs;
static struct wl_list toplevels;
static uint32_t mgr_tag_count;
static char *mgr_layouts[MAX_LAYOUTS];
static int mgr_layout_count;

/* ---------------- wl_output listener (mostly stubs) ---------------- */

static void
wl_output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
		int32_t pw, int32_t ph, int32_t sub, const char *make,
		const char *model, int32_t transform) { }
static void
wl_output_mode(void *data, struct wl_output *o, uint32_t flags,
		int32_t w, int32_t h, int32_t refresh) { }
static void
wl_output_done(void *data, struct wl_output *o) { }
static void
wl_output_scale(void *data, struct wl_output *o, int32_t factor) { }

static void
wl_output_name(void *data, struct wl_output *o, const char *name)
{
	struct Output *out = data;
	snprintf(out->name, sizeof out->name, "%s", name);
}

static void
wl_output_description(void *data, struct wl_output *o, const char *desc) { }

static const struct wl_output_listener wl_output_listener = {
	.geometry    = wl_output_geometry,
	.mode        = wl_output_mode,
	.done        = wl_output_done,
	.scale       = wl_output_scale,
	.name        = wl_output_name,
	.description = wl_output_description,
};

/* ---------------- zdwl_ipc_manager_v2 listener ---------------- */

static void
mgr_tags(void *data, struct zdwl_ipc_manager_v2 *m, uint32_t amount)
{
	mgr_tag_count = amount > MAX_TAGS ? MAX_TAGS : amount;
}

static void
mgr_layout(void *data, struct zdwl_ipc_manager_v2 *m, const char *name)
{
	if (mgr_layout_count < MAX_LAYOUTS)
		mgr_layouts[mgr_layout_count++] = strdup(name);
}

static const struct zdwl_ipc_manager_v2_listener manager_listener = {
	.tags   = mgr_tags,
	.layout = mgr_layout,
};

/* ---------------- zdwl_ipc_output_v2 listener (state writer) ---------------- */

static void
ipc_toggle_visibility(void *data, struct zdwl_ipc_output_v2 *o) { }

static void
ipc_active(void *data, struct zdwl_ipc_output_v2 *o, uint32_t active)
{
	((struct Output *)data)->active = active;
}

static void
ipc_tag(void *data, struct zdwl_ipc_output_v2 *o, uint32_t tag,
		uint32_t state, uint32_t clients, uint32_t focused)
{
	struct Output *out = data;
	if (tag < MAX_TAGS)
		out->tags[tag] = (struct TagState){state, clients, focused};
}

static void
ipc_layout(void *data, struct zdwl_ipc_output_v2 *o, uint32_t layout)
{
	((struct Output *)data)->layout_index = layout;
}

static void
ipc_title(void *data, struct zdwl_ipc_output_v2 *o, const char *title)
{
	struct Output *out = data;
	snprintf(out->title, sizeof out->title, "%s", title);
}

static void
ipc_appid(void *data, struct zdwl_ipc_output_v2 *o, const char *appid)
{
	struct Output *out = data;
	snprintf(out->appid, sizeof out->appid, "%s", appid);
}

static void
ipc_layout_symbol(void *data, struct zdwl_ipc_output_v2 *o, const char *sym)
{
	struct Output *out = data;
	snprintf(out->layout_symbol, sizeof out->layout_symbol, "%s", sym);
}

static void
ipc_frame(void *data, struct zdwl_ipc_output_v2 *o) { }

static void
ipc_fullscreen(void *data, struct zdwl_ipc_output_v2 *o, uint32_t fs)
{
	((struct Output *)data)->fullscreen = fs;
}

static void
ipc_floating(void *data, struct zdwl_ipc_output_v2 *o, uint32_t fl)
{
	((struct Output *)data)->floating = fl;
}

static void
ipc_client(void *data, struct zdwl_ipc_output_v2 *o, const char *identifier,
		uint32_t tags, uint32_t focused, uint32_t urgent,
		uint32_t floating, uint32_t fullscreen)
{
	struct Output *out = data;
	struct ClientInfo *ci = calloc(1, sizeof *ci);
	if (!ci)
		return;
	snprintf(ci->identifier, sizeof ci->identifier, "%s", identifier);
	ci->tags      = tags;
	ci->focused   = focused;
	ci->urgent    = urgent;
	ci->floating  = floating;
	ci->fullscreen = fullscreen;
	wl_list_insert(out->clients.prev, &ci->link);
}

static const struct zdwl_ipc_output_v2_listener ipc_output_listener = {
	.toggle_visibility = ipc_toggle_visibility,
	.active            = ipc_active,
	.tag               = ipc_tag,
	.layout            = ipc_layout,
	.title             = ipc_title,
	.appid             = ipc_appid,
	.layout_symbol     = ipc_layout_symbol,
	.frame             = ipc_frame,
	.fullscreen        = ipc_fullscreen,
	.floating          = ipc_floating,
	.client            = ipc_client,
};

/* ---------------- ext_foreign_toplevel_handle_v1 listener ---------------- */

static void
tl_closed(void *data, struct ext_foreign_toplevel_handle_v1 *h)
{
	struct Toplevel *t = data;
	wl_list_remove(&t->link);
	ext_foreign_toplevel_handle_v1_destroy(h);
	free(t);
}

static void
tl_done(void *data, struct ext_foreign_toplevel_handle_v1 *h) { }

static void
tl_title(void *data, struct ext_foreign_toplevel_handle_v1 *h, const char *title)
{
	struct Toplevel *t = data;
	snprintf(t->title, sizeof t->title, "%s", title);
}

static void
tl_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *h, const char *app_id)
{
	struct Toplevel *t = data;
	snprintf(t->appid, sizeof t->appid, "%s", app_id);
}

static void
tl_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *h, const char *id)
{
	struct Toplevel *t = data;
	snprintf(t->identifier, sizeof t->identifier, "%s", id);
}

static const struct ext_foreign_toplevel_handle_v1_listener toplevel_listener = {
	.closed     = tl_closed,
	.done       = tl_done,
	.title      = tl_title,
	.app_id     = tl_app_id,
	.identifier = tl_identifier,
};

/* ---------------- ext_foreign_toplevel_list_v1 listener ---------------- */

static void
tl_list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
		struct ext_foreign_toplevel_handle_v1 *h)
{
	struct Toplevel *t = calloc(1, sizeof *t);
	if (!t)
		return;
	t->handle = h;
	wl_list_insert(toplevels.prev, &t->link);
	ext_foreign_toplevel_handle_v1_add_listener(h, &toplevel_listener, t);
}

static void
tl_list_finished(void *data, struct ext_foreign_toplevel_list_v1 *list) { }

static const struct ext_foreign_toplevel_list_v1_listener toplevel_list_listener = {
	.toplevel = tl_list_toplevel,
	.finished = tl_list_finished,
};

/* ---------------- wl_registry listener ---------------- */

static void
registry_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *interface, uint32_t version)
{
	if (strcmp(interface, zdwl_ipc_manager_v2_interface.name) == 0) {
		manager = wl_registry_bind(reg, name,
				&zdwl_ipc_manager_v2_interface, 2);
		zdwl_ipc_manager_v2_add_listener(manager, &manager_listener, NULL);
	} else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
		toplevel_list = wl_registry_bind(reg, name,
				&ext_foreign_toplevel_list_v1_interface, 1);
		ext_foreign_toplevel_list_v1_add_listener(toplevel_list,
				&toplevel_list_listener, NULL);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		uint32_t bind_ver = version >= 4 ? 4 : version;
		struct Output *o = calloc(1, sizeof *o);
		if (!o)
			return;
		o->reg_name = name;
		o->wl_output = wl_registry_bind(reg, name,
				&wl_output_interface, bind_ver);
		snprintf(o->name, sizeof o->name, "wl_output@%u", name);
		wl_list_init(&o->clients);
		if (bind_ver >= 4)
			wl_output_add_listener(o->wl_output, &wl_output_listener, o);
		wl_list_insert(outputs.prev, &o->link);
	}
}

static void
registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) { }

static const struct wl_registry_listener registry_listener = {
	.global        = registry_global,
	.global_remove = registry_global_remove,
};

/* ---------------- JSON build ---------------- */

static struct json_object *
build_root(void)
{
	struct json_object *root, *mgr_obj, *layouts_arr;
	struct json_object *outputs_arr, *toplevels_arr;
	struct Output *o;
	struct Toplevel *t;
	uint32_t i;

	root = json_object_new_object();

	mgr_obj = json_object_new_object();
	json_object_object_add(mgr_obj, "tag_count", json_object_new_int((int32_t)mgr_tag_count));
	layouts_arr = json_object_new_array();
	for (i = 0; i < (uint32_t)mgr_layout_count; i++)
		json_object_array_add(layouts_arr, json_object_new_string(mgr_layouts[i]));
	json_object_object_add(mgr_obj, "layouts", layouts_arr);
	json_object_object_add(root, "manager", mgr_obj);

	outputs_arr = json_object_new_array();
	wl_list_for_each(o, &outputs, link) {
		struct json_object *out_obj = json_object_new_object();
		struct json_object *layout_obj = json_object_new_object();
		struct json_object *tags_arr = json_object_new_array();
		struct json_object *clients_arr;
		struct ClientInfo *ci;

		json_object_object_add(out_obj, "name", json_object_new_string(o->name));
		json_object_object_add(out_obj, "active", json_object_new_int((int32_t)o->active));

		json_object_object_add(layout_obj, "index",
				json_object_new_int((int32_t)o->layout_index));
		json_object_object_add(layout_obj, "symbol",
				json_object_new_string(o->layout_symbol));
		json_object_object_add(layout_obj, "name",
				o->layout_index < (uint32_t)mgr_layout_count
				? json_object_new_string(mgr_layouts[o->layout_index])
				: NULL);
		json_object_object_add(out_obj, "layout", layout_obj);

		json_object_object_add(out_obj, "title", json_object_new_string(o->title));
		json_object_object_add(out_obj, "appid", json_object_new_string(o->appid));
		json_object_object_add(out_obj, "fullscreen",
				json_object_new_int((int32_t)o->fullscreen));
		json_object_object_add(out_obj, "floating",
				json_object_new_int((int32_t)o->floating));

		for (i = 0; i < mgr_tag_count; i++) {
			struct json_object *tag_obj = json_object_new_object();
			json_object_object_add(tag_obj, "index", json_object_new_int((int32_t)i));
			json_object_object_add(tag_obj, "state",
					json_object_new_int((int32_t)o->tags[i].state));
			json_object_object_add(tag_obj, "clients",
					json_object_new_int((int32_t)o->tags[i].clients));
			json_object_object_add(tag_obj, "focused",
					json_object_new_int((int32_t)o->tags[i].focused));
			json_object_array_add(tags_arr, tag_obj);
		}
		json_object_object_add(out_obj, "tags", tags_arr);

		clients_arr = json_object_new_array();
		wl_list_for_each(ci, &o->clients, link) {
			struct json_object *ci_obj = json_object_new_object();
			json_object_object_add(ci_obj, "identifier",
					json_object_new_string(ci->identifier));
			json_object_object_add(ci_obj, "tags",
					json_object_new_int((int32_t)ci->tags));
			json_object_object_add(ci_obj, "focused",
					json_object_new_int((int32_t)ci->focused));
			json_object_object_add(ci_obj, "urgent",
					json_object_new_int((int32_t)ci->urgent));
			json_object_object_add(ci_obj, "floating",
					json_object_new_int((int32_t)ci->floating));
			json_object_object_add(ci_obj, "fullscreen",
					json_object_new_int((int32_t)ci->fullscreen));
			json_object_array_add(clients_arr, ci_obj);
		}
		json_object_object_add(out_obj, "clients", clients_arr);

		json_object_array_add(outputs_arr, out_obj);
	}
	json_object_object_add(root, "outputs", outputs_arr);

	toplevels_arr = json_object_new_array();
	wl_list_for_each(t, &toplevels, link) {
		struct json_object *tl_obj = json_object_new_object();
		json_object_object_add(tl_obj, "identifier",
				json_object_new_string(t->identifier));
		json_object_object_add(tl_obj, "title", json_object_new_string(t->title));
		json_object_object_add(tl_obj, "appid", json_object_new_string(t->appid));
		json_object_array_add(toplevels_arr, tl_obj);
	}
	json_object_object_add(root, "toplevels", toplevels_arr);

	return root;
}

/* ---------------- main ---------------- */

int
main(void)
{
	struct wl_registry *reg;
	struct json_object *root;
	struct Output *o;
	struct Toplevel *t, *tnext;

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "wl_display_connect failed\n");
		return 1;
	}
	wl_list_init(&outputs);
	wl_list_init(&toplevels);

	reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!manager) {
		fprintf(stderr, "compositor lacks zdwl_ipc_manager_v2\n");
		return 1;
	}
	wl_display_roundtrip(display);

	wl_list_for_each(o, &outputs, link) {
		o->ipc = zdwl_ipc_manager_v2_get_output(manager, o->wl_output);
		zdwl_ipc_output_v2_add_listener(o->ipc, &ipc_output_listener, o);
	}
	wl_display_roundtrip(display);

	root = build_root();
	puts(json_object_to_json_string_ext(root,
			JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED));
	json_object_put(root);

	wl_list_for_each_safe(t, tnext, &toplevels, link) {
		wl_list_remove(&t->link);
		ext_foreign_toplevel_handle_v1_destroy(t->handle);
		free(t);
	}
	if (toplevel_list)
		ext_foreign_toplevel_list_v1_destroy(toplevel_list);
	wl_list_for_each(o, &outputs, link) {
		struct ClientInfo *ci, *cinext;
		wl_list_for_each_safe(ci, cinext, &o->clients, link) {
			wl_list_remove(&ci->link);
			free(ci);
		}
		zdwl_ipc_output_v2_release(o->ipc);
	}
	zdwl_ipc_manager_v2_release(manager);
	wl_display_disconnect(display);
	return 0;
}
