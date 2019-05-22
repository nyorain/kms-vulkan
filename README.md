# kms-quads: a simple KMS example

**NOTE** This code is a fork of [kms-quads](https://gitlab.freedesktop.org/daniels/kms-quads),
follow the link for more information.
It was developed by Daniel Stone, [Collabora](https://www.collabora.com)
and [DAQRI](https://www.daqri.com). This fork adds **experimental** vulkan support,
see the vulkan section below.

---

kms-quads is a simple and well-explained example of how to use the Linux
kernel's KMS API to drive graphical displays. It is built with the Meson build
system:
```shell
  $ meson build
  $ ninja -C build
```

When run from a text terminal with no arguments, it takes over the terminal and
displays a simple animation run independently on all currently-active displays:
```shell
  # ./build/kms-quads
```
Press ESC to quit the sample to go back to your terminal (requires
libinput/libudev).

kms-quads uses logind to switch the current TTY to graphical mode, and to
access KMS resources as an unprivileged user. In case logind can't be used,
kms-quads attempts to directly switch the TTY mode (requires root), for which a
specific TTY can be set through an environment variable, or as stdin:
```shell
  # TTYNO=4 ./build/kms-quads
  # ./build/kms-quads < /dev/tty4
```

During startup, kms-quads will iterate through all the available KMS resources,
create output chains for all available outputs, render an initial image, and
send an initial atomic modesetting request to show the initial image on all
outputs. After this, each output will independently run its own repaint loop
displaying a timed animation.

**NOTE: by default, kms-quads will run indefinietly without any method
of switching the vt. Unless you would like to be stuck with the program,
you probably rather want to run something like this (SIGINT is handled
by the program to correctly clean up):**
```shell
  # ./build/kms-quads & (sleep 10; pkill -INT kms-quads)
```

Afterwards you will be able to switch the vt. If the program crashes,
you might in bad cases also be stuck without any method (known to me)
of switching back to your original VT, requiring a restart.

## What is KMS?

The Linux kernel's graphical subsystem is the Direct Rendering Manager, or DRM
for short (unrelated to the other DRM!). DRM is the subsystem for both
rendering GPUs and display devices. Each of these devices will have one or
more DRM devices.

Each GPU driver has its own user/kernel API, as these devices behave quite
differently and there is no standard to be found.

However, display devices are similar enough that the Kernel Modesetting API, or
KMS for short, covers all display device support in the kernel. KMS exposes a
number of display components to userspace for it to control:

  * framebuffers is a set of pixels forming a single 2D image

  * planes takes a framebuffer and can optionally crop and scale it, or alter
    its colour management

  * CRTCs stack, blend, and combine the output of planes together (e.g. a
    cursor plane, on top of a UI plane, on top of a video plane), and generate
    a single logical pixel stream from the resulting image, at a defined
    resolution

  * connectors consume CRTC output and send it to to a physical display device,
    e.g. a HDMI monitor, or an LVDS panel: they can be queried for current
    display information such as the display's EDID information block, or
    whether or not the display is currently connected

kms-quads uses the KMS API to enumerate the currently-active devices, obtain a
list of all these resources, and construct a complete display output chain of
all these objects.

There are a number of presentations available on how KMS is built, including
[one from Boris
Brezillon](https://events.static.linuxfound.org/sites/events/files/slides/brezillon-drm-kms.pdf),
which is primarily focused on the kernel implementation.

## Vulkan

This fork adds vulkan support, showing how to use the `VK_EXT_image_drm_format_modifier`
extension to import gbm buffer objects into vulkan for rendering. With
this, you can theoretically build a vulkan kms app (like a wayland compositor)
that has no need for EGL or GL at all.
Most of the files (except vulkan.c and its shaders) are only
minimally changed to allow using vulkan. The application currently
tries to create a vulkan renderer but if some extensions are missing
or if the drm/kms driver does not support modifiers, it will fall
back the previously implemented EGL or dumb buffer rendering backend.
The EGL/dumb buffer renderer show simple moving colored quads (hence
the original project name) while the vulkan renderer currently shows a
smoothly animated color wheel. This way you can know which renderer
is used, but it will obviously also be logged.

Vulkan can only import dma buffer images if their format modifier is known.
It additionally needs a couple of extension. At the time of writing (May 2019),
AMD has no support for drm format modifiers at all, so vulkan importing won't
work on AMD hardware (*yet, hopefully*).
Support for the required `VK_EXT_image_drm_format_modifier`
extension is not merged into mesa upstream yet, but there exists a [merge
request](https://gitlab.freedesktop.org/mesa/mesa/merge_requests/515) for
anv, the intel vulkan driver. The application was tested and verified
to work with that implementation on an intel gpu.

**Important:** Theoretically, the `VK_EXT_queue_family_foreign` extension
is needed as well. The vulkan standard states that `VK_QUEUE_FAMILY_FOREIGN_EXT`
has to be used to transfer ownership of an image to a non-vulkan image
user (the drm subsystem). There is no (up-to-date) mesa patch for this extension
for any desktop vulkan driver though so we currently fall back to using
`VK_QUEUE_FAMILY_EXTERNAL`, but this **isn't guaranteed to work**! Once the
extension is supported on any driver, a patch will be trivial.

As you can see, the whole vulkan support for KMS is still rather experimental
and not widely supported. I hope to keep this application updated as more
drivers receive correct upstream support for all the required extensions.

## What is atomic modesetting?

Atomic modesetting is a relatively recent development of the KMS API to apply
and change state. The pre-atomic KMS interface (drmModeSetCrtc,
drmModePageFlip, drmModeSetPlane, drmModeObjectSetProperty) was not easily
extensible, and crucially lacked synchronisation between changing all the
different objects.

Atomic modesetting replaces these calls with a unified and extensible
property-based interface. Instead of issuing individual commands, userspace
creates an atomic request structure, which holds an arbitrary number of
property set commands. These can then be tested (to see if the proposed
configuration is valid), or committed to the hardware.

The available properties can be discovered whilst enumerating the available
resources.

kms-quads exclusively uses atomic modesetting, with no fallback to the
pre-atomic API.

There are a couple of presentations on the atomic API, including [one from
Daniel Vetter](https://www.youtube.com/watch?v=LjiB_JeDn2M).


## What are EGL and GBM?

OpenGL and OpenGL ES are rendering-only APIs. They only submit graphics data
and rendering commands to buffers which are supplied by an external interface.
EGL is generally used to do this; there are many good explanations of what EGL
is available online.

Unlike predecessors GLX (for X11) and WGL (Windows), EGL is independent of the
underlying display technology. It can be used on any number of platforms,
including Wayland, X11, and KMS.

To use EGL with KMS, we need a helper interface called GBM to bridge EGL's
more stateful interface with KMS. The `gbm_device` object corresponds to a KMS
device, from which you can create an `EGLDisplay`. You can select an
`EGLConfig` by matching the `EGL_NATIVE_VISUAL_ID` attribute with a GBM/KMS
format token, though take note that `EGL_NATIVE_VISUAL_ID` is ignored by
`eglChooseConfigs()`, so you must iterate the configs and do the matching by
hand.

There are two ways to use GBM to allocate buffers for EGL/GL.

The first is to simply create a `gbm_surface` (optionally passing the list of
acceptable modifiers supplied by KMS), create an `EGLSurface` from that, and
render as usual. Calling `eglSwapBuffers` will not post your new buffer to KMS,
however: in order to display it, you must call `gbm_surface_lock_front_buffer`,
which gives you a `gbm_bo` you can create a DRM framebuffer from. Once the
buffer is no longer in use by KMS, you can call `gbm_surface_release_buffer` to
give the buffer back to the implementation ready for reuse.

kms-quads uses an alternative approach: we explicitly allocate `gbm_bo`s
ourselves, import the BO (buffer object) to an `EGLImage`, bind the `EGLImage`
to a GL texture unit, then create a GL framebuffer object to render to that
texture. After ensuring the GL commands have been flushed, we create a DRM
framebuffer from the `gbm_bo` and display it ourselves.

There is little practical difference between the two approaches. Using a
`gbm_surface` is easier and requires less typing, however you cannot control the
number of buffers allocated by the GBM/EGL implementation. It also has some
lifetime challenges: destroying a `gbm_surface` will immediately destroy all
the `gbm_bo`s it allocated, with no way to keep them alive for longer.

Directly allocating `gbm_bo`s requires more typing, but offers more control.

There is one notable caveat when rendering into `gbm_bo`s via GL framebuffer
objects and displaying these buffers directly via KMS: GL framebuffer objects
are defined to have their origin in the lower-left corner. Scan-out, however,
happens from top to bottom. As a result, your buffer content might be displayed
upside-down. One solution to this problem is to apply a mirror transformation
around the Y axis in the vertex shader. Another one is to use the extension
`GL_MESA_framebuffer_flip_y` to provide the driver with a hint to flip the
coordinate system while writing into a framebuffer object. Both approaches are
implemented in this sample.

## Contact

The original code has been authored by [Collabora](https://www.collabora.com)
and [DAQRI](https://www.daqri.com).

Issues and merge requests are welcome on this project, and can be filed [in
freedesktops GitLab](https://gitlab.freedesktop.org/nyorain/kms-vulkan) or
[on Github](https://github.com/nyorain/kms-vulkan).
The original code (kms-quads) is hosted on [freedesktops
Gitlab](https://gitlab.freedesktop.org/daniels/kms-quads) and
welcomes all issues and merge requests as well.

As this project is hosted on freedesktop.org, it follows the [freedesktop.org
Code of Conduct](https://www.freedesktop.org/wiki/CodeOfConduct). Please be
kind and considerate.
