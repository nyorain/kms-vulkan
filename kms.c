/*
 * The code in this file actually drives the majority of the KMS API.
 *
 * Most of the property code can be safely ignored. Other clients like
 * kmscube simply have helpers which look up the property/enum ID every
 * time they want to use the property. This works fine, but is really
 * inefficient, and also surprisingly error-prone if you typo the property
 * name somewhere.
 *
 * The property code here is taken from Weston, which optimises for the
 * constant-update case by being able to use static enums to refer to
 * property IDs. The trade-off is that the init/setup code is more
 * difficult to read and reason about.
 */

/*
 * Copyright © 2014-2019 Collabora, Ltd.
 * Copyright © 2014-2015 Intel Corporation
 * Copyright © 2018-2019 DAQRI, LLC and its affiliates
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Author: Daniel Stone <daniels@collabora.com>
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/kd.h>
#include <linux/major.h>
#include <linux/vt.h>

#include "kms-quads.h"

/*
 * All the properties we support for the different object types, as well as
 * the different enum values we support. Changes must be kept in sync with
 * the enum wdrm_* definitions inside the main header.
 *
 * All these properties and enums will be looked up if possible.
 */
static struct drm_property_enum_info plane_type_enums[] = {
	[WDRM_PLANE_TYPE_PRIMARY] = {
		.name = "Primary",
	},
	[WDRM_PLANE_TYPE_OVERLAY] = {
		.name = "Overlay",
	},
	[WDRM_PLANE_TYPE_CURSOR] = {
		.name = "Cursor",
	},
};

static const struct drm_property_info plane_props[] = {
	[WDRM_PLANE_TYPE] = {
		.name = "type",
		.enum_values = plane_type_enums,
		.num_enum_values = WDRM_PLANE_TYPE__COUNT,
	},
	[WDRM_PLANE_SRC_X] = { .name = "SRC_X", },
	[WDRM_PLANE_SRC_Y] = { .name = "SRC_Y", },
	[WDRM_PLANE_SRC_W] = { .name = "SRC_W", },
	[WDRM_PLANE_SRC_H] = { .name = "SRC_H", },
	[WDRM_PLANE_CRTC_X] = { .name = "CRTC_X", },
	[WDRM_PLANE_CRTC_Y] = { .name = "CRTC_Y", },
	[WDRM_PLANE_CRTC_W] = { .name = "CRTC_W", },
	[WDRM_PLANE_CRTC_H] = { .name = "CRTC_H", },
	[WDRM_PLANE_FB_ID] = { .name = "FB_ID", },
	[WDRM_PLANE_CRTC_ID] = { .name = "CRTC_ID", },
	[WDRM_PLANE_IN_FORMATS] = { .name = "IN_FORMATS" },
	[WDRM_PLANE_IN_FENCE_FD] = { .name = "IN_FENCE_FD" },
};

static struct drm_property_enum_info dpms_state_enums[] = {
	[WDRM_DPMS_STATE_OFF] = {
		.name = "Off",
	},
	[WDRM_DPMS_STATE_ON] = {
		.name = "On",
	},
	[WDRM_DPMS_STATE_STANDBY] = {
		.name = "Standby",
	},
	[WDRM_DPMS_STATE_SUSPEND] = {
		.name = "Suspend",
	},
};

static const struct drm_property_info connector_props[] = {
	[WDRM_CONNECTOR_EDID] = { .name = "EDID" },
	[WDRM_CONNECTOR_DPMS] = {
		.name = "DPMS",
		.enum_values = dpms_state_enums,
		.num_enum_values = WDRM_DPMS_STATE__COUNT,
	},
	[WDRM_CONNECTOR_CRTC_ID] = { .name = "CRTC_ID", },
	[WDRM_CONNECTOR_NON_DESKTOP] = { .name = "non-desktop", },
};

static const struct drm_property_info crtc_props[] = {
	[WDRM_CRTC_MODE_ID] = { .name = "MODE_ID", },
	[WDRM_CRTC_ACTIVE] = { .name = "ACTIVE", },
	[WDRM_CRTC_OUT_FENCE_PTR] = { .name = "OUT_FENCE_PTR", },
};

/**
 * Get the current value of a KMS property
 *
 * Given a drmModeObjectGetProperties return, as well as the drm_property_info
 * for the target property, return the current value of that property,
 * with an optional default. If the property is a KMS enum type, the return
 * value will be translated into the appropriate internal enum.
 *
 * If the property is not present, the default value will be returned.
 *
 * @param info Internal structure for property to look up
 * @param props Raw KMS properties for the target object
 * @param def Value to return if property is not found
 */
static uint64_t
drm_property_get_value(struct drm_property_info *info,
		       const drmModeObjectProperties *props,
		       uint64_t def)
{
	unsigned int i;

	if (info->prop_id == 0)
		return def;

	for (i = 0; i < props->count_props; i++) {
		unsigned int j;

		if (props->props[i] != info->prop_id)
			continue;

		/* Simple (non-enum) types can return the value directly */
		if (info->num_enum_values == 0)
			return props->prop_values[i];

		/* Map from raw value to enum value */
		for (j = 0; j < info->num_enum_values; j++) {
			if (!info->enum_values[j].valid)
				continue;
			if (info->enum_values[j].value != props->prop_values[i])
				continue;

			return j;
		}

		/* We don't have a mapping for this enum; return default. */
		break;
	}

	return def;
}

/**
 * Cache DRM property values
 *
 * Update a per-object array of drm_property_info structures, given the
 * DRM properties of the object.
 *
 * Call this every time an object newly appears (note that only connectors
 * can be hotplugged), the first time it is seen, or when its status changes
 * in a way which invalidates the potential property values (currently, the
 * only case for this is connector hotplug).
 *
 * This updates the property IDs and enum values within the drm_property_info
 * array.
 *
 * DRM property enum values are dynamic at runtime; the user must query the
 * property to find out the desired runtime value for a requested string
 * name. Using the 'type' field on planes as an example, there is no single
 * hardcoded constant for primary plane types; instead, the property must be
 * queried at runtime to find the value associated with the string "Primary".
 *
 * This helper queries and caches the enum values, to allow us to use a set
 * of compile-time-constant enums portably across various implementations.
 * The values given in enum_names are searched for, and stored in the
 * same-indexed field of the map array.
 *
 * @param device Device
 * @param src DRM property info array to source from
 * @param info DRM property info array to copy into
 * @param num_infos Number of entries in the source array
 * @param props DRM object properties for the object
 */
static void
drm_property_info_populate(struct device *device,
		           const struct drm_property_info *src,
			   struct drm_property_info *info,
			   unsigned int num_infos,
			   drmModeObjectProperties *props)
{
	drmModePropertyRes *prop;
	unsigned i, j;

	for (i = 0; i < num_infos; i++) {
		unsigned int j;

		info[i].name = src[i].name;
		info[i].prop_id = 0;
		info[i].num_enum_values = src[i].num_enum_values;

		if (info[i].num_enum_values == 0)
			continue;

		info[i].enum_values =
			malloc(src[i].num_enum_values *
			       sizeof(*info[i].enum_values));
		assert(info[i].enum_values);
		for (j = 0; j < info[i].num_enum_values; j++) {
			info[i].enum_values[j].name = src[i].enum_values[j].name;
			info[i].enum_values[j].valid = false;
		}
	}

	for (i = 0; i < props->count_props; i++) {
		unsigned int k;

		prop = drmModeGetProperty(device->kms_fd, props->props[i]);
		if (!prop)
			continue;

		for (j = 0; j < num_infos; j++) {
			if (!strcmp(prop->name, info[j].name))
				break;
		}

		/* We don't know/care about this property. */
		if (j == num_infos) {
			drmModeFreeProperty(prop);
			continue;
		}

		info[j].prop_id = props->props[i];

		/* Make sure we don't get mixed up between enum and normal
		 * properties. */
		assert(!!(prop->flags & DRM_MODE_PROP_ENUM) ==
		       !!info[j].num_enum_values);

		for (k = 0; k < info[j].num_enum_values; k++) {
			int l;

			for (l = 0; l < prop->count_enums; l++) {
				if (!strcmp(prop->enums[l].name,
					    info[j].enum_values[k].name))
					break;
			}

			if (l == prop->count_enums)
				continue;

			info[j].enum_values[k].valid = true;
			info[j].enum_values[k].value = prop->enums[l].value;
		}

		drmModeFreeProperty(prop);
	}
}

/**
 * Free DRM property information
 *
 * Frees all memory associated with a DRM property info array and zeroes
 * it out, leaving it usable for a further drm_property_info_update() or
 * drm_property_info_free().
 *
 * @param info DRM property info array
 * @param num_props Number of entries in array to free
 */
static void
drm_property_info_free(struct drm_property_info *info, int num_props)
{
	int i;

	for (i = 0; i < num_props; i++)
		free(info[i].enum_values);

	memset(info, 0, sizeof(*info) * num_props);
}

static const char * const connector_type_names[] = {
	[DRM_MODE_CONNECTOR_Unknown]     = "Unknown",
	[DRM_MODE_CONNECTOR_VGA]         = "VGA",
	[DRM_MODE_CONNECTOR_DVII]        = "DVI-I",
	[DRM_MODE_CONNECTOR_DVID]        = "DVI-D",
	[DRM_MODE_CONNECTOR_DVIA]        = "DVI-A",
	[DRM_MODE_CONNECTOR_Composite]   = "Composite",
	[DRM_MODE_CONNECTOR_SVIDEO]      = "SVIDEO",
	[DRM_MODE_CONNECTOR_LVDS]        = "LVDS",
	[DRM_MODE_CONNECTOR_Component]   = "Component",
	[DRM_MODE_CONNECTOR_9PinDIN]     = "DIN",
	[DRM_MODE_CONNECTOR_DisplayPort] = "DP",
	[DRM_MODE_CONNECTOR_HDMIA]       = "HDMI-A",
	[DRM_MODE_CONNECTOR_HDMIB]       = "HDMI-B",
	[DRM_MODE_CONNECTOR_TV]          = "TV",
	[DRM_MODE_CONNECTOR_eDP]         = "eDP",
#ifdef DRM_MODE_CONNECTOR_DSI
	[DRM_MODE_CONNECTOR_VIRTUAL]     = "Virtual",
	[DRM_MODE_CONNECTOR_DSI]         = "DSI",
#endif
#ifdef DRM_MODE_CONNECTOR_DPI
	[DRM_MODE_CONNECTOR_DPI]         = "DPI",
#endif
#ifdef DRM_MODE_CONNECTOR_WRITEBACK
	[DRM_MODE_CONNECTOR_WRITEBACK]	 = "Writeback",
#endif
};

/*
 * The IN_FORMATS blob has two variable-length arrays at the end; one of
 * uint32_t formats, and another of the supported modifiers. To allow the
 * blob to be extended and carry more information, they carry byte offsets
 * pointing to the start of the two arrays.
 */
static inline uint32_t *
formats_ptr(struct drm_format_modifier_blob *blob)
{
	return (uint32_t *)(((char *)blob) + blob->formats_offset);
}

static inline struct drm_format_modifier *
modifiers_ptr(struct drm_format_modifier_blob *blob)
{
	return (struct drm_format_modifier *)
		(((char *)blob) + blob->modifiers_offset);
}

/*
 * This populates the list of supported formats and modifiers for the output's
 * primary plane. The IN_FORMATS property, available on every plane, declares
 * the supported format + modifier combinations for the plane.
 *
 * The parsing is somewhat difficult, so rather than accessing it on demand,
 * here we simply turn it into an array of modifiers, which can be directly
 * passed to, e.g., gbm_surface_create_with_modifiers().
 */
static void plane_formats_populate(struct output *output,
				   drmModeObjectPropertiesPtr props)
{
	uint32_t blob_id;
	drmModePropertyBlobRes *blob;
	struct drm_format_modifier_blob *fmt_mod_blob; /* IN_FORMATS content */
	uint32_t *blob_formats; /* array of formats */
	struct drm_format_modifier *blob_modifiers;

	blob_id = drm_property_get_value(&output->props.plane[WDRM_PLANE_IN_FORMATS],
					 props, 0);
	if (blob_id == 0) {
		debug("[%s] plane does not have IN_FORMATS\n", output->name);
		return;
	}

	blob = drmModeGetPropertyBlob(output->device->kms_fd, blob_id);
	assert(blob);

	fmt_mod_blob = blob->data;
	blob_formats = formats_ptr(fmt_mod_blob);
	blob_modifiers = modifiers_ptr(fmt_mod_blob);

	for (unsigned int f = 0; f < fmt_mod_blob->count_formats; f++) {
		if (blob_formats[f] != DRM_FORMAT_XRGB8888)
			continue;

		for (unsigned int m = 0; m < fmt_mod_blob->count_modifiers; m++) {
			struct drm_format_modifier *mod = &blob_modifiers[m];

			if ((m < mod->offset) || (m > mod->offset + 63))
				continue;
			if (!(mod->formats & (1 << (m - mod->offset))))
				continue;

			output->modifiers = realloc(output->modifiers,
						    (output->num_modifiers + 1) * sizeof(*output->modifiers));
			assert(output->modifiers);
			output->modifiers[output->num_modifiers++] = mod->modifier;
		}
	}

	drmModeFreePropertyBlob(blob);
}

/*
 * This gets and prints a little bit of information from the EDID block,
 * as described in edid.c.
 */
static void output_get_edid(struct output *output,
			    drmModeObjectPropertiesPtr props)
{
	drmModePropertyBlobPtr blob;
	struct edid_info *edid;
	uint32_t blob_id;
	int ret;

	blob_id = drm_property_get_value(&output->props.connector[WDRM_CONNECTOR_EDID],
					 props, 0);
	if (blob_id == 0) {
		debug("[%s] output does not have EDID\n", output->name);
		return;
	}

	blob = drmModeGetPropertyBlob(output->device->kms_fd, blob_id);
	assert(blob);

	edid = edid_parse(blob->data, blob->length);
	drmModeFreePropertyBlob(blob);
	if (!edid)
		return;

	debug("[%s] EDID PNP ID %s, EISA ID %s, name %s, serial %s\n",
	       output->name, edid->pnp_id, edid->eisa_id,
	       edid->monitor_name, edid->serial_number);
	free(edid);
}

/*
 * In atomic modesetting, any kind of bulk transfer is handled by blob
 * properties. For instance, the 'MODE_ID' property will be set to a 32-bit
 * value giving the ID of a blob (or 0 if no mode applies), the content of
 * which can then be fetched with drmModeGetPropertyBlob (or the getpropblob
 * ioctl). Users can also create new blobs with drmModeCreatePropertyBlob (or
 * CREATEPROPBLOB).
 *
 * These are also used for EDID to communicate monitor mode information,
 * gamma ramps and other colour management information, and anything which
 * is too large to usefully describe through properties.
 */
static uint32_t mode_blob_create(struct device *device, drmModeModeInfo *mode)
{
	uint32_t ret;
	int err;

	err = drmModeCreatePropertyBlob(device->kms_fd, mode, sizeof(*mode), &ret);
	if (err < 0) {
		fprintf(stderr, "couldn't create MODE_ID blob: %m\n");
		return 0;
	}

	return ret;
}

/*
 * Create an output structure by working backwards from a connector to
 * find an active plane -> CRTC -> connector display chain. Also fills in the
 * object property structures so they're ready for use.
 *
 * This reuses existing routing, so requires the target connector to already be
 * active. It is possible to use our routing, but per the comment below, this
 * makes the code more difficult to follow.
 *
 * In a system which tracks every KMS object (plane/CRTC/connector), instead of
 * calling drmModeGet*() for each object, you could instead just use
 * drm_property_get_value() on each object to determine the current routing.
 */
struct output *output_create(struct device *device,
			     drmModeConnectorPtr connector)
{
	struct output *output = NULL;
	drmModeObjectPropertiesPtr props;
	drmModeEncoderPtr encoder = NULL;
	drmModePlanePtr plane = NULL;
	drmModeCrtcPtr crtc = NULL;
	uint64_t refresh;

	/* Find the encoder (a deprecated KMS object) for this connector. */
	if (connector->encoder_id == 0) {
		debug("[CONN:%" PRIu32 "]: no encoder\n", connector->connector_id);
		return NULL;
	}
	for (int e = 0; e < device->res->count_encoders; e++) {
		if (device->res->encoders[e] == connector->encoder_id) {
			encoder = drmModeGetEncoder(device->kms_fd,
						    device->res->encoders[e]);
			break;
		}
	}
	assert(encoder);

	/*
	 * Find the CRTC currently used by this connector. It is possible to
	 * use a different CRTC if desired, however unlike the pre-atomic API,
	 * we have to explicitly change every object in the routing path.
	 *
	 * For example, if we wanted to use CRTC 44 for connector 51, but
	 * connector 45 was currently active on that CRTC, we would need to
	 * also set CRTC_ID == 0 on connector 45 when we committed the request
	 * with CRTC_ID == 44 on connector 51.
	 *
	 * This is entirely doable, but requires more book-keeping: notably,
	 * tracking every plane/CRTC/connector exposed by KMS, and making sure
	 * our first commit set or cleared out the state on every object.
	 * Weston handles this with its 'state_invalid' flag.
	 *
	 * As this makes enumeration more complex, we have chosen to not do
	 * that in this example.
	 */
	if (encoder->crtc_id == 0) {
		debug("[CONN:%" PRIu32 "]: no CRTC\n", connector->connector_id);
		goto out_encoder;
	}
	for (int c = 0; c < device->res->count_crtcs; c++) {
		if (device->res->crtcs[c] == encoder->crtc_id) {
			crtc = drmModeGetCrtc(device->kms_fd,
					      device->res->crtcs[c]);
			break;
		}
	}
	assert(crtc);

	/* Ensure the CRTC is active. */
	if (crtc->buffer_id == 0) {
		debug("[CONN:%" PRIu32 "]: not active\n", connector->connector_id);
		goto out_crtc;
	}

	/*
	 * The kernel doesn't directly tell us what it considers to be the
	 * single primary plane for this CRTC (i.e. what would be updated
	 * by drmModeSetCrtc), but if it's already active then we can cheat
	 * by looking for something displaying the same framebuffer ID,
	 * since that information is duplicated.
	 */
	for (int p = 0; p < device->num_planes; p++) {
		debug("[PLANE: %" PRIu32 "] CRTC ID %" PRIu32 ", FB %" PRIu32 "\n", device->planes[p]->plane_id, device->planes[p]->crtc_id, device->planes[p]->fb_id);
		if (device->planes[p]->crtc_id == crtc->crtc_id &&
		    device->planes[p]->fb_id == crtc->buffer_id) {
			plane = device->planes[p];
			break;
		}
	}
	assert(plane);

	/* DRM is supposed to provide a refresh interval, but often doesn't;
	 * calculate our own in milliHz for higher precision anyway. */
	refresh = ((crtc->mode.clock * 1000000LL / crtc->mode.htotal) +
		   (crtc->mode.vtotal / 2)) / crtc->mode.vtotal;

	printf("[CRTC:%" PRIu32 ", CONN %" PRIu32 ", PLANE %" PRIu32 "]: active at %u x %u, %" PRIu64 " mHz\n",
	       crtc->crtc_id, connector->connector_id, plane->plane_id,
	       crtc->width, crtc->height, refresh);

	output = calloc(1, sizeof(*output));
	assert(output);
	output->device = device;
	output->primary_plane_id = plane->plane_id;
	output->crtc_id = crtc->crtc_id;
	output->connector_id = connector->connector_id;
	output->commit_fence_fd = -1;
	asprintf(&output->name, "%s-%d",
		 (connector->connector_type < ARRAY_LENGTH(connector_type_names) ?
		 	connector_type_names[connector->connector_type] :
			"UNKNOWN"),
		 connector->connector_type_id);
	output->needs_repaint = true;

	/*
	 * Just reuse the CRTC's existing mode: requires it to already be
	 * active. In order to use a different mode, we could look at the
	 * mode list exposed in the connector, or construct a new DRM mode
	 * from EDID.
	 */
	output->mode = crtc->mode;
	output->refresh_interval_nsec = millihz_to_nsec(refresh);
	debug("[%s] refresh interval %" PRIu64 "ns / %" PRIu64 "ms\n", output->name, output->refresh_interval_nsec, output->refresh_interval_nsec / 1000000UL);
	output->mode_blob_id = mode_blob_create(device, &output->mode);

	/*
	 * Now we have all our objects lined up, get their property lists from
	 * KMS and use that to fill in the props structures we have above, so
	 * we can more easily query and set them.
	 */
	props = drmModeObjectGetProperties(device->kms_fd, output->primary_plane_id,
					   DRM_MODE_OBJECT_PLANE);
	assert(props);
	drm_property_info_populate(device, plane_props, output->props.plane,
				   WDRM_PLANE__COUNT, props);
	plane_formats_populate(output, props);
	drmModeFreeObjectProperties(props);

	props = drmModeObjectGetProperties(device->kms_fd, output->crtc_id,
					   DRM_MODE_OBJECT_CRTC);
	assert(props);
	drm_property_info_populate(device, crtc_props, output->props.crtc,
				   WDRM_CRTC__COUNT, props);
	drmModeFreeObjectProperties(props);

	props = drmModeObjectGetProperties(device->kms_fd, output->connector_id,
					   DRM_MODE_OBJECT_CONNECTOR);
	assert(props);
	drm_property_info_populate(device, connector_props, output->props.connector,
				   WDRM_CONNECTOR__COUNT, props);
	output_get_edid(output, props);
	drmModeFreeObjectProperties(props);

	/*
	 * Set if we support explicit fencing inside KMS; the EGL renderer will
	 * clear this if it doesn't support it.
	 */
	output->explicit_fencing =
		(output->props.plane[WDRM_PLANE_IN_FENCE_FD].prop_id &&
		 output->props.crtc[WDRM_CRTC_OUT_FENCE_PTR].prop_id);

	drmModeFreePlane(plane);
out_crtc:
	drmModeFreeCrtc(crtc);
out_encoder:
	drmModeFreeEncoder(encoder);

	return output;
}

void output_destroy(struct output *output)
{
	struct device *device = output->device;
	int i;

	for (i = 0; i < BUFFER_QUEUE_DEPTH; i++) {
		if (output->buffers[i])
			buffer_destroy(output->buffers[i]);
	}

	if (output->device->egl_dpy)
		output_egl_destroy(device, output);

	if (output->mode_blob_id != 0)
		drmModeDestroyPropertyBlob(device->kms_fd, output->mode_blob_id);

	free(output);
}

/* Sets a CRTC property inside an atomic request. */
static int
crtc_add_prop(drmModeAtomicReq *req, struct output *output,
	      enum wdrm_crtc_property prop, uint64_t val)
{
	struct drm_property_info *info = &output->props.crtc[prop];
	int ret;

	if (info->prop_id == 0)
		return -1;

	ret = drmModeAtomicAddProperty(req, output->crtc_id, info->prop_id,
				       val);
	debug("\t[CRTC:%lu] %lu (%s) -> %llu (0x%llx)\n",
	      (unsigned long) output->crtc_id,
	      (unsigned long) info->prop_id, info->name,
	      (unsigned long long) val, (unsigned long long) val);
	return (ret <= 0) ? -1 : 0;
}

/* Sets a connector property inside an atomic request. */
static int
connector_add_prop(drmModeAtomicReq *req, struct output *output,
		   enum wdrm_connector_property prop, uint64_t val)
{
	struct drm_property_info *info = &output->props.connector[prop];
	int ret;

	if (info->prop_id == 0)
		return -1;

	ret = drmModeAtomicAddProperty(req, output->connector_id,
				       info->prop_id, val);
	debug("\t[CONN:%lu] %lu (%s) -> %llu (0x%llx)\n",
	      (unsigned long) output->connector_id,
	      (unsigned long) info->prop_id, info->name,
	      (unsigned long long) val, (unsigned long long) val);
	return (ret <= 0) ? -1 : 0;
}

/* Sets a plane property inside an atomic request. */
static int
plane_add_prop(drmModeAtomicReq *req, struct output *output,
	       enum wdrm_plane_property prop, uint64_t val)
{
	struct drm_property_info *info = &output->props.plane[prop];
	int ret;

	if (info->prop_id == 0)
		return -1;

	ret = drmModeAtomicAddProperty(req, output->primary_plane_id,
				       info->prop_id, val);
	debug("\t[PLANE:%lu] %lu (%s) -> %llu (0x%llx)\n",
	      (unsigned long) output->primary_plane_id,
	      (unsigned long) info->prop_id, info->name,
	      (unsigned long long) val, (unsigned long long) val);
	return (ret <= 0) ? -1 : 0;
}


/*
 * Populates an atomic request structure with this output's current
 * configuration.
 *
 * Atomic requests are applied incrementally on top of the current state, so
 * there is no need here to apply the entire output state, except on the first
 * modeset if we are changing the display routing (per output_create comments).
 */
void output_add_atomic_req(struct output *output, drmModeAtomicReqPtr req,
			   struct buffer *buffer)
{
	int ret;

	debug("[%s] atomic state for commit:\n", output->name);

	ret = plane_add_prop(req, output, WDRM_PLANE_CRTC_ID, output->crtc_id);

	/*
	 * SRC_X/Y/W/H are the co-ordinates to use as the dimensions of the
	 * framebuffer source: you can use these to crop an image. Source
	 * co-ordinates are in 16.16 fixed-point to allow for better scaling;
	 * as we just use a full-size uncropped image, we don't need this.
	 */
	ret |= plane_add_prop(req, output, WDRM_PLANE_FB_ID, buffer->fb_id);
	if (output->explicit_fencing && buffer->render_fence_fd >= 0) {
		assert(linux_sync_file_is_valid(buffer->render_fence_fd));
		ret |= plane_add_prop(req, output, WDRM_PLANE_IN_FENCE_FD,
				      buffer->render_fence_fd);
	}
	ret |= plane_add_prop(req, output, WDRM_PLANE_SRC_X, 0);
	ret |= plane_add_prop(req, output, WDRM_PLANE_SRC_Y, 0);
	ret |= plane_add_prop(req, output, WDRM_PLANE_SRC_W,
			      buffer->width << 16);
	ret |= plane_add_prop(req, output, WDRM_PLANE_SRC_H,
			      buffer->height << 16);

	/*
	 * DST_X/Y/W/H position the plane's output within the CRTC's output
	 * space; these positions are plain integer, as it makes no sense for
	 * output positions to be expressed in subpixels.
	 *
	 * Anyway, we just use a full-screen buffer with no scaling.
	 */
	ret |= plane_add_prop(req, output, WDRM_PLANE_CRTC_X, 0);
	ret |= plane_add_prop(req, output, WDRM_PLANE_CRTC_Y, 0);
	ret |= plane_add_prop(req, output, WDRM_PLANE_CRTC_W, buffer->width);
	ret |= plane_add_prop(req, output, WDRM_PLANE_CRTC_H, buffer->height);

	/* Ensure we do actually have a full-screen buffer. */
	assert(buffer->width == output->mode.hdisplay);
	assert(buffer->height == output->mode.vdisplay);

	/*
	 * Changing any of these three properties requires the ALLOW_MODESET
	 * flag to be set on the atomic commit.
	 */
	ret |= crtc_add_prop(req, output, WDRM_CRTC_MODE_ID,
			     output->mode_blob_id);
	ret |= crtc_add_prop(req, output, WDRM_CRTC_ACTIVE, 1);

	if (output->explicit_fencing) {
		if (output->commit_fence_fd >= 0)
			close(output->commit_fence_fd);
		output->commit_fence_fd = -1;

		/*
		 * OUT_FENCE_PTR takes a pointer as a value, which the kernel
		 * fills in at commit time. The fence signals when the commit
		 * completes, i.e. when the event we request is sent.
		 */
		ret |= crtc_add_prop(req, output, WDRM_CRTC_OUT_FENCE_PTR,
				     (uint64_t) (uintptr_t) &output->commit_fence_fd);
	}

	ret |= connector_add_prop(req, output, WDRM_CONNECTOR_CRTC_ID,
				  output->crtc_id);

	assert(ret == 0);
}

/*
 * Commits the atomic state to KMS.
 *
 * Using the NONBLOCK + PAGE_FLIP_EVENT flags means that we will return
 * immediately; when the flip has actually been completed in hardware,
 * the KMS FD will become readable via select() or poll(), and we will
 * receive an event to be read and dispatched via drmHandleEvent().
 *
 * For atomic commits, this goes to the page_flip_handler2 vfunc we set
 * in our DRM event context passed to drmHandleEvent(), which will be
 * called once for each CRTC affected by this atomic commit; the last
 * parameter of drmModeAtomicCommit() is a user-data parameter which
 * will be passed to the handler.
 *
 * The ALLOW_MODESET flag should not be used in regular operation.
 * Commits which require potentially expensive operations: changing clocks,
 * per-block power toggles, or anything with a setup time which requires
 * a longer-than-usual wait. It is used when we are changing the routing
 * or modes; here we set it on our first commit (since the prior state
 * could be very different), but make sure to not use it in steady state.
 *
 * Another flag which can be used - but isn't here - is TEST_ONLY. This
 * flag simply checks whether or not the atomic commit _would_ succeed,
 * and returns without committing the state to the kernel. Weston uses
 * this to determine whether or not we can use overlays by brute force:
 * we try to place each view on a particular plane one by one, testing
 * whether or not it succeeds for each plane. TEST_ONLY commits are very
 * cheap, so can be used to iteratively determine a successful configuration,
 * as KMS itself does not describe the constraints a driver has, e.g.
 * certain planes can only scale by certain amounts.
 */
int atomic_commit(struct device *device, drmModeAtomicReqPtr req,
		  bool allow_modeset)
{
	int ret;
	uint32_t flags = (DRM_MODE_ATOMIC_NONBLOCK |
			  DRM_MODE_PAGE_FLIP_EVENT);

	if (allow_modeset)
		flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;

	return drmModeAtomicCommit(device->kms_fd, req, flags, device);
}
