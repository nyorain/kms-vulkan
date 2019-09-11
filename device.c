/*
 * This file implements handling of DRM device nodes as well as setting
 * up the VT/TTY layer in order to let us do graphics in the first place.
 *
 * The preferred method to do this on modern systems is to use systemd's
 * logind, a D-Bus API which allows us access to privileged devices such as DRM
 * and input devices without being root. It also handles resetting the TTY for
 * us if we screw up and crash, which is really excellent. This sample borrows
 * some code from wlroots to use logind (see logind.c).
 *
 * In case logind can't be used, we fall back to direct VT handling, in which
 * case this sample needs to run as root.
 *
 * VT switching is currently unimplemented. An example of how to implement VT
 * switching is found in launcher-direct.c from Weston for direct use of VT,
 * and in the full logind.c sources of wlroots when going over logind.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <linux/kd.h>
#include <linux/major.h>
#include <linux/vt.h>

#include "kms-quads.h"

/*
 * Set up the VT/TTY so it runs in graphics mode and lets us handle our own
 * input. This uses the VT specified in $TTYNO if specified, or the current VT
 * if we're running directly from a VT, or failing that tries to find a free
 * one to switch to.
 */
static int vt_setup(struct device *device)
{
	const char *tty_num_env = getenv("TTYNO");
	int tty_num = 0;
	int cur_vt;
	char tty_dev[32];

	/* If $TTYNO is set in the environment, then use that first. */
	if (tty_num_env) {
		char *endptr = NULL;

		tty_num = strtoul(tty_num_env, &endptr, 10);
		if (tty_num == 0 || *endptr != '\0') {
			fprintf(stderr, "invalid $TTYNO environment variable\n");
			return -1;
		}
		snprintf(tty_dev, sizeof(tty_dev), "/dev/tty%d", tty_num);
	} else if (ttyname(STDIN_FILENO)) {
		/* Otherwise, if we're running from a VT ourselves, just reuse
		 * that. */
		ttyname_r(STDIN_FILENO, tty_dev, sizeof(tty_dev));
	} else {
		int tty0;

		/* Other-other-wise, look for a free VT we can use by querying
		 * /dev/tty0. */
		tty0 = open("/dev/tty0", O_WRONLY | O_CLOEXEC);
		if (tty0 < 0) {
			fprintf(stderr, "couldn't open /dev/tty0\n");
			return -errno;
		}

		if (ioctl(tty0, VT_OPENQRY, &tty_num) < 0 || tty_num < 0) {
			fprintf(stderr, "couldn't get free TTY\n");
			close(tty0);
			return -errno;
		}
		close(tty0);
		snprintf(tty_dev, sizeof(tty_dev), "/dev/tty%d", tty_num);
	}

	device->vt_fd = open(tty_dev, O_RDWR | O_NOCTTY);
	if (device->vt_fd < 0) {
		fprintf(stderr, "failed to open VT %d\n", tty_num);
		return -errno;
	}

	/* If we get our VT from stdin, work painfully backwards to find its
	 * VT number. */
	if (tty_num == 0) {
		struct stat buf;

		if (fstat(device->vt_fd, &buf) == -1 ||
		    major(buf.st_rdev) != TTY_MAJOR) {
			fprintf(stderr, "VT file %s is bad\n", tty_dev);
			return -1;
		}

		tty_num = minor(buf.st_rdev);
	}
	assert(tty_num != 0);

	printf("using VT %d\n", tty_num);

	/* Switch to the target VT. */
	if (ioctl(device->vt_fd, VT_ACTIVATE, tty_num) != 0 ||
	    ioctl(device->vt_fd, VT_WAITACTIVE, tty_num) != 0) {
		fprintf(stderr, "couldn't switch to VT %d\n", tty_num);
		return -errno;
	}

	debug("switched to VT %d\n", tty_num);

	/* Completely disable kernel keyboard processing: this prevents us
	 * from being killed on Ctrl-C. */
	if (ioctl(device->vt_fd, KDGKBMODE, &device->saved_kb_mode) != 0 ||
	    ioctl(device->vt_fd, KDSKBMODE, K_OFF) != 0) {
		fprintf(stderr, "failed to disable TTY keyboard processing\n");
		return -errno;
	}

	/* Change the VT into graphics mode, so the kernel no longer prints
	 * text out on top of us. */
	if (ioctl(device->vt_fd, KDSETMODE, KD_GRAPHICS) != 0) {
		fprintf(stderr, "failed to switch TTY to graphics mode\n");
		return -errno;
	}

	debug("VT setup complete\n");

	/* Normally we would also call VT_SETMODE to change the mode to
	 * VT_PROCESS here, which would allow us to intercept VT-switching
	 * requests and tear down KMS. But we don't, since that requires
	 * signal handling. */
	return 0;
}

static void vt_reset(struct device *device)
{
	ioctl(device->vt_fd, KDSKBMODE, device->saved_kb_mode);
	ioctl(device->vt_fd, KDSETMODE, KD_TEXT);
}

/*
 * Open a single KMS device, enumerate its resources, and attempt to find
 * usable outputs.
 */
static struct device *device_open(struct logind *session, const char *filename)
{
	struct device *ret = calloc(1, sizeof(*ret));
	const char *gbm_env;
	drmModePlaneResPtr plane_res;
	drm_magic_t magic;
	uint64_t cap;
	int err;

	assert(ret);

	/*
	 * Open the device and ensure we have support for universal planes and
	 * atomic modesetting.
	 */
	if (session)
		ret->kms_fd = logind_take_device(session, filename);
	else
		ret->kms_fd = open(filename, O_RDWR | O_CLOEXEC, 0);
	if (ret->kms_fd < 0) {
		fprintf(stderr, "warning: couldn't open %s: %s\n", filename,
			strerror(errno));
		goto err;
	}

	/*
	 * In order to drive KMS, we need to be 'master'. This should already
	 * have happened for us thanks to being root and the first client.
	 * There can only be one master at a time, so this will fail if
	 * (e.g.) trying to run this test whilst a graphical session is
	 * already active on the current VT.
	 */
	if (drmGetMagic(ret->kms_fd, &magic) != 0 ||
	    drmAuthMagic(ret->kms_fd, magic) != 0) {
		fprintf(stderr, "KMS device %s is not master\n", filename);
		goto err_fd;
	}

	err = drmSetClientCap(ret->kms_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	err |= drmSetClientCap(ret->kms_fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (err != 0) {
		fprintf(stderr, "no support for universal planes or atomic\n");
		goto err_fd;
	}

	err = drmGetCap(ret->kms_fd, DRM_CAP_ADDFB2_MODIFIERS, &cap);
	ret->fb_modifiers = (err == 0 && cap != 0);
	debug("device %s framebuffer modifiers\n",
	      (ret->fb_modifiers) ? "supports" : "does not support");

	/*
	 * The two 'resource' properties describe the KMS capabilities for
	 * this device.
	 */
	ret->res = drmModeGetResources(ret->kms_fd);
	if (!ret->res) {
		fprintf(stderr, "couldn't get card resources for %s\n",
			filename);
		goto err_fd;
	}

	plane_res = drmModeGetPlaneResources(ret->kms_fd);
	if (!plane_res) {
		fprintf(stderr, "device %s has no planes\n", filename);
		goto err_res;
	}

	if (ret->res->count_crtcs <= 0 || ret->res->count_connectors <= 0 ||
	    ret->res->count_encoders <= 0 || plane_res->count_planes <= 0) {
		printf("device %s is not a KMS device\n", filename);
		goto err_plane_res;
	}

	ret->planes = calloc(plane_res->count_planes, sizeof(*ret->planes));
	ret->num_planes = plane_res->count_planes;
	assert(ret->planes);
	for (unsigned int i = 0; i < plane_res->count_planes; i++) {
		ret->planes[i] = drmModeGetPlane(ret->kms_fd, plane_res->planes[i]);
		assert(ret->planes[i]);
	}

	ret->outputs = calloc(ret->res->count_connectors,
			      sizeof(*ret->outputs));
	assert(ret->outputs);

	/*
	 * Go through our connectors one by one and try to find a usable
	 * output chain. The comments in output_create() describe how we
	 * determine how to set up the output, and why we work backwards
	 * from a connector.
	 */
	for (int i = 0; i < ret->res->count_connectors; i++) {
		drmModeConnectorPtr connector =
			drmModeGetConnector(ret->kms_fd,
					    ret->res->connectors[i]);
		struct output *output = output_create(ret, connector);

		if (!output)
			continue;

		ret->outputs[ret->num_outputs++] = output;
	}

	if (ret->num_outputs == 0) {
		fprintf(stderr, "device %s has no active outputs\n", filename);
		goto err_outputs;
	}

	/*
	 * If using GPU rendering, create a GBM device to allocate buffers
	 * for us, then an EGLDisplay we can use to connect EGL to KMS.
	 *
	 * We don't create surfaces or contexts here; we'll do that later
	 * in per-output setup.
	 */
	if (!getenv("KMS_NO_GBM"))
		ret->gbm_device = gbm_create_device(ret->kms_fd);
	if (ret->gbm_device && !device_egl_setup(ret))
		goto err_gbm;

	printf("using device %s with %d outputs and %s rendering\n",
	       filename, ret->num_outputs,
	       (ret->gbm_device) ? "GPU" : "software");
	return ret;

err_gbm:
	gbm_device_destroy(ret->gbm_device);
err_outputs:
	free(ret->outputs);
	for (int i = 0; i < ret->num_planes; i++)
		drmModeFreePlane(ret->planes[i]);
	free(ret->planes);
err_plane_res:
	drmModeFreePlaneResources(plane_res);
err_res:
	drmModeFreeResources(ret->res);
err_fd:
	if (session)
		logind_release_device(session, ret->kms_fd);
	else
		close(ret->kms_fd);
err:
	free(ret);
	return NULL;
}

/*
 * Enumerate all KMS devices and find one we can use; also set up our TTY for
 * graphics mode if we find one.
 */
struct device *device_create(void)
{
	struct logind *session;
	struct device *ret;
	drmDevicePtr *devices;
	int num_devices;

#if defined(HAVE_LOGIND)
	session = logind_create();
	if (!session)
	{
		fprintf(stderr, "failed to create sesssion\n");
		goto err;
	}
#else
	session = NULL;
#endif

	num_devices = drmGetDevices2(0, NULL, 0);
	if (num_devices == 0) {
		fprintf(stderr, "no DRM devices available\n");
		goto err;
	}

	devices = calloc(num_devices, sizeof(*devices));
	assert(devices);
	num_devices = drmGetDevices2(0, devices, num_devices);
	printf("%d DRM devices available\n", num_devices);

	for (int i = 0; i < num_devices; i++) {
		drmDevicePtr candidate = devices[i];
		int fd;

		/*
		 * We need /dev/dri/cardN nodes for modesetting, not render
		 * nodes; render nodes are only used for GPU rendering, and
		 * control nodes are totally useless. Primary nodes are the
		 * only ones which let us control KMS.
		 */
		if (!(candidate->available_nodes & (1 << DRM_NODE_PRIMARY)))
			continue;

		ret = device_open(session, candidate->nodes[DRM_NODE_PRIMARY]);
		if (ret)
			break;
	}

	drmFreeDevices(devices, num_devices);
	if (!ret) {
		fprintf(stderr, "couldn't find any suitable KMS device\n");
		goto err;
	}

	if (!session && vt_setup(ret) != 0) {
		fprintf(stderr, "couldn't set up VT for graphics mode\n");
		goto err_dev;
	}

	ret->session = session;
	return ret;

err_dev:
	device_destroy(ret);
err:
	if (session)
	    logind_destroy(session);
	return NULL;
}

void device_destroy(struct device *device)
{
	struct output *output;

	for (int i = 0; i < device->num_outputs; i++)
		output_destroy(device->outputs[i]);
	free(device->outputs);

	if (device->gbm_device)
		gbm_device_destroy(device->gbm_device);

	if (device->session)
	{
		logind_release_device(device->session, device->kms_fd);
		logind_destroy(device->session);
	}
	else
	{
		close(device->kms_fd);
		vt_reset(device);
	}
	free(device);
}
