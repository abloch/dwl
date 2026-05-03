/*
 * dwl-cli: query and control a running dwl compositor.
 *
 * Usage:
 *   dwl-cli [--output <name>] status               print JSON state (default)
 *   dwl-cli [--output <name>] focus <id>           focus client by identifier
 *   dwl-cli [--output <name>] view <mask>          switch monitor view to tagmask
 *   dwl-cli [--output <name>] view-toggle <mask>   swap to alt tagset with mask
 *   dwl-cli [--output <name>] client-tags set <id> <tags>    replace tags
 *   dwl-cli [--output <name>] client-tags add <id> <tags>    add tags, keep others
 *   dwl-cli [--output <name>] client-tags toggle <id> <tags> toggle tags
 *   dwl-cli [--output <name>] client-tags remove <id> <tags> remove tags, keep others
 *   dwl-cli [--output <name>] urgent <id> <0|1>    set/unset urgency
 *   dwl-cli [--output <name>] layout <index>       switch layout
 *
 * Tag mask syntax: N, t<N> (1-indexed), comma-separated list (e.g. 1,3,5), or 0x hex (raw bitmask).
 *
 * Protocol notes:
 *   State is pushed synchronously on bind — three roundtrips suffice for
 *   status. Action mode skips the third roundtrip (no need for client list).
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

	/* previous state for watch diff */
	int watch_initialized;
	uint32_t prev_active;
	uint32_t prev_fullscreen;
	uint32_t prev_floating;
	uint32_t prev_layout_index;
	char prev_layout_symbol[64];
	char prev_title[512];
	char prev_appid[256];
	struct TagState prev_tags[MAX_TAGS];
	struct wl_list prev_clients;
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
static int is_watch;

/* ---------------- tag mask parsing ---------------- */

/*
 * Accept 0x hex (raw bitmask), t<N>, plain decimal N (1-indexed tag number),
 * or a comma-separated list of 1-indexed tag numbers (e.g. "1,3,5").
 * Returns 0 on parse error.
 */
static uint32_t
parse_tagmask(const char *s)
{
	char *end;
	long n;
	unsigned long v;
	const char *num;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		v = strtoul(s, &end, 16);
		if (*end != '\0')
			return 0;
		return (uint32_t)v;
	}
	/* comma-separated list: "1,3,5" → bitmask */
	if (strchr(s, ',')) {
		char buf[256];
		char *tok;
		uint32_t mask = 0;
		snprintf(buf, sizeof buf, "%s", s);
		for (tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
			n = strtol(tok, &end, 10);
			if (*end != '\0' || n < 1 || n > MAX_TAGS)
				return 0;
			mask |= (1u << (n - 1));
		}
		return mask;
	}
	/* t<N> or plain decimal: 1-indexed tag number → bitmask */
	num = (s[0] == 't' && s[1] != '\0') ? s + 1 : s;
	n = strtol(num, &end, 10);
	if (*end != '\0' || n < 1 || n > MAX_TAGS)
		return 0;
	return (uint32_t)(1u << (n - 1));
}

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

/* forward declarations for diff helpers defined in the JSON build section */
static struct json_object *build_output_json(struct Output *o);
static void snapshot_output(struct Output *out);
static void emit_output_diff(struct Output *out);

static void
ipc_frame(void *data, struct zdwl_ipc_output_v2 *o)
{
	struct Output *out = data;
	struct ClientInfo *ci, *tmp;

	if (is_watch) {
		if (!out->watch_initialized) {
			out->watch_initialized = 1;
			snapshot_output(out);
		} else {
			emit_output_diff(out);
			snapshot_output(out);
		}
	}
	wl_list_for_each_safe(ci, tmp, &out->clients, link) {
		wl_list_remove(&ci->link);
		free(ci);
	}
}

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
				&zdwl_ipc_manager_v2_interface, 4);
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
		wl_list_init(&o->prev_clients);
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
tags_to_array(uint32_t mask)
{
	struct json_object *arr = json_object_new_array();
	for (int t = 0; t < MAX_TAGS; t++)
		if (mask & (1u << t))
			json_object_array_add(arr, json_object_new_int(t + 1));
	return arr;
}

/* Find ClientInfo for identifier across all outputs. */
static struct ClientInfo *
find_client_info(const char *identifier, struct Output **out_output)
{
	struct Output *o;
	struct ClientInfo *ci;
	wl_list_for_each(o, &outputs, link) {
		wl_list_for_each(ci, &o->clients, link) {
			if (strcmp(ci->identifier, identifier) == 0) {
				if (out_output)
					*out_output = o;
				return ci;
			}
		}
	}
	return NULL;
}

static struct ClientInfo *
find_client_in_list(struct wl_list *list, const char *id)
{
	struct ClientInfo *ci;
	wl_list_for_each(ci, list, link)
		if (strcmp(ci->identifier, id) == 0)
			return ci;
	return NULL;
}

static void
snapshot_output(struct Output *out)
{
	struct ClientInfo *ci, *tmp, *copy;

	out->prev_active       = out->active;
	out->prev_fullscreen   = out->fullscreen;
	out->prev_floating     = out->floating;
	out->prev_layout_index = out->layout_index;
	snprintf(out->prev_layout_symbol, sizeof out->prev_layout_symbol, "%s", out->layout_symbol);
	snprintf(out->prev_title,  sizeof out->prev_title,  "%s", out->title);
	snprintf(out->prev_appid,  sizeof out->prev_appid,  "%s", out->appid);
	memcpy(out->prev_tags, out->tags, sizeof out->tags);

	wl_list_for_each_safe(ci, tmp, &out->prev_clients, link) {
		wl_list_remove(&ci->link);
		free(ci);
	}
	wl_list_for_each(ci, &out->clients, link) {
		copy = malloc(sizeof *copy);
		if (!copy)
			continue;
		*copy = *ci;
		wl_list_insert(out->prev_clients.prev, &copy->link);
	}
}

static void
emit_output_diff(struct Output *out)
{
	struct json_object *diff, *changed_tags, *added, *removed, *changed_clients;
	struct ClientInfo *ci, *prev_ci;
	uint32_t i;
	int any = 0;

	diff = json_object_new_object();
	json_object_object_add(diff, "output", json_object_new_string(out->name));

#define DIFF_UINT(field, key) \
	if (out->field != out->prev_##field) { \
		json_object_object_add(diff, (key), json_object_new_int((int32_t)out->field)); \
		any = 1; \
	}
	DIFF_UINT(active,     "active")
	DIFF_UINT(fullscreen, "fullscreen")
	DIFF_UINT(floating,   "floating")
#undef DIFF_UINT

	if (out->layout_index != out->prev_layout_index ||
	    strcmp(out->layout_symbol, out->prev_layout_symbol) != 0) {
		struct json_object *lo = json_object_new_object();
		json_object_object_add(lo, "index",  json_object_new_int((int32_t)out->layout_index));
		json_object_object_add(lo, "symbol", json_object_new_string(out->layout_symbol));
		json_object_object_add(lo, "name",
				out->layout_index < (uint32_t)mgr_layout_count
				? json_object_new_string(mgr_layouts[out->layout_index]) : NULL);
		json_object_object_add(diff, "layout", lo);
		any = 1;
	}
	if (strcmp(out->title, out->prev_title) != 0) {
		json_object_object_add(diff, "title", json_object_new_string(out->title));
		any = 1;
	}
	if (strcmp(out->appid, out->prev_appid) != 0) {
		json_object_object_add(diff, "appid", json_object_new_string(out->appid));
		any = 1;
	}

	changed_tags = json_object_new_array();
	for (i = 0; i < mgr_tag_count; i++) {
		struct TagState *cur = &out->tags[i];
		struct TagState *prv = &out->prev_tags[i];
		if (cur->state != prv->state || cur->clients != prv->clients ||
		    cur->focused != prv->focused) {
			struct json_object *t = json_object_new_object();
			json_object_object_add(t, "index",   json_object_new_int((int32_t)(i + 1)));
			json_object_object_add(t, "state",   json_object_new_int((int32_t)cur->state));
			json_object_object_add(t, "clients", json_object_new_int((int32_t)cur->clients));
			json_object_object_add(t, "focused", json_object_new_int((int32_t)cur->focused));
			json_object_array_add(changed_tags, t);
			any = 1;
		}
	}
	if (json_object_array_length(changed_tags) > 0)
		json_object_object_add(diff, "tags", changed_tags);
	else
		json_object_put(changed_tags);

	added           = json_object_new_array();
	removed         = json_object_new_array();
	changed_clients = json_object_new_array();

	wl_list_for_each(ci, &out->clients, link) {
		prev_ci = find_client_in_list(&out->prev_clients, ci->identifier);
		if (!prev_ci) {
			struct json_object *c = json_object_new_object();
			json_object_object_add(c, "identifier", json_object_new_string(ci->identifier));
			json_object_object_add(c, "tags",       tags_to_array(ci->tags));
			json_object_object_add(c, "focused",    json_object_new_int((int32_t)ci->focused));
			json_object_object_add(c, "urgent",     json_object_new_int((int32_t)ci->urgent));
			json_object_object_add(c, "floating",   json_object_new_int((int32_t)ci->floating));
			json_object_object_add(c, "fullscreen", json_object_new_int((int32_t)ci->fullscreen));
			json_object_array_add(added, c);
			any = 1;
		} else {
			struct json_object *c = json_object_new_object();
			int cdiff = 0;
			json_object_object_add(c, "identifier", json_object_new_string(ci->identifier));
			if (ci->tags != prev_ci->tags) {
				json_object_object_add(c, "tags", tags_to_array(ci->tags));
				cdiff = 1;
			}
#define CDIFF_UINT(field, key) \
			if (ci->field != prev_ci->field) { \
				json_object_object_add(c, (key), json_object_new_int((int32_t)ci->field)); \
				cdiff = 1; \
			}
			CDIFF_UINT(focused,    "focused")
			CDIFF_UINT(urgent,     "urgent")
			CDIFF_UINT(floating,   "floating")
			CDIFF_UINT(fullscreen, "fullscreen")
#undef CDIFF_UINT
			if (cdiff) {
				json_object_array_add(changed_clients, c);
				any = 1;
			} else {
				json_object_put(c);
			}
		}
	}
	wl_list_for_each(prev_ci, &out->prev_clients, link) {
		if (!find_client_in_list(&out->clients, prev_ci->identifier)) {
			json_object_array_add(removed, json_object_new_string(prev_ci->identifier));
			any = 1;
		}
	}

	if (json_object_array_length(added) > 0)
		json_object_object_add(diff, "clients_added", added);
	else
		json_object_put(added);
	if (json_object_array_length(removed) > 0)
		json_object_object_add(diff, "clients_removed", removed);
	else
		json_object_put(removed);
	if (json_object_array_length(changed_clients) > 0)
		json_object_object_add(diff, "clients_changed", changed_clients);
	else
		json_object_put(changed_clients);

	if (any) {
		puts(json_object_to_json_string_ext(diff, JSON_C_TO_STRING_PLAIN));
		fflush(stdout);
	}
	json_object_put(diff);
}

static struct json_object *
build_output_json(struct Output *o)
{
	struct json_object *out_obj = json_object_new_object();
	struct json_object *layout_obj = json_object_new_object();
	struct json_object *tags_arr = json_object_new_array();
	struct json_object *clients_arr = json_object_new_array();
	struct ClientInfo *ci;
	uint32_t i;

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
		json_object_object_add(tag_obj, "index", json_object_new_int((int32_t)(i + 1)));
		json_object_object_add(tag_obj, "state",
				json_object_new_int((int32_t)o->tags[i].state));
		json_object_object_add(tag_obj, "clients",
				json_object_new_int((int32_t)o->tags[i].clients));
		json_object_object_add(tag_obj, "focused",
				json_object_new_int((int32_t)o->tags[i].focused));
		json_object_array_add(tags_arr, tag_obj);
	}
	json_object_object_add(out_obj, "tags", tags_arr);

	wl_list_for_each(ci, &o->clients, link) {
		struct json_object *ci_obj = json_object_new_object();
		json_object_object_add(ci_obj, "identifier",
				json_object_new_string(ci->identifier));
		json_object_object_add(ci_obj, "tags", tags_to_array(ci->tags));
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

	return out_obj;
}

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
	wl_list_for_each(o, &outputs, link)
		json_object_array_add(outputs_arr, build_output_json(o));
	json_object_object_add(root, "outputs", outputs_arr);

	/*
	 * Toplevels: metadata from ext_foreign_toplevel_handle_v1 merged with
	 * per-client dwl state from zdwl_ipc_output_v2 (cross-referenced by
	 * identifier).
	 */
	toplevels_arr = json_object_new_array();
	wl_list_for_each(t, &toplevels, link) {
		struct json_object *tl_obj = json_object_new_object();
		struct Output *tl_output = NULL;
		struct ClientInfo *ci = find_client_info(t->identifier, &tl_output);

		json_object_object_add(tl_obj, "identifier",
				json_object_new_string(t->identifier));
		json_object_object_add(tl_obj, "title", json_object_new_string(t->title));
		json_object_object_add(tl_obj, "appid", json_object_new_string(t->appid));

		if (ci) {
			json_object_object_add(tl_obj, "output",
					json_object_new_string(tl_output->name));
			json_object_object_add(tl_obj, "tags", tags_to_array(ci->tags));
			json_object_object_add(tl_obj, "focused",
					json_object_new_int((int32_t)ci->focused));
			json_object_object_add(tl_obj, "urgent",
					json_object_new_int((int32_t)ci->urgent));
			json_object_object_add(tl_obj, "floating",
					json_object_new_int((int32_t)ci->floating));
			json_object_object_add(tl_obj, "fullscreen",
					json_object_new_int((int32_t)ci->fullscreen));
		}

		json_object_array_add(toplevels_arr, tl_obj);
	}
	json_object_object_add(root, "toplevels", toplevels_arr);

	return root;
}

/* ---------------- helpers ---------------- */

static void
cleanup(void)
{
	struct Toplevel *t, *tnext;
	struct Output *o;

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
		wl_list_for_each_safe(ci, cinext, &o->prev_clients, link) {
			wl_list_remove(&ci->link);
			free(ci);
		}
		if (o->ipc)
			zdwl_ipc_output_v2_release(o->ipc);
	}
	zdwl_ipc_manager_v2_release(manager);
	wl_display_disconnect(display);
}

/* Find the output to send actions to (by name, or first active, or first). */
static struct Output *
find_target_output(const char *name)
{
	struct Output *o, *first = NULL, *active = NULL;
	wl_list_for_each(o, &outputs, link) {
		if (!first)
			first = o;
		if (o->active && !active)
			active = o;
		if (name && strcmp(o->name, name) == 0)
			return o;
	}
	if (name) {
		fprintf(stderr, "dwl-cli: output '%s' not found\n", name);
		return NULL;
	}
	return active ? active : first;
}

/* ---------------- main ---------------- */

int
main(int argc, char *argv[])
{
	struct wl_registry *reg;
	struct Output *o;
	struct Output *target;
	const char *output_name = NULL;
	const char *cmd;
	int is_status;
	int ret = 0;
	int i;

	i = 1;
	if (i < argc && strcmp(argv[i], "--output") == 0) {
		if (i + 1 >= argc) {
			fprintf(stderr, "dwl-cli: --output requires an argument\n");
			return 1;
		}
		output_name = argv[i + 1];
		i += 2;
	}

	if (i >= argc) {
		fprintf(stderr, "Usage: dwl-cli [--output <name>] <command> [args]\n"
			"Commands:\n"
			"  status                              print JSON state\n"
			"  watch                               stream JSON lines on each state change\n"
			"  focus <id>                          focus client by identifier\n"
			"  view <mask>                         switch monitor view to tagmask\n"
			"  view-toggle <mask>                  swap to alt tagset with mask\n"
			"  client-tags set <id> <tags>    replace tags\n"
			"  client-tags add <id> <tags>    add tags, keep others\n"
			"  client-tags toggle <id> <tags> toggle tags\n"
			"  client-tags remove <id> <tags> remove tags, keep others\n"
			"  urgent <id> <0|1|on|off|true|false>  set/unset urgency\n"
			"  layout <index>                      switch layout by index\n"
			"\n"
			"Tag mask syntax: N, t<N> (1-indexed), 1,3,5 (comma list), or 0x hex (raw bitmask).\n");
		return 1;
	}
	cmd = argv[i++];

	is_status = (strcmp(cmd, "status") == 0);
	is_watch  = (strcmp(cmd, "watch") == 0);

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "dwl-cli: wl_display_connect failed\n");
		return 1;
	}
	wl_list_init(&outputs);
	wl_list_init(&toplevels);

	reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (!manager) {
		fprintf(stderr, "dwl-cli: compositor lacks zdwl_ipc_manager_v2\n");
		return 1;
	}
	wl_display_roundtrip(display);

	/* Bind ipc_output for every monitor. */
	wl_list_for_each(o, &outputs, link) {
		o->ipc = zdwl_ipc_manager_v2_get_output(manager, o->wl_output);
		zdwl_ipc_output_v2_add_listener(o->ipc, &ipc_output_listener, o);
	}
	wl_display_roundtrip(display);

	if (is_status) {
		struct json_object *root = build_root();
		puts(json_object_to_json_string_ext(root,
				JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED));
		json_object_put(root);
		cleanup();
		return 0;
	}

	if (is_watch) {
		while (wl_display_dispatch(display) != -1)
			;
		cleanup();
		return 0;
	}

	/* --- Action mode --- */
	target = find_target_output(output_name);
	if (!target) {
		cleanup();
		return 1;
	}

	if (strcmp(cmd, "focus") == 0) {
		if (i >= argc) {
			fprintf(stderr, "dwl-cli: focus requires <identifier>\n");
			ret = 1;
		} else {
			zdwl_ipc_output_v2_focus_client(target->ipc, argv[i]);
			wl_display_roundtrip(display);
		}

	} else if (strcmp(cmd, "view") == 0) {
		if (i >= argc) {
			fprintf(stderr, "dwl-cli: view requires <tagmask>\n");
			ret = 1;
		} else {
			uint32_t mask = parse_tagmask(argv[i]);
			if (!mask) {
				fprintf(stderr, "dwl-cli: invalid tagmask '%s'\n", argv[i]);
				ret = 1;
			} else {
				zdwl_ipc_output_v2_set_tags(target->ipc, mask, 0);
				wl_display_roundtrip(display);
			}
		}

	} else if (strcmp(cmd, "view-toggle") == 0) {
		if (i >= argc) {
			fprintf(stderr, "dwl-cli: view-toggle requires <tagmask>\n");
			ret = 1;
		} else {
			uint32_t mask = parse_tagmask(argv[i]);
			if (!mask) {
				fprintf(stderr, "dwl-cli: invalid tagmask '%s'\n", argv[i]);
				ret = 1;
			} else {
				zdwl_ipc_output_v2_set_tags(target->ipc, mask, 1);
				wl_display_roundtrip(display);
			}
		}

	} else if (strcmp(cmd, "client-tags") == 0) {
		const char *op = (i < argc) ? argv[i++] : NULL;
		const char *id = (i < argc) ? argv[i++] : NULL;
		const char *maskstr = (i < argc) ? argv[i++] : NULL;

		if (!op || !id || !maskstr) {
			fprintf(stderr, "dwl-cli: client-tags requires <set|add|toggle|remove> <id> <tags>\n");
			ret = 1;
		} else {
			uint32_t mask = parse_tagmask(maskstr);
			if (!mask) {
				fprintf(stderr, "dwl-cli: invalid tagmask '%s'\n", maskstr);
				ret = 1;
			} else {
				uint32_t and_tags, xor_tags;
				if (strcmp(op, "set") == 0) {
					/* replace: new = mask */
					and_tags = 0;
					xor_tags = mask;
				} else if (strcmp(op, "add") == 0) {
					/* add: new = current | mask */
					and_tags = ~mask;
					xor_tags = mask;
				} else if (strcmp(op, "toggle") == 0) {
					/* toggle: new = current ^ mask */
					and_tags = ~0u;
					xor_tags = mask;
				} else if (strcmp(op, "remove") == 0) {
					/* remove: new = current & ~mask */
					and_tags = ~mask;
					xor_tags = 0;
				} else {
					fprintf(stderr, "dwl-cli: client-tags op must be set, add, toggle, or remove\n");
					ret = 1;
					goto done;
				}
				zdwl_ipc_output_v2_set_client_tags_by_id(target->ipc, id, and_tags, xor_tags);
				wl_display_roundtrip(display);
			}
		}

	} else if (strcmp(cmd, "urgent") == 0) {
		const char *id = (i < argc) ? argv[i++] : NULL;
		const char *valstr = (i < argc) ? argv[i++] : NULL;
		if (!id || !valstr) {
			fprintf(stderr, "dwl-cli: urgent requires <identifier> <0|1|on|off|true|false>\n");
			ret = 1;
		} else {
			uint32_t val;
			if (strcmp(valstr, "on") == 0 || strcmp(valstr, "true") == 0)
				val = 1;
			else if (strcmp(valstr, "off") == 0 || strcmp(valstr, "false") == 0)
				val = 0;
			else
				val = (uint32_t)atoi(valstr);
			zdwl_ipc_output_v2_set_client_urgent(target->ipc, id, val);
			wl_display_roundtrip(display);
		}

	} else if (strcmp(cmd, "layout") == 0) {
		if (i >= argc) {
			fprintf(stderr, "dwl-cli: layout requires <index>\n");
			ret = 1;
		} else {
			uint32_t idx = (uint32_t)atoi(argv[i]);
			zdwl_ipc_output_v2_set_layout(target->ipc, idx);
			wl_display_roundtrip(display);
		}

	} else {
		fprintf(stderr, "dwl-cli: unknown command '%s'\n", cmd);
		fprintf(stderr, "Commands: status, watch, focus, view, view-toggle, client-tags, urgent, layout\n");
		ret = 1;
	}

done:
	cleanup();
	return ret;
}
