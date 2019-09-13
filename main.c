/*
 * kms-quads is an example of how to use the KMS API directly and set up
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
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/timerfd.h>

#include "kms-quads.h"

/* Allow the driver to drift half a millisecond every frame. */
#define FRAME_TIMING_TOLERANCE (NSEC_PER_SEC / 2000)

/* The amount of nanoseconds a repaint of an output is scheduled before its
 * next estimated KMS commit completion time. There is no guarantee we are
 * going to actually make it with this margin for every GPU out there. */
#define RENDER_LEEWAY_NSEC (NSEC_PER_MSEC * 5)

static struct buffer *find_free_buffer(struct output *output)
{
	for (int i = 0; i < BUFFER_QUEUE_DEPTH; i++) {
		if (!output->buffers[i]->in_use)
			return output->buffers[i];
	}

	assert(0 && "could not find free buffer for output!");
}

/*
 * Informs us that an atomic commit has completed for the given CRTC. This will
 * be called one for each output (identified by the crtc_id) for each commit.
 * We will be given the user_data parameter we passed to drmModeAtomicCommit
 * (which for us is just the device struct), as well as the frame sequence
 * counter as well as the actual time that our commit became active in hardware.
 *
 * This time is usually close to the start of the vblank period of the previous
 * frame, but depends on the driver.
 *
 * If the driver declares DRM_CAP_TIMESTAMP_MONOTONIC in its capabilities,
 * these times will be given as CLOCK_MONOTONIC values. If not (e.g. VMware),
 * all bets are off.
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

	/* Find the output this event is delivered for. */
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

	/*
	 * Compare the actual completion timestamp to what we had predicted it
	 * would be when we submitted it.
	 *
	 * This example simply screams into the logs if we hit a different
	 * time from what we had predicted. However, there are a number of
	 * things you could do to better deal with this: for example, if your
	 * frame is always late, you need to either start drawing earlier, or
	 * if that is not possible, halve your frame rate so you can draw
	 * steadily and predictably, if more slowly.
	 */
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

	output->last_frame = completion;

	/*
	 * buffer_pending is the buffer we've just committed; this event tells
	 * us that buffer_pending is now being displayed, which means that
	 * buffer_last is no longer being displayed and we can reuse it.
	 */
	assert(output->buffer_pending);
	assert(output->buffer_pending->in_use);

	if (output->explicit_fencing) {
		/*
		 * Print the time that the KMS fence FD signaled, i.e. when the
		 * last commit completed. It should be the same time as passed
		 * to this event handler in the function arguments.
		 */
		if (output->buffer_last &&
		    output->buffer_last->kms_fence_fd >= 0) {
			assert(linux_sync_file_is_valid(output->buffer_last->kms_fence_fd));
			debug("\tKMS fence time: %" PRIu64 "ns\n",
			      linux_sync_file_get_fence_time(output->buffer_last->kms_fence_fd));
		}

		if (device->gbm_device)
		{
			/*
			 * Print the time that the render fence FD signaled, i.e. when
			 * we finished writing to the buffer that we have now started
			 * displaying. It should be strictly before the KMS fence FD
			 * time.
			 */
			assert(linux_sync_file_is_valid(output->buffer_pending->render_fence_fd));
			debug("\trender fence time: %" PRIu64 "ns\n",
			      linux_sync_file_get_fence_time(output->buffer_pending->render_fence_fd));
		}
	}

	if (output->buffer_last) {
		assert(output->buffer_last->in_use);
		debug("\treleasing buffer with FB ID %" PRIu32 "\n", output->buffer_last->fb_id);
		output->buffer_last->in_use = false;
		output->buffer_last = NULL;
	}
	output->buffer_last = output->buffer_pending;
	output->buffer_pending = NULL;

	/* Next frame time is estimated to be completion time plus refresh
	 * interval. This timestamp is also used as the presentation time to drive
	 * the animation progress when repainting outputs.
	 */
	timespec_add_nsec(&output->next_frame, &completion, output->refresh_interval_nsec);

	debug("[%s] predicting presentation at %" PRIu64 " (%" PRIu64 "ns / %" PRIu64 "ms away)\n",
	      output->name, timespec_to_nsec(&output->next_frame),
	      timespec_sub_to_nsec(&output->next_frame, &completion),
	      timespec_sub_to_msec(&output->next_frame, &completion));

	/* If our driver supports MONOTONIC clock based timestamps, schedule the
	 * repaint to happen shortly before the next frame will be scanned out.  We
	 * are using timer_fd for that, and set its time relative to the next
	 * frame's presentation time. We are taking some leeway into account, so
	 * the frame rendering can actually make the deadline. This technique allows
	 * a frame to be rendered closely to its presentation time while minimizing
	 * latency. However, there's the risk that we won't meet the deadline if
	 * the given leeway is too short.
	 *
	 * If the driver doesn't support MONOTONIC timestamps, simply use an
	 * absolute time that is far in the past so the repaint event will be
	 * scheduled as soon as possible. */
	struct itimerspec t = { .it_interval = { 0, 0 }, .it_value = { 0, 1 } };
	if (device->monotonic_timestamps)
	{
		timespec_add_nsec(&t.it_value, &output->next_frame, -RENDER_LEEWAY_NSEC);
		debug("[%s] scheduling re-paint at %" PRIu64 " (%" PRIu64 "ns / %" PRIu64 "ms away)\n",
			  output->name, timespec_to_nsec(&t.it_value),
			  timespec_sub_to_nsec(&t.it_value, &completion),
			  timespec_sub_to_msec(&t.it_value, &completion));
	} else {
		debug("[%s] scheduling re-paint to be happen immediately\n",
				output->name);
	}

	int ret = timerfd_settime(output->repaint_timer_fd, TFD_TIMER_ABSTIME, &t, NULL);
	if (ret < 0)
		error("failed to set timerfd time: %s\n", strerror(errno));
}

static void repaint_one_output(struct output *output, drmModeAtomicReqPtr req,
			       const struct timespec *anim_start,
			       bool *needs_modeset)
{
	struct timespec now;
	struct buffer *buffer;
	float anim_progress = 0;

	/*
	 * Find a free buffer we can use to render into
	 */
	buffer = find_free_buffer(output);
	assert(buffer);

	if (timespec_to_nsec(&output->last_frame) == 0UL)
	{
		/*
		 * If this output hasn't been painted before, then we need to set
		 * ALLOW_MODESET so we can get our first buffer on screen; if we
		 * have already presented to this output, then we don't need to since
		 * our configuration is similar enough.
		 */
		*needs_modeset = true;
		debug("[%s] scheduling first frame\n", output->name);
	} else {
		/*
		 * Use the next frame's presentation time to determine
		 * the progress of the animation loop, and then render the content for
		 * that position. Since this calculation is based on absolute timing
		 * it will naturally catch up with dropped frames.
		 */
		int64_t abs_delta_nsec = timespec_sub_to_nsec(&output->next_frame, anim_start);
		int64_t rel_delta_nsec = abs_delta_nsec % ANIMATION_LOOP_DURATION_NSEC;
		anim_progress = (float)rel_delta_nsec / ANIMATION_LOOP_DURATION_NSEC;
	}

	buffer_fill(buffer, anim_progress);

	/* Add the output's new state to the atomic modesetting request. */
	output_add_atomic_req(output, req, buffer);
	buffer->in_use = true;
	output->buffer_pending = buffer;
	output->needs_repaint = false;

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
	struct input *input;
	int ret = 0;
	struct timespec anim_start;

	struct sigaction sa;
	sa.sa_handler = sighandler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);

	/*
	 * Find a suitable KMS device, and set up our VT.
	 * This will create outputs for every currently-enabled connector.
	 */
	device = device_create();
	if (!device) {
		fprintf(stderr, "no usable KMS devices!\n");
		return 1;
	}
#if defined(HAVE_INPUT)
	input = input_create(device->session);
	if (!input) {
		fprintf(stderr, "failed to create input\n");
		return 1;
	}
#else
	input = NULL;
#endif

	/* we poll each output and the KMS master FD */
	int num_poll_fds = device->num_outputs + 1;
	struct pollfd * poll_fds = calloc(num_poll_fds, sizeof(*poll_fds));

	/*
	 * Allocate framebuffers to display on all our outputs.
	 *
	 * It is possible to use an EGLSurface here, but we explicitly allocate
	 * buffers ourselves so we can manage the queue depth.
	 */
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

		/* each output has an individual timer to shedule repainting. We are
		 * polling each one in our main loop
		 */
		poll_fds[i] = (struct pollfd) {
			.fd = output->repaint_timer_fd,
			.events = POLLIN,
		};
	}

	poll_fds[device->num_outputs] = (struct pollfd) {
		.fd = device->kms_fd,
		.events = POLLIN,
	};

	drmEventContext evctx = {
		.version = 3,
		.page_flip_handler2 = atomic_event_handler,
	};

	ret = clock_gettime(CLOCK_MONOTONIC, &anim_start);
	if (ret < 0)
	{
		ret = 4;
		goto out;
	}

	/* Our main rendering loop, which we spin forever. */
	while (!shall_exit) {
		drmModeAtomicReq *req;
		bool needs_modeset = false;
		int output_count = 0;
		int ret = 0;

		/*
		 * Allocate an atomic-modesetting request structure for any
		 * work we will do in this loop iteration.
		 *
		 * Atomic modesetting allows us to group together KMS requests
		 * for multiple outputs, so this request may contain more than
		 * one output's repaint data.
		 */
		req = drmModeAtomicAlloc();
		assert(req);

		/*
		 * See which of our outputs needs repainting, and repaint them
		 * if any.
		 *
		 * On our first run through the loop, all our outputs will
		 * need repainting, so the request will contain the state for
		 * all the outputs, submitted together.
		 *
		 * This is good since it gives the driver a complete overview
		 * of any hardware changes it would need to perform to reach
		 * the target state.
		 */
		for (int i = 0; i < device->num_outputs; i++) {
			struct output *output = device->outputs[i];
			if (output->needs_repaint) {
				/*
				 * Add this output's new state to the atomic
				 * request.
				 */
				repaint_one_output(output, req, &anim_start, &needs_modeset);
				output_count++;
			}
		}

		/*
		 * Committing the atomic request to KMS makes the configuration
		 * current. As we request non-blocking mode, this function will
		 * return immediately, and send us events through the DRM FD
		 * when the request has actually completed. Even if we group
		 * updates for multiple outputs into a single request, the
		 * kernel will send one completion event for each output.
		 *
		 * Hence, after the first repaint, each output effectively
		 * runs its own repaint loop. This allows us to work with
		 * outputs running at different frequencies, or running out of
		 * phase from each other, without dropping our frame rate to
		 * the lowest common denominator.
		 *
		 * It does mean that we need to paint the buffers for
		 * each output individually, rather than having a single buffer
		 * with the content for every output.
		 */
		if (output_count)
			ret = atomic_commit(device, req, needs_modeset);
		drmModeAtomicFree(req);
		if (ret != 0) {
			fprintf(stderr, "atomic commit failed: %d\n", ret);
			break;
		}

		/*
		 * The out-fence FD from KMS signals when the commit we've just
		 * made becomes active, at the same time as the event handler
		 * will fire. We can use this to find when the _previous_
		 * buffer is free to reuse again.
		 */
		for (int i = 0; i < device->num_outputs; i++) {
			struct output *output = device->outputs[i];
			if (output->explicit_fencing && output->commit_fence_fd >= 0 &&
			    output->buffer_last) {
				assert(linux_sync_file_is_valid(output->commit_fence_fd));
				fd_replace(&output->buffer_last->kms_fence_fd,
					   output->commit_fence_fd);
				output->commit_fence_fd = -1;
			}
		}

		/*
		 * Now we have (maybe) repainted some outputs, we go to sleep waiting
		 * for either output repaint events or completion events from KMS. As
		 * each output completes, we will receive one event per output (making
		 * the DRM FD be readable and waking us from poll), which we then
		 * dispatch through drmHandleEvent into our callback.
		 */
		ret = poll(poll_fds, num_poll_fds, -1);
		if (ret == -1) {
			fprintf(stderr, "error polling FDs: %d\n", ret);
			break;
		}

		for (int i = 0; i < num_poll_fds; ++i) {
			if (!(poll_fds[i].revents & POLLIN))
				continue;

			if (i < device->num_outputs) {
				/* For any output repaint event, we mark its output to get
				 * repainted. The repainting will happen at the top of our main
				 * loop, immediately after all events from FDs have been
				 * handled.
				 */
				device->outputs[i]->needs_repaint = true;

				/* disarm timer_fd, so subsequent polls won't wake up anymore
				 * until a new wakeup time is set*/
				static const struct itimerspec its = {
					.it_interval = {0, 0},
					.it_value = {0, 0}
				};
				int ret = timerfd_settime(device->outputs[i]->repaint_timer_fd,
							  TFD_TIMER_ABSTIME, &its, NULL);
				if (ret < 0)
				{
					error("failed set timerfd time: %s\n", strerror(errno));
					ret = -1;
					goto out;
				}
			} else {
				/* The KMS master FD was signaled. Handle any pending DRM events */
				ret = drmHandleEvent(device->kms_fd, &evctx);
				if (ret == -1) {
					fprintf(stderr, "error reading KMS events: %d\n", ret);
					break;
				}
			}
		}

		if (input)
			shall_exit = input_was_ESC_key_pressed(input);
	}

out:
	if (input)
	    input_destroy(input);
	device_destroy(device);
	fprintf(stdout, "good-bye\n");
	return ret;
}
