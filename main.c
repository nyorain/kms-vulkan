/*
 * kms-skeleton is an example of how to use the KMS API directly and set up
 * a basic timed render loop.
 *
 * It is to be a relatively straightforward learning/explanation utility,
 * with far more comments than features.
 *
 * Other more full-featured examples include:
 *   * kmscube (support for OpenGL ES rendering, GStreamer video decoding,
 *     explicit fencing)
 *   * kmscon (text consoles reimplemented on top of KMS)
 *   * Weston (overlay plane usage, much more reasoning about timing)
 *
 * This file contains the implementation of the main loop, primarily
 * concerned with scheduling and committing frame updates.
 */

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
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kms-skeleton.h"

/* Allow the driver to drift half a millisecond every frame. */
#define FRAME_TIMING_TOLERANCE (NSEC_PER_SEC / 2000)

static struct buffer *find_free_buffer(struct output *output)
{
	for (int i = 0; i < BUFFER_QUEUE_DEPTH; i++) {
		if (!output->buffers[i]->in_use)
			return output->buffers[i];
	}

	assert(0 && "could not find free buffer for output!");
}

/*
 * Informs us that an atomic commit has completed for the given CRTC.
 */
static void atomic_event_handler(int fd,
				 unsigned int sequence,
				 unsigned int tv_sec,
				 unsigned int tv_usec,
				 unsigned int crtc_id,
				 void *user_data)
{
	struct device *device = user_data;
	struct output *output = NULL;
	struct timespec completion = {
		.tv_sec = tv_sec,
		.tv_nsec = (tv_usec * 1000),
	};
	int64_t delta_nsec;

	for (int i = 0; i < device->num_outputs; i++) {
		if (device->outputs[i]->crtc_id == crtc_id) {
			output = device->outputs[i];
			break;
		}
	}

	if (!output) {
		debug("[CRTC:%u] received atomic completion for unknown CRTC",
		      crtc_id);
		return;
	}

	delta_nsec = timespec_sub_to_nsec(&completion, &output->next_frame);
	if (timespec_to_nsec(&output->last_frame) != 0 &&
	    llabs((long long) delta_nsec) > FRAME_TIMING_TOLERANCE) {
		debug("[%s] FRAME %" PRIi64 "ns %s: expected %" PRIu64 ", got %" PRIu64 "\n",
		      output->name,
		      delta_nsec,
		      (delta_nsec < 0) ? "EARLY" : "LATE",
		      timespec_to_nsec(&output->next_frame),
		      timespec_to_nsec(&completion));
	} else {
		debug("[%s] completed at %" PRIu64 " (delta %" PRIi64 "ns)\n",
		      output->name,
		      timespec_to_nsec(&completion),
		      delta_nsec);
	}

	output->needs_repaint = true;
	output->last_frame = completion;

	/*
	 * buffer_pending is the buffer we've just committed; this event tells
	 * us that buffer_pending is now being displayed, which means that
	 * buffer_last is no longer being displayed and we can reuse it.
	 */
	assert(output->buffer_pending);
	assert(output->buffer_pending->in_use);
	if (output->buffer_last) {
		assert(output->buffer_last->in_use);
		output->buffer_last->in_use = false;
		output->buffer_last = NULL;
	}
	output->buffer_last = output->buffer_pending;
	output->buffer_pending = NULL;
}

/*
 * Advance the output's frame counter, aiming to achieve linear animation
 * speed: if we miss a frame, try to catch up by dropping frames.
 */
static void advance_frame(struct output *output, struct timespec *now)
{
	struct timespec too_soon;

	/* For our first tick, we won't have predicted a time. */
	if (timespec_to_nsec(&output->last_frame) == 0L)
		return;

	/*
	 * Starting from our last frame completion time, advance the predicted
	 * completion for our next frame by one frame's refresh time, until we
	 * have at least a 4ms margin in which to paint a new buffer and submit
	 * our frame to KMS.
	 *
	 * This will skip frames in the animation if necessary, so it is
	 * temporally correct.
	 */
	timespec_add_msec(&too_soon, now, 4);
	output->next_frame = output->last_frame;

	while (timespec_sub_to_nsec(&too_soon, &output->next_frame) >= 0) {
		timespec_add_nsec(&output->next_frame, &output->next_frame,
				  output->refresh_interval_nsec);
		output->frame_num = (output->frame_num + 1) % NUM_ANIM_FRAMES;
	}
}

static void repaint_one_output(struct output *output, drmModeAtomicReqPtr req,
			       bool *needs_modeset)
{
	struct timespec now;
	struct buffer *buffer;
	int ret;

	ret = clock_gettime(CLOCK_MONOTONIC, &now);
	assert(ret == 0);

	buffer = find_free_buffer(output);
	assert(buffer);
	advance_frame(output, &now);

	buffer_fill(buffer, output->frame_num);
	output_add_atomic_req(output, req, buffer);
	buffer->in_use = true;
	output->buffer_pending = buffer;
	output->needs_repaint = false;

	/*
	 * If this output hasn't been painted before, then we need to set
	 * ALLOW_MODESET so we can get our first buffer on screen; if we
	 * have already presented to this output, then we don't need to since
	 * our configuration is similar enough.
	 */
	if (timespec_to_nsec(&output->last_frame) == 0UL)
		*needs_modeset = true;

	if (timespec_to_nsec(&output->next_frame) != 0UL) {
		debug("[%s] predicting presentation at %" PRIu64 " (%" PRIu64 "ns / %" PRIu64 "ms away)\n",
		      output->name,
		      timespec_to_nsec(&output->next_frame),
		      timespec_sub_to_nsec(&output->next_frame, &now),
		      timespec_sub_to_msec(&output->next_frame, &now));
	} else {
		debug("[%s] scheduling first frame\n", output->name);
	}
}

static bool shall_exit = false;

static void sighandler(int signo)
{
	if (signo == SIGINT)
		shall_exit = true;
	return;
}

int main(int argc, char *argv[])
{
	struct device *device;
	int ret = 0;

	struct sigaction sa;
	sa.sa_handler = sighandler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);

	/* Find a suitable KMS device, and set up our VT. */
	device = device_create();
	if (!device) {
		fprintf(stderr, "no usable KMS devices!\n");
		return 1;
	}

	/* Allocate framebuffers to display on all our outputs. */
	for (int i = 0; i < device->num_outputs; i++) {
		struct output *output = device->outputs[i];
		int j;

		if (device->gbm_device) {
			ret = output_egl_setup(output);
			if (!ret) {
				fprintf(stderr,
					"Couldn't set up EGL for output %s\n",
					output->name);
				ret = 2;
				goto out;
			}
		}

		for (j = 0; j < BUFFER_QUEUE_DEPTH; j++) {
			output->buffers[j] = buffer_create(device, output);
			if (!output->buffers[j]) {
				ret = 3;
				goto out;
			}
		}
	}

	while (!shall_exit) {
		drmModeAtomicReq *req;
		bool needs_modeset = false;
		int output_count = 0;
		int ret = 0;
		drmEventContext evctx = {
			.version = 3,
			.page_flip_handler2 = atomic_event_handler,
		};
		struct pollfd poll_fd = {
			.fd = device->kms_fd,
			.events = POLLIN,
		};

		req = drmModeAtomicAlloc();
		assert(req);

		/*
		 * See which of our outputs needs repainting, and repaint them
		 * if any.
		 */
		for (int i = 0; i < device->num_outputs; i++) {
			struct output *output = device->outputs[i];
			if (output->needs_repaint) {
				repaint_one_output(output, req, &needs_modeset);
				output_count++;
			}
		}

		if (output_count)
			ret = atomic_commit(device, req, needs_modeset);

		/*
		 * The out-fence FD from KMS signals when the commit we've just
		 * made becomes active, at the same time as the event handler
		 * will fire. We can use this to find when the _previous_
		 * buffer is free to reuse again.
		 */
		for (int i = 0; i < device->num_outputs; i++) {
			struct output *output = device->outputs[i];
			if (output->explicit_fencing && output->buffer_last) {
				fd_replace(&output->buffer_last->kms_fence_fd,
					   output->commit_fence_fd);
				output->commit_fence_fd = -1;

			}
		}

		drmModeAtomicFree(req);
		if (ret != 0) {
			fprintf(stderr, "atomic commit failed: %d\n", ret);
			break;
		}

		ret = poll(&poll_fd, 1, -1);
		if (ret == -1) {
			fprintf(stderr, "error polling KMS FD: %d\n", ret);
			break;
		}

		ret = drmHandleEvent(device->kms_fd, &evctx);
		if (ret == -1) {
			fprintf(stderr, "error reading KMS events: %d\n", ret);
			break;
		}
	}

out:
	device_destroy(device);
	fprintf(stdout, "good-bye\n");
	return ret;
}
