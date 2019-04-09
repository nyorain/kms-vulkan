/*
 * Copyright © 2018-2019 Collabora, Ltd.
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

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

/*
 * Headers from the kernel's DRM uABI, allowing us to use ioctls directly.
 * These come from the kernel, via libdrm.
 */
#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

/*
 * Headers from the libdrm userspace library API, prefixed xf86*. These
 * mostly provide device and resource enumeration, as well as wrappers
 * around many ioctls, notably atomic modesetting.
 */
#include <xf86drm.h>
#include <xf86drmMode.h>

/* GBM allocates buffers we can use with both EGL and KMS. */
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

/* Utility header from Weston to more easily handle time values. */
#include "timespec-util.h"


#ifdef DEBUG
#define debug(...) fprintf(stderr, __VA_ARGS__)
#else
#define debug(...) /**/
#endif

#define error(...) fprintf(stderr, __VA_ARGS__)

#define ARRAY_LENGTH(x) (sizeof(x) / sizeof((x)[0]))

struct buffer;
struct device;
struct output;


#define BUFFER_QUEUE_DEPTH 3 /* how many buffers to allocate per output */
#define NUM_ANIM_FRAMES 240 /* how many frames before we wrap around */


/**
 * Represents the values of an enum-type KMS property. These properties
 * have a certain range of values you can use, exposed as strings from
 * the kernel; userspace needs to look up the value that string
 * corresponds to and use it.
 */
struct drm_property_enum_info {
	const char *name; /**< name as string (static, not freed) */
	bool valid; /**< true if value is supported; ignore if false */
	uint64_t value; /**< raw value */
};

/**
 * Holds information on a DRM property, including its ID and the enum
 * values it holds.
 *
 * DRM properties are allocated dynamically, and maintained as DRM objects
 * within the normal object ID space; they thus do not have a stable ID
 * to refer to. This includes enum values, which must be referred to by
 * integer values, but these are not stable.
 *
 * drm_property_info allows a cache to be maintained where we can use
 * enum values internally to refer to properties, with the mapping to DRM
 * ID values being maintained internally.
 */
struct drm_property_info {
	const char *name; /**< name as string (static, not freed) */
	uint32_t prop_id; /**< KMS property object ID */
	unsigned int num_enum_values; /**< number of enum values */
	struct drm_property_enum_info *enum_values; /**< array of enum values */
};

/**
 * List of properties attached to DRM planes
 */
enum wdrm_plane_property {
	WDRM_PLANE_TYPE = 0,
	WDRM_PLANE_SRC_X,
	WDRM_PLANE_SRC_Y,
	WDRM_PLANE_SRC_W,
	WDRM_PLANE_SRC_H,
	WDRM_PLANE_CRTC_X,
	WDRM_PLANE_CRTC_Y,
	WDRM_PLANE_CRTC_W,
	WDRM_PLANE_CRTC_H,
	WDRM_PLANE_FB_ID,
	WDRM_PLANE_CRTC_ID,
	WDRM_PLANE_IN_FORMATS,
	WDRM_PLANE_IN_FENCE_FD,
	WDRM_PLANE__COUNT
};

/**
 * Possible values for the WDRM_PLANE_TYPE property.
 */
enum wdrm_plane_type {
	WDRM_PLANE_TYPE_PRIMARY = 0,
	WDRM_PLANE_TYPE_CURSOR,
	WDRM_PLANE_TYPE_OVERLAY,
	WDRM_PLANE_TYPE__COUNT
};

/**
 * List of properties attached to a DRM connector
 */
enum wdrm_connector_property {
	WDRM_CONNECTOR_EDID = 0,
	WDRM_CONNECTOR_DPMS,
	WDRM_CONNECTOR_CRTC_ID,
	WDRM_CONNECTOR_NON_DESKTOP,
	WDRM_CONNECTOR__COUNT
};

enum wdrm_dpms_state {
	WDRM_DPMS_STATE_OFF = 0,
	WDRM_DPMS_STATE_ON,
	WDRM_DPMS_STATE_STANDBY, /* unused */
	WDRM_DPMS_STATE_SUSPEND, /* unused */
	WDRM_DPMS_STATE__COUNT
};

/**
 * List of properties attached to DRM CRTCs
 */
enum wdrm_crtc_property {
	WDRM_CRTC_MODE_ID = 0,
	WDRM_CRTC_ACTIVE,
	WDRM_CRTC_OUT_FENCE_PTR,
	WDRM_CRTC__COUNT
};


/*
 * A buffer to display on screen. We currently use KMS dumb buffers for this.
 * Dumb buffers are specifically limited to the usecase of allocating linear
 * (not tiled) memory, using mmap to write to the buffer with the CPU only,
 * unmapping, and then displaying the buffer on the primary plane.
 *
 * Other ways to get buffers include rendering through GL (implemented by
 * using GBM as a buffer allocation shim in between KMS and EGL) or Vulkan,
 * or importing dmabufs created elsewhere. All of the above methods will
 * give you a GEM handle directly.
 */
struct buffer {
	/*
	 * The output this buffer was created for.
	 */
	struct output *output;

	/*
	 * true if this buffer is currently owned by KMS.
	 */
	bool in_use;

	/*
	 * The GEM handle for this buffer, returned from the dumb-buffer
	 * creation ioctl. GEM names are also returned from
	 * drmModePrimeFDToHandle when importing dmabufs directly, however
	 * this has huge caveats and you almost certainly shouldn't do it
	 * directly.
	 *
	 * GEM handles (or 'names') represents a dumb bag of bits: in order to
	 * display the buffer we've created, we need to create a framebuffer,
	 * which annotates our GEM buffer with additional metadata.
	 *
	 * 0 is always an invalid GEM handle.
	 */
	uint32_t gem_handles[4];

	/*
	 * Framebuffers wrap GEM buffers with additional metadata such as the
	 * image dimensions. It is this framebuffer ID which is passed to KMS
	 * to display.
	 *
	 * 0 is always an invalid framebuffer id.
	 */
	uint32_t fb_id;

	/*
	 * dma_fence FD for completion of the rendering to this buffer.
	 */
	int render_fence_fd;

	/*
	 * dma_fence FD for completion of the last KMS commit this buffer
	 * was used in.
	 */
	int kms_fence_fd;

	/*
	 * The format and modifier together describe how the image is laid
	 * out in memory; both are canonically described in drm_fourcc.h.
	 *
	 * The format contains information on how each pixel is laid out:
	 * the type, ordering, and width of each colour channel within the
	 * pixel. The modifier contains information on how pixels are laid
	 * out within the buffer, e.g. whether they are purely linear,
	 * tiled, compressed, etc.
	 *
	 * Each plane has a property named 'IN_FORMATS' which describes
	 * the format + modifier combinations it will accept; the framebuffer
	 * must be allocated accordingly.
	 *
	 * An example of parsing 'IN_FORMATS' to create an array of formats,
	 * each containing an array of modifiers, can be found in libweston's
	 * compositor-drm.c.
	 *
	 * 0 is always an invalid format ID. However, a 0 modifier always means
	 * that the image has a linear layout; DRM_FORMAT_MOD_INVALID is the
	 * invalid modifier, which is used to signify that the user does not
	 * know or care about the modifier being used.
	 */
	uint32_t format;
	uint64_t modifier;

	/* Parameters for our memory-mapped image. */
	struct {
		uint32_t *mem;
		unsigned int size;
	} dumb;

	struct {
		struct gbm_bo *bo;
		EGLImage img;
		GLuint tex_id;
		GLuint fbo_id;
	} gbm;

	unsigned int width;
	unsigned int height;
	unsigned int pitches[4]; /* in bytes */
	unsigned int offsets[4]; /* in bytes */
};

/*
 * An 'output' is our abstractive structure of a plane -> CRTC -> connector
 * display pipeline.
 *
 * Working backwards:
 *   - the connector represents a connection to a physical device, i.e. one
 *     monitor; connectors have native modes (usually coming from the output
 *     device's EDID), a number of properties describing properties of the
 *     device and output stream such as 'Broadcast RGB' which informs the
 *     device of the colour encoding and range
 *   - the CRTC is responsible for generating the image to display on one or
 *     more connectors; its main property is the mode, which configures the
 *     output resolution, as well as various other important properties such
 *     as gamma/CTM/degamma for colour management; the CRTC produces this
 *     output by combining images fed to it by one or more ...
 *   - planes take an input image (via framebuffers), optionally crop and
 *     scale the image, and then place that image within the CRTC; they have
 *     some properties such as zpos (for ordering overlapping planes) and
 *     colour management
 *
 * To simplify things, we define each output as one plane -> CRTC -> connector
 * chain. We pick a connector, find a CRTC which works with that connector
 * (searching back through the encoder, which is a deprecated and unused KMS
 * object), then find a primary plane which works with that CRTC.
 *
 * There are multiple types of planes: primary planes are usually used to
 * show a single flat fullscreen image, overlay planes are used to display
 * content on top of this which is blended by the display controller (often
 * video content), and cursor planes are almost exclusively used for mouse
 * cursors. For our uses, we only care about primary planes.
 *
 * Note that _only_ overlay planes will be enumerated by default; enabling
 * the 'universal planes' client capability causes the kernel to advertise
 * primary and cursor planes to us.
 */
struct output {
	struct device *device;

	/* A friendly name. */
	char *name;

	/*
	 * Should we render this output the next time we go through the
	 * event loop?
	 */
	bool needs_repaint;

	/*
	 * The plane -> CRTC -> connector chain we use.
	 *
	 * 0 is always an invalid ID for these objects.
	 */
	uint32_t primary_plane_id;
	uint32_t crtc_id;
	uint32_t connector_id;

	/* Supported format modifiers for XRGB8888. */
	uint64_t *modifiers;
	unsigned int num_modifiers;

	struct {
		struct drm_property_info plane[WDRM_PLANE__COUNT];
		struct drm_property_info crtc[WDRM_CRTC__COUNT];
		struct drm_property_info connector[WDRM_CONNECTOR__COUNT];
	} props;

	/*
	 * Mode reused from whatever was active when we started.
	 *
	 * 0 is always an invalid mode ID.
	 */
	uint32_t mode_blob_id;
	drmModeModeInfo mode;
	int64_t refresh_interval_nsec;

	/* Whether or not the output supports explicit fencing. */
	bool explicit_fencing;
	/* Fence FD for completion of the last atomic commit. */
	int commit_fence_fd;

	/* Buffers allocated by us. */
	struct buffer *buffers[BUFFER_QUEUE_DEPTH];

	/*
	 * The buffer we've just committed to KMS, waiting for it to send the
	 * atomic-complete event to tell us it's started displaying; set by
	 * repaint_one_output and cleared by atomic_event_handler.
	 */
	struct buffer *buffer_pending;

	/*
	 * The buffer currently being displayed by KMS, having been advanced
	 * from buffer_pending inside atomic_event_handler, then cleared by
	 * atomic_event_handler when the hardware starts displaying the next
	 * buffer.
	 */
	struct buffer *buffer_last;

	/*
	 * Time the last frame's commit completed from KMS, and when the
	 * next frame's commit is predicted to complete.
	 */
	struct timespec last_frame;
	struct timespec next_frame;

	/*
	 * The frame of the animation to display.
	 */
	unsigned int frame_num;

	struct {
		EGLConfig cfg;
		EGLContext ctx;
		GLuint gl_prog;
		GLuint pos_attr;
		GLuint col_uniform;
	} egl;
};

/*
 * A device is one KMS node from /dev/dri/ and its resources.
 *
 * The list of devices can be enumerated either by drmGetDevices2, or by
 * using the systemd-logind API, which allows unprivileged user processes to
 * access the KMS device as long as they are run on the active VT, as well
 * as offering crash resilience. For simplicity, we use the libdrm
 * enumeration.
 *
 * DRM devices come in three flavours: primary (aka 'card'), control, and
 * render nodes. Render nodes are used by GPUs which can allow unprivileged
 * user process to submit rendering and export buffers to another process.
 * Control nodes are useless and have been removed upstream. Primary nodes
 * are the ones we're interested in: these are the only ones which let us
 * actually drive KMS.
 *
 * For legacy reasons, non-KMS devices also have primary nodes. On an
 * NXP i.MX system, we have a card device for imx-drm (the KMS driver),
 * as well as both a card and a render device for etnaviv (the GPU driver),
 * even though we cannot use KMS through etnaviv. Hence we need to open
 * the primary devices and check that they are actually KMS devices.
 */
struct device {
	/* KMS device node. */
	int kms_fd;

	/* Queried at startup by
	 * drmModeGetResources / drmModeGetPlaneResources. */
	drmModeResPtr res;
	drmModePlanePtr *planes;
	int num_planes;

	/* Whether or not the device supports format modifiers. */
	bool fb_modifiers;

	/* The GBM device is our buffer allocator, and we create an EGL
	 * display from that to import buffers into. */
	struct gbm_device *gbm_device;
	EGLDisplay egl_dpy;

	/* Populated by us, to combine plane -> CRTC -> connector. */
	struct output **outputs;
	int num_outputs;

	/* /dev/tty* device. */
	int vt_fd;
	int saved_kb_mode; /* keyboard mode before we entered */
};

/*
 * Finds the first KMS-capable device in the system, probes its resources,
 * and sets up VT/TTY handling ready to display content.
 */
struct device *device_create(void);
bool device_egl_setup(struct device *device);
void device_destroy(struct device *device);

/*
 * Takes a target KMS connector, and returns a struct containing a complete
 * plane -> CRTC -> connector chain.
 */
struct output *output_create(struct device *device,
			     drmModeConnectorPtr connector);
bool output_egl_setup(struct output *output);
void output_egl_destroy(struct device *device, struct output *output);
void output_destroy(struct output *output);

/* Create and destroy framebuffers for a given output. */
struct buffer *buffer_create(struct device *device, struct output *output);
struct buffer *buffer_egl_create(struct device *device, struct output *output);
void buffer_destroy(struct buffer *buffer);
void buffer_egl_destroy(struct device *device, struct buffer *buffer);

/* Fill a buffer for a given animation step. */
void buffer_fill(struct buffer *buffer, int frame_num);
void buffer_egl_fill(struct buffer *buffer, int frame_num);

/*
 * Adds an output's state to an atomic request, setting it up to display a
 * given buffer.
 */
void output_add_atomic_req(struct output *output, drmModeAtomicReqPtr req,
			   struct buffer *buffer);

/*
 * Commits an atomic request to KMS. Upon completion, the KMS FD will become
 * readable with one event for every CRTC included in the request.
 *
 * allow_modeset should be set at startup or when substantially changing
 * configuration; it should not be set during steady-state operation, as it
 * allows the driver to perform operations which take longer than usual,
 * causing frames to be skipped.
 */
int atomic_commit(struct device *device, drmModeAtomicReqPtr req,
		  bool allow_modeset);

/*
 * Parse the very basic information from the EDID block, as described in
 * edid.c. The EDID parser could be fairly trivially extended to pull
 * more information, such as the mode.
 */
struct edid_info {
	char eisa_id[13];
	char monitor_name[13];
	char pnp_id[5];
	char serial_number[13];
};

struct edid_info *
edid_parse(const uint8_t *data, size_t length);

bool
gl_extension_supported(const char *haystack, const char *needle);

static void
fd_replace(int *target, int source)
{
	if (*target >= 0)
		close(*target);
	*target = source;
}

static void
fd_dup_into(int *target, int source)
{
	int duped = fcntl(source, F_DUPFD_CLOEXEC, 0);
	assert(duped >= 0);
	fd_replace(target, duped);
}