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

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "kms-skeleton.h"

/*
 * Using the CPU mapping, fill the buffer with a simple pixel-by-pixel
 * checkerboard; the boundaries advance from top-left to bottom-right.
 */
void buffer_fill(struct buffer *buffer, int frame_num)
{
	struct output *output = buffer->output;

	if (buffer->gbm.bo) {
		buffer_egl_fill(buffer, frame_num);
		return;
	}

	for (unsigned int y = 0; y < buffer->height; y++) {
		/*
		 * We play silly games with pointer types so we advance
		 * by (y*pitch) in bytes rather than in pixels, then cast back.
		 */
		uint8_t b;
		uint32_t *pix =
			(uint32_t *) ((uint8_t *) buffer->dumb.mem + (y * buffer->pitches[0]));
		if (y >= (buffer->height * output->frame_num) / NUM_ANIM_FRAMES)
			b = 0xff;
		else
			b = 0;

		for (unsigned int x = 0; x < buffer->width; x++) {
			uint32_t r;

			if (x >= (buffer->width * output->frame_num) / NUM_ANIM_FRAMES)
				r = 0xff;
			else
				r = 0;

			*pix++ = (0xff << 24 /* A */) | (r << 16) | \
				 (0x00 <<  8 /* G */) | b;
		}
	}
}

/*
 * Allocate a new framebuffer to display. Uses the output's current mode to
 * create a dumb buffer, mmaps the dumb buffer and fills it with pixels,
 * then creates a KMS framebuffer wrapping the dumb buffer.
 *
 * If you were using another kind of buffer (e.g. GBM or dmabuf), you could
 * skip everything up until the drmModeAddFB2 call here. Weston provides a
 * comprehensive example of multiple buffer types:
 *   https://gitlab.freedesktop.org/wayland/weston/tree/master/libweston/compositor-drm.c
 */
static struct buffer *buffer_dumb_create(struct device *device, struct output *output)
{
	struct buffer *ret = calloc(1, sizeof(*ret));
	struct drm_mode_create_dumb create;
	struct drm_mode_map_dumb map;
	struct drm_mode_destroy_dumb destroy;
	unsigned int m;
	int err;

	assert(ret);

	/*
	 * As we only create linear buffers here, make sure the plane supports
	 * the linear modifier.
	 */
	for (m = 0; m < output->num_modifiers; m++) {
		if (output->modifiers[m] == DRM_FORMAT_MOD_LINEAR)
			break;
	}
	assert(!device->fb_modifiers || m < output->num_modifiers);

	/*
	 * The create ioctl uses the combination of depth and bpp to infer
	 * a format; 24/32 refers to DRM_FORMAT_XRGB8888 as defined in
	 * the drm_fourcc.h header. These arguments are the same as given
	 * to drmModeAddFB, which has since been superseded by
	 * drmModeAddFB2 as the latter takes an explicit format token.
	 *
	 * We only specify these arguments; the driver calculates the
	 * pitch (also known as stride or row length) and total buffer size
	 * for us, also returning us the GEM handle.
	 *
	 * For more information on pixel formats, a very useful reference
	 * is the Pixel Format Guide to the Galaxy, which covers most of the
	 * pixel formats used across the low-level graphics stack:
	 *   https://afrantzis.com/pixel-format-guide/
	 */
	create = (struct drm_mode_create_dumb) {
		.width = output->mode.hdisplay,
		.height = output->mode.vdisplay,
		.bpp = 32,
	};
	err = drmIoctl(device->kms_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	if (err != 0) {
		fprintf(stderr, "failed to create %u x %u dumb buffer: %m\n",
			create.width, create.height);
		goto err;
	}
	assert(create.handle > 0);
	assert(create.pitch >= create.width * (create.bpp / 8));
	assert(create.size >= create.pitch * create.height);

	ret->output = output;
	ret->gem_handles[0] = create.handle;
	ret->format = DRM_FORMAT_XRGB8888;
	ret->modifier = DRM_FORMAT_MOD_LINEAR;
	ret->width = create.width;
	ret->height = create.height;
	ret->pitches[0] = create.pitch;
	ret->render_fence_fd = -1;
	ret->kms_fence_fd = -1;

	/*
	 * In order to map the buffer, we call an ioctl specific to the buffer
	 * type, which returns us a fake offset to use with the mmap syscall.
	 * mmap itself then works as you expect.
	 *
	 * Note this means it is not possible to map arbitrary offsets of
	 * buffers without specifically requesting it from the kernel.
	 */
	map = (struct drm_mode_map_dumb) {
		.handle = ret->gem_handles[0],
	};
	err = drmIoctl(device->kms_fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	if (err != 0) {
		fprintf(stderr, "failed to get %u x %u mmap offset: %m\n",
			ret->width, ret->height);
		goto err_dumb;
	}

	ret->dumb.mem = mmap(NULL, create.size, PROT_WRITE, MAP_SHARED,
			     device->kms_fd, map.offset);
	if (ret->dumb.mem == MAP_FAILED) {
		fprintf(stderr, "failed to mmap %u x %u dumb buffer: %m\n",
			ret->width, ret->height);
		goto err_dumb;
	}
	ret->dumb.size = create.size;

	return ret;

err_dumb:
	destroy = (struct drm_mode_destroy_dumb) { .handle = create.handle };
	drmIoctl(device->kms_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
err:
	free(ret);
	return NULL;
}

struct buffer *buffer_create(struct device *device, struct output *output)
{
	struct buffer *ret;
	uint64_t modifiers[4] = { 0, };
	int err;

	if (device->gbm_device)
		ret = buffer_egl_create(device, output);
	else
		ret = buffer_dumb_create(device, output);

	if (!ret)
		return NULL;

	for (int i = 0; ret->gem_handles[i]; i++) {
		modifiers[i] = ret->modifier;
		debug("[GEM:%" PRIu32 "]: %u x %u %s buffer (plane %d), pitch %u\n",
		      ret->gem_handles[i], ret->width, ret->height,
		      (ret->dumb.mem) ? "dumb" : "GBM",
		      i, ret->pitches[i]);
	}

	/*
	 * Wrap our GEM buffer in a KMS framebuffer, so we can then attach it
	 * to a plane.
	 *
	 * drmModeAddFB2 accepts multiple image planes (not to be confused with
	 * the KMS plane objects!), for images which have multiple buffers.
	 * For example, YUV images may have the luma (Y) components in a
	 * separate buffer to the chroma (UV) components.
	 *
	 * When using modifiers (which we do not for dumb buffers), we can also
	 * have multiple planes even for RGB images, as image compression often
	 * uses an auxiliary buffer to store compression metadata.
	 *
	 * Dumb buffers are always strictly single-planar, so we do not need
	 * the extra planes nor the offset field.
	 *
	 * AddFB2WithModifiers takes a list of modifiers per plane, however
	 * the kernel enforces that they must be the same for each plane
	 * which is there, and 0 for everything else.
	 */
	if (device->fb_modifiers) {
		err = drmModeAddFB2WithModifiers(device->kms_fd,
						 ret->width, ret->height,
						 ret->format,
						 ret->gem_handles,
						 ret->pitches,
						 ret->offsets,
						 modifiers, &ret->fb_id,
						 DRM_MODE_FB_MODIFIERS);
	} else {
		err = drmModeAddFB2(device->kms_fd, ret->width, ret->height,
			    	    ret->format, ret->gem_handles, ret->pitches,
				    ret->offsets, &ret->fb_id, 0);
	}

	if (err != 0 || ret->fb_id == 0) {
		fprintf(stderr, "failed AddFB2 on %u x %u %s (modifier 0x%" PRIx64 ") buffer: %m\n",
			ret->width, ret->height,
			(ret->dumb.mem) ? "dumb" : "GBM",
			ret->modifier);
		goto err;
	}
	return ret;

err:
	buffer_destroy(ret);
	return NULL;
}

void buffer_destroy(struct buffer *buffer)
{
	struct output *output = buffer->output;
	struct device *device = output->device;

	drmModeRmFB(device->kms_fd, buffer->fb_id);

	if (buffer->dumb.mem) {
		struct drm_mode_destroy_dumb destroy = {
			.handle = buffer->gem_handles[0],
		};

		munmap(buffer->dumb.mem, buffer->dumb.size);
		drmIoctl(device->kms_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	} else if (buffer->gbm.bo) {
		buffer_egl_destroy(device, buffer);
	}
	free(buffer);
}
