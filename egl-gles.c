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

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>

#include "kms-quads.h"

#include <drm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#if defined(HAVE_GL_CORE)
#include <GL/gl.h>
#include <GL/glext.h>
#define _APIENTRYP APIENTRYP
#else
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
/*
 * GLES2 exts is needed for prototypes of GL_OES_EGL_image
 * (i.e. glEGLImageTargetTexture2DOES etc.)
 */
#include <GLES2/gl2ext.h>
#define _APIENTRYP GL_APIENTRYP
#endif

/* Add glFramebufferParameteri signature manually if needed. Actual context
 * might support higher vGL/GLES version than exposed through headers */
#if !(GL_VERSION_4_3 == 1) || !(GL_ES_VERSION_3_1 == 1)
typedef void (_APIENTRYP PFNGLFRAMEBUFFERPARAMETERIPROC) (GLenum target, GLenum pname, GLint param);
#endif

#if !defined(GL_FRAMEBUFFER_FLIP_Y_MESA)
/* see https://www.khronos.org/registry/OpenGL/extensions/MESA/MESA_framebuffer_flip_y.txt */
#define GL_FRAMEBUFFER_FLIP_Y_MESA 0x8BBB
#endif

bool
gl_extension_supported(const char *haystack, const char *needle)
{
	size_t extlen = strlen(needle);
	const char *end = haystack + strlen(haystack);

	while (haystack < end) {
		size_t n = 0;

		/* Skip whitespaces, if any */
		if (*haystack == ' ') {
			haystack++;
			continue;
		}

		n = strcspn(haystack, " ");

		/* Compare strings */
		if (n == extlen && strncmp(needle, haystack, n) == 0)
			return true; /* Found */

		haystack += n;
	}

	/* Not found */
	return false;
}

/* Create a dmabuf FD from a GEM handle. */
static int handle_to_fd(struct device *device, uint32_t gem_handle)
{
	struct drm_prime_handle prime = {
		.handle = gem_handle,
		.flags = DRM_RDWR | DRM_CLOEXEC,
	};
	int ret;

	ret = ioctl(device->kms_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
	if (ret != 0) {
		error("failed to export GEM handle %" PRIu32 " to FD\n", gem_handle);
		return -1;
	}

	return prime.fd;
}

/* Create an EGLDisplay for a device. */
bool device_egl_setup(struct device *device)
{
	/* Client extensions, supported without a display. */
	const char *exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

	/*
	 * If we have newer EGL which supports eglGetPlatformDisplay, use that
	 * and explictly pass a platform token in. If we're on an ancient EGL
	 * implementation, just use eglGetDisplay instead, and hope it detects
	 * the display type correctly.
	 */
	if (exts &&
	    (gl_extension_supported(exts, "EGL_KHR_platform_gbm") ||
	     gl_extension_supported(exts, "EGL_MESA_platform_gbm"))) {
		PFNEGLGETPLATFORMDISPLAYEXTPROC get_dpy =
			(PFNEGLGETPLATFORMDISPLAYEXTPROC)
			eglGetProcAddress("eglGetPlatformDisplayEXT");
		device->egl_dpy =
			get_dpy(EGL_PLATFORM_GBM_KHR, device->gbm_device, NULL);
	} else {
		device->egl_dpy = eglGetDisplay(device->gbm_device);
	}

	if (!device->egl_dpy) {
		error("couldn't create EGLDisplay from GBM device\n");
		return false;
	}

	if (!eglInitialize(device->egl_dpy, NULL, NULL)) {
		error("couldn't initialise EGL display\n");
		return false;
	}

	exts = eglQueryString(device->egl_dpy, EGL_EXTENSIONS);
	assert(exts);

	/*
	 * We require dmabuf import to operate at all, since we allocate our
	 * buffers independently and import them. We require both of the KMS
	 * and EGL stacks to support modifiers in order to use them, but not
	 * having them is not fatal.
	 */
	if (!gl_extension_supported(exts, "EGL_EXT_image_dma_buf_import")) {
		error("EGL dmabuf import not supported\n");
		return false;
	}

	device->fb_modifiers &=
		gl_extension_supported(exts, "EGL_EXT_image_dma_buf_import_modifiers");
	debug("%susing format modifiers\n",
	      (device->fb_modifiers) ? "" : "not ");

	/*
	 * At the cost of wasted allocations, we could avoid the need for
	 * surfaceless_context by allocating a scratch gbm_surface which
	 * we never use, apart from having it around to make the context
	 * current. But that is not implemented here.
	 */
	if (!gl_extension_supported(exts, "EGL_KHR_surfaceless_context")) {
		error("EGL surfaceless context not supported");
		return false;
	}

	return true;
}

EGLConfig
egl_find_config(struct output *output)
{
	struct device *device = output->device;
	EGLConfig ret = EGL_NO_CONFIG_KHR;
	EGLConfig *configs;
	EGLint num_cfg;
	EGLBoolean err;

	/*
	 * In order to render correctly, we need to find an EGLConfig which
	 * actually corresponds to our DRM format config, else we'll get
	 * channel size/order mismatches. GBM implements this by providing
	 * the DRM format in the EGL_NATIVE_VISUAL_ID field of the configs.
	 *
	 * Unfortunately, we can't just do this the 'normal' way by passing
	 * EGL_NATIVE_VISUAL_ID as a constraint in the attribs field of
	 * eglChooseConfig, because eglChooseConfig is specified to ignore
	 * that field, and not generate an error if you pass it.
	 *
	 * Instead, we loop over every available config and query its
	 * NATIVE_VISUAL_ID until we find one.
	 */
	err = eglGetConfigs(device->egl_dpy, NULL, 0, &num_cfg);
	assert(err);
	configs = malloc(num_cfg * sizeof(*configs));
	assert(configs);
	err = eglGetConfigs(device->egl_dpy, configs, num_cfg, &num_cfg);
	assert(err);

	for (EGLint c = 0; c < num_cfg; c++) {
		EGLint visual;
		err = eglGetConfigAttrib(device->egl_dpy, configs[c],
					 EGL_NATIVE_VISUAL_ID, &visual);
		assert(err);
		if (visual == DRM_FORMAT_XRGB8888) {
			ret = configs[c];
			break;
		}
	}

	free(configs);

	if (!ret) {
		error("no EGL config for format 0x%" PRIx32 "\n",
		      DRM_FORMAT_XRGB8888);
	}

	return ret;
}

EGLContext
egl_create_context(struct output *output)
{
	struct device *device = output->device;
	const char *exts = eglQueryString(device->egl_dpy, EGL_EXTENSIONS);
	EGLBoolean err;
	EGLContext ret;
	EGLint nattribs = 2;
	EGLint attribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, 3,
		EGL_NONE,                  EGL_NONE,
		EGL_NONE,                  EGL_NONE,
		EGL_NONE,                  EGL_NONE,
		EGL_NONE,                  EGL_NONE
	};
	EGLint *attrib_version = &attribs[1];

	/* OpenGL Vertex Array Objects are available in GLES3 and required in
	 * GLCore */
	output->egl.use_vao = true;

	if (getenv("GL_CORE"))
	{
	    output->egl.gl_core = true;

	    attribs[nattribs++] = EGL_CONTEXT_MINOR_VERSION;
	    attribs[nattribs++] = 3;
	    attribs[nattribs++] = EGL_CONTEXT_OPENGL_PROFILE_MASK;
	    attribs[nattribs++] = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT;

	    err = eglBindAPI(EGL_OPENGL_API);
	    assert(err);
	} else {
	    err = eglBindAPI(EGL_OPENGL_ES_API);
	    assert(err);
	}

	/*
	 * Try to give ourselves a high-priority context if we can; this may or
	 * may not work, depending on the platform.
	 */
	if (gl_extension_supported(exts, "EGL_IMG_context_priority")) {
		attribs[nattribs++] = EGL_CONTEXT_PRIORITY_LEVEL_IMG;
		attribs[nattribs++] = EGL_CONTEXT_PRIORITY_HIGH_IMG;

		ret = eglCreateContext(device->egl_dpy, output->egl.cfg,
				       EGL_NO_CONTEXT, attribs);
		if (ret)
			return ret;

		/* Fall back if we can't create a high-priority context. */
		attribs[--nattribs] = EGL_NONE;
		attribs[--nattribs] = EGL_NONE;
		debug("couldn't create high-priority EGL context, falling back\n");
	}

	ret = eglCreateContext(device->egl_dpy, output->egl.cfg,
			       EGL_NO_CONTEXT, attribs);
	if (ret)
		return ret;

	if (!output->egl.gl_core) {
		debug("couldn't create GLES3 context, falling back\n");
		/* GLES2 doesn't have VAO support, and some drivers
		 * don't handle even VBOs very well */
		*attrib_version = 2;
		/* As a last-ditch attempt, try an ES2 context. */
		ret = eglCreateContext(device->egl_dpy, output->egl.cfg,
				       EGL_NO_CONTEXT, attribs);
		if (ret)
			return ret;

		output->egl.use_vao = false;
	}

	error("couldn't create any EGL context!\n");
	return EGL_NO_CONTEXT;
}

/* The following is boring boilerplate GL to draw four quads. */
static const char *vert_shader_text_gles =
	"precision highp float;\n"
	"attribute vec2 in_pos;\n"
	"uniform mat4 u_proj;\n"
	"void main() {\n"
	"  gl_Position = u_proj * vec4(in_pos, 0.0, 1.0);\n"
	"}\n";

static const char *frag_shader_text_gles =
	"precision highp float;\n"
	"uniform vec4 u_col;\n"
	"void main() {\n"
	"  gl_FragColor = u_col;\n"
	"}\n";

static const char *vert_shader_text_glcore =
	"#version 330 core\n"
	"in vec2 in_pos;\n"
	"uniform mat4 u_proj;\n"
	"void main() {\n"
	"  gl_Position = u_proj * vec4(in_pos, 0.0, 1.0);\n"
	"}\n";

static const char *frag_shader_text_glcore =
	"#version 330 core\n"
	"uniform vec4 u_col;\n"
	"out vec4 out_color;\n"
	"void main() {\n"
	"  out_color = u_col;\n"
	"}\n";

static GLuint
create_shader(GLuint program, const char *source, GLenum shader_type)
{
        GLuint shader;
        GLint status;

        shader = glCreateShader(shader_type);
        assert(shader != 0);

        glShaderSource(shader, 1, (const char **) &source, NULL);
        glCompileShader(shader);

        glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
        if (!status) {
                char log[1000];
                GLsizei len;
                glGetShaderInfoLog(shader, 1000, &len, log);
                fprintf(stderr, "Error: compiling %s: %*s\n",
                        shader_type == GL_VERTEX_SHADER ? "vertex" : "fragment",
                        len, log);
                return EGL_FALSE;
        }

	glAttachShader(program, shader);
	glDeleteShader(shader);

        return EGL_TRUE;
}

bool
output_egl_setup(struct output *output)
{
	struct device *device = output->device;
	const char *exts = eglQueryString(device->egl_dpy, EGL_EXTENSIONS);
	EGLBoolean ret;
	GLint status;
	GLfloat proj[] = {
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};

	/*
	 * Explicit fencing support requires us to be able to export EGLSync
	 * objects to dma_fence FDs (to give to KMS, so it can wait on GPU
	 * rendering completion), and to be able to import dma_fence FDs to
	 * EGLSync objects to give to GL, so it can wait on KMS completion.
	 */
	output->explicit_fencing &=
		(gl_extension_supported(exts, "EGL_KHR_fence_sync") &&
		 gl_extension_supported(exts, "EGL_KHR_wait_sync") &&
		 gl_extension_supported(exts, "EGL_ANDROID_native_fence_sync"));
	debug("%susing explicit fencing\n",
	      (output->explicit_fencing) ? "" : "not ");

	output->egl.cfg = egl_find_config(output);
	if (!output->egl.cfg)
		return false;

	output->egl.ctx = egl_create_context(output);
	if (output->egl.ctx == EGL_NO_CONTEXT)
		return false;

	ret = eglMakeCurrent(output->device->egl_dpy,
			     EGL_NO_SURFACE, EGL_NO_SURFACE,
			     output->egl.ctx);
	assert(ret);

	/* glGetString on GL Core with GL_EXTENSIONS is an error,
	 * so only do that if not using GL Core */
	if (!output->egl.gl_core)
	{
		exts = (const char *) glGetString(GL_EXTENSIONS);

		if (!gl_extension_supported(exts, "GL_OES_EGL_image")) {
			error("GL_OES_EGL_image not supported\n");
			goto out_ctx;
		}

		if (output->explicit_fencing &&
			!gl_extension_supported(exts, "GL_OES_EGL_sync")) {
			error("GL_OES_EGL_sync not supported\n");
			goto out_ctx;
		}

		output->egl.have_gl_mesa_framebuffer_flip_y =
			gl_extension_supported(exts, "GL_MESA_framebuffer_flip_y");

	} else {
		const GLubyte *ext;
		bool found_image = false;
		bool found_sync = false;
		int num_exts = 0;

		glGetIntegerv(GL_NUM_EXTENSIONS, &num_exts);

		for (int i = 0; i < num_exts; i++) {
			ext = glGetStringi(GL_EXTENSIONS, i);
			if (strcmp((const char *) ext, "GL_OES_EGL_image") == 0)
				found_image = true;
			else if (strcmp((const char *) ext, "GL_EXT_EGL_sync") == 0)
				found_sync = true;
			else if (strcmp((const char *) ext, "GL_MESA_framebuffer_flip_y") == 0)
				output->egl.have_gl_mesa_framebuffer_flip_y = true;
			else if (strcmp((const char *) ext, "GL_OES_vertex_array_object") == 0)
				output->egl.use_vao = true;
		}

		if (!found_image) {
			error("GL_OES_EGL_image not supported\n");
			goto out_ctx;
		}

		if (output->explicit_fencing && !found_sync) {
			error("GL_EXT_EGL_sync not supported\n");
			goto out_ctx;
		}
	}

	printf("using GL setup: \n"
		"   renderer '%s'\n"
		"   vendor '%s'\n"
		"   GL version '%s'\n"
		"   GLSL version '%s'\n",
		glGetString(GL_RENDERER), glGetString(GL_VENDOR),
		glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));

	output->egl.gl_prog = glCreateProgram();
	ret = create_shader(output->egl.gl_prog,
		output->egl.gl_core ? vert_shader_text_glcore : vert_shader_text_gles,
		GL_VERTEX_SHADER);
	assert(ret);
	ret = create_shader(output->egl.gl_prog,
		output->egl.gl_core ? frag_shader_text_glcore : frag_shader_text_gles,
		GL_FRAGMENT_SHADER);
	assert(ret);

	output->egl.pos_attr = 0;
	glBindAttribLocation(output->egl.gl_prog, output->egl.pos_attr, "in_pos");

	glLinkProgram(output->egl.gl_prog);
	glGetProgramiv(output->egl.gl_prog, GL_LINK_STATUS, &status);
	if (!status) {
		char log[1000];
		GLsizei len;
		glGetProgramInfoLog(output->egl.gl_prog, 1000, &len, log);
		error("Error: linking GLSL program: %*s\n", len, log);
		goto err_program;
	}
	assert(status);

	output->egl.col_uniform = glGetUniformLocation(output->egl.gl_prog, "u_col");
	output->egl.proj_uniform = glGetUniformLocation(output->egl.gl_prog, "u_proj");

	glUseProgram(output->egl.gl_prog);

	/* If we can't flip Y through GL_MESA_framebuffer_flip_y,
	 * apply a mirror transformation directly in the shader's
	 * projection matrix */
	if (!output->egl.have_gl_mesa_framebuffer_flip_y)
	{
		/* flip sign of row=1, col=1 to flip Y */
		proj[5] *= -1;
	}
	glUniformMatrix4fv(output->egl.proj_uniform, 1, false, proj);

	glGenBuffers(1, &output->egl.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, output->egl.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 8, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	if (output->egl.use_vao) {
		glGenVertexArrays(1, &output->egl.vao);
		glBindVertexArray(output->egl.vao);

		glBindBuffer(GL_ARRAY_BUFFER, output->egl.vbo);
		glVertexAttribPointer(output->egl.pos_attr, 2, GL_FLOAT, GL_FALSE, 0, (char *)(NULL));
		glEnableVertexAttribArray(output->egl.pos_attr);

		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindVertexArray(0);
	}

	return true;
err_program:
	glDeleteProgram(output->egl.gl_prog);
out_ctx:
	eglDestroyContext(output->device->egl_dpy, output->egl.ctx);
	return false;
}

void
output_egl_destroy(struct device* device, struct output *output)
{
	EGLBoolean ret;

	ret = eglMakeCurrent(device->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
			     output->egl.ctx);
	assert(ret);

	if (output->egl.use_vao)
		glDeleteVertexArrays(1, &output->egl.vao);
	glDeleteBuffers(1, &output->egl.vbo);
	glDeleteProgram(output->egl.gl_prog);
	eglDestroyContext(output->device->egl_dpy, output->egl.ctx);
}

/*
 * Allocates a buffer and makes it usable for rendering with EGL/GL. We achieve
 * this by allocating each individual buffer with GBM, importing it into EGL
 * as an EGLImage, binding the EGLImage to a texture unit, then finally creating
 * a FBO from that texture unit so we can render into it.
 */
struct buffer *buffer_egl_create(struct device *device, struct output *output)
{
	struct buffer *ret = calloc(1, sizeof(*ret));
	static PFNEGLCREATEIMAGEKHRPROC create_img = NULL;
	static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target_tex_2d = NULL;
	static PFNGLFRAMEBUFFERPARAMETERIPROC framebuffer_parameteri = NULL;
	EGLint attribs[64] = { 0, }; /* see note below about type */
	EGLint nattribs = 0;
	EGLBoolean err;
	int num_planes;
	int dma_buf_fds[4] = { -1, -1, -1, -1 };

	assert(ret);

	ret->output = output;
	ret->render_fence_fd = -1;
	ret->kms_fence_fd = -1;

	/*
	 * We have the list of the acceptable modifiers for KMS, which we just
	 * pass straight to GBM. GBM takes this list of modifiers, and picks
	 * the 'best' modifier according to whatever internal preference the
	 * driver wants to use. We can then query the GBM BO to find out which
	 * modifier it selected.
	 */
	if (device->fb_modifiers) {
		ret->gbm.bo = gbm_bo_create_with_modifiers(device->gbm_device,
							   output->mode.hdisplay,
							   output->mode.vdisplay,
							   DRM_FORMAT_XRGB8888,
							   output->modifiers,
							   output->num_modifiers);
	}
	if (!ret->gbm.bo) {
		/*
		 * Fall back to the non-modifier path if we can't create a
		 * buffer with modifiers.
		 */
		device->fb_modifiers = false;
		ret->gbm.bo = gbm_bo_create(device->gbm_device,
					    output->mode.hdisplay,
					    output->mode.vdisplay,
					    DRM_FORMAT_XRGB8888,
					    GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
	}

	if (!ret->gbm.bo) {
		error("failed to create %u x %u BO\n",
		      output->mode.hdisplay, output->mode.vdisplay);
		goto err;
	}

	/*
	 * We can query all the image properties from the GBM BO once we've
	 * created it.
	 */
	ret->format = DRM_FORMAT_XRGB8888;
	ret->width = output->mode.hdisplay;
	ret->height = output->mode.vdisplay;
	ret->modifier = gbm_bo_get_modifier(ret->gbm.bo);
	num_planes = gbm_bo_get_plane_count(ret->gbm.bo);
	for (int i = 0; i < num_planes; i++) {
		union gbm_bo_handle h;

		/* In hindsight, we got this API wrong. */
		h = gbm_bo_get_handle_for_plane(ret->gbm.bo, i);
		if (h.u32 == 0 || h.s32 == -1) {
			error("failed to get handle for BO plane %d (modifier 0x%" PRIx64 ")\n",
			      i, ret->modifier);
			goto err_bo;
		}
		ret->gem_handles[i] = h.u32;

		dma_buf_fds[i] = handle_to_fd(device, ret->gem_handles[i]);
		if (dma_buf_fds[i] == -1) {
			error("failed to get file descriptor for BO plane %d (modifier 0x%" PRIx64 ")\n",
			      i, ret->modifier);
			goto err_bo;
		}

		ret->pitches[i] = gbm_bo_get_stride_for_plane(ret->gbm.bo, i);
		if (ret->pitches[i] == 0) {
			error("failed to get stride for BO plane %d (modifier 0x%" PRIx64 ")\n",
			      i, ret->modifier);
			goto err_bo;
		}

		ret->offsets[i] = gbm_bo_get_offset(ret->gbm.bo, i);
	}

	/*
	 * EGL has two versions of image creation, which are not actually
	 * interchangeable: eglCreateImageKHR takes an EGLint for its attrib
	 * list, whereas eglCreateImage (core as of 1.5) takes an EGLAttrib
	 * list, so we would have to have two different list-population
	 * implementations depending on which path we took.
	 *
	 * To avoid this, just use eglCreateImageKHR, since everyone
	 * implements that.
	 */
	if (!create_img) {
		create_img = (PFNEGLCREATEIMAGEKHRPROC)
			eglGetProcAddress("eglCreateImageKHR");
	}
	assert(create_img);

	attribs[nattribs++] = EGL_WIDTH;
	attribs[nattribs++] = ret->width;
	attribs[nattribs++] = EGL_HEIGHT;
	attribs[nattribs++] = ret->height;
	attribs[nattribs++] = EGL_LINUX_DRM_FOURCC_EXT;
	attribs[nattribs++] = DRM_FORMAT_XRGB8888;
	debug("importing %u x %u EGLImage with %d planes\n", ret->width, ret->height, num_planes);

	attribs[nattribs++] = EGL_DMA_BUF_PLANE0_FD_EXT;
	attribs[nattribs++] = dma_buf_fds[0];
	debug("\tplane 0 FD %d\n", attribs[nattribs - 1]);
	attribs[nattribs++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
	attribs[nattribs++] = ret->offsets[0];
	debug("\tplane 0 offset %d\n", attribs[nattribs - 1]);
	attribs[nattribs++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
	attribs[nattribs++] = ret->pitches[0];
	debug("\tplane 0 pitch %d\n", attribs[nattribs - 1]);
	if (device->fb_modifiers) {
		attribs[nattribs++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		attribs[nattribs++] = ret->modifier >> 32;
		debug("\tmodifier hi 0x%" PRIx32 "\n", attribs[nattribs - 1]);
		attribs[nattribs++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		attribs[nattribs++] = ret->modifier & 0xffffffff;
		debug("\tmodifier lo 0x%" PRIx32 "\n", attribs[nattribs - 1]);
	}

	if (num_planes > 1) {
		attribs[nattribs++] = EGL_DMA_BUF_PLANE1_FD_EXT;
		attribs[nattribs++] = dma_buf_fds[1];
		attribs[nattribs++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
		attribs[nattribs++] = ret->offsets[1];
		attribs[nattribs++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
		attribs[nattribs++] = ret->pitches[1];
		if (device->fb_modifiers) {
			attribs[nattribs++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
			attribs[nattribs++] = ret->modifier >> 32;
			attribs[nattribs++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
			attribs[nattribs++] = ret->modifier & 0xffffffff;
		}
	}

	if (num_planes > 2) {
		attribs[nattribs++] = EGL_DMA_BUF_PLANE2_FD_EXT;
		attribs[nattribs++] = dma_buf_fds[2];
		attribs[nattribs++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
		attribs[nattribs++] = ret->offsets[2];
		attribs[nattribs++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
		attribs[nattribs++] = ret->pitches[2];
		if (device->fb_modifiers) {
			attribs[nattribs++] = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT;
			attribs[nattribs++] = ret->modifier >> 32;
			attribs[nattribs++] = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT;
			attribs[nattribs++] = ret->modifier & 0xffffffff;
		}
	}

	if (num_planes > 3) {
		attribs[nattribs++] = EGL_DMA_BUF_PLANE3_FD_EXT;
		attribs[nattribs++] = dma_buf_fds[3];
		attribs[nattribs++] = EGL_DMA_BUF_PLANE3_OFFSET_EXT;
		attribs[nattribs++] = ret->offsets[3];
		attribs[nattribs++] = EGL_DMA_BUF_PLANE3_PITCH_EXT;
		attribs[nattribs++] = ret->pitches[3];
		if (device->fb_modifiers) {
			attribs[nattribs++] = EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT;
			attribs[nattribs++] = ret->modifier >> 32;
			attribs[nattribs++] = EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT;
			attribs[nattribs++] = ret->modifier & 0xffffffff;
		}
	}

	attribs[nattribs++] = EGL_NONE;

	err = eglMakeCurrent(device->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
			     output->egl.ctx);
	assert(err);

	/*
	 * Create an EGLImage from our GBM BO, which will give EGL and GLES
	 * the ability to use it as a render target.
	 */
	ret->gbm.img = create_img(device->egl_dpy, EGL_NO_CONTEXT,
				  EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
	if (!ret->gbm.img) {
		error("failed to create EGLImage for %u x %u BO (modifier 0x%" PRIx64 ")\n",
		      ret->width, ret->height, ret->modifier);
		goto err_bo;
	}

	/*
	 * EGL does not take ownership of the dma-buf file descriptors and will
	 * clone them internally. We don't need the file descriptors after
	 * they've been imported, so we can just close them now.
	 */
	for (int i = 0; i < num_planes; i++) {
		close(dma_buf_fds[i]);
	}

	/*
	 * Bind the EGLImage to a GLES texture unit, then bind that texture
	 * to a GL framebuffer object, so we can use it to render into.
	 */
	glGenTextures(1, &ret->gbm.tex_id);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ret->gbm.tex_id);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	if (!target_tex_2d) {
		target_tex_2d = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
			eglGetProcAddress("glEGLImageTargetTexture2DOES");
	}
	assert(target_tex_2d);
	target_tex_2d(GL_TEXTURE_2D, ret->gbm.img);

	glGenFramebuffers(1, &ret->gbm.fbo_id);
	glBindFramebuffer(GL_FRAMEBUFFER, ret->gbm.fbo_id);

	if (output->egl.have_gl_mesa_framebuffer_flip_y)
	{
		if (!framebuffer_parameteri) {
			framebuffer_parameteri = (PFNGLFRAMEBUFFERPARAMETERIPROC)
				eglGetProcAddress("glFramebufferParameteri");
		}
		assert(framebuffer_parameteri);
		framebuffer_parameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_FLIP_Y_MESA, GL_TRUE);
		debug("GL_MESA_framebuffer_flip_y is available\n");
	}

	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			       ret->gbm.tex_id, 0);
	assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

	return ret;

err_bo:
	gbm_bo_destroy(ret->gbm.bo);
	for (size_t i = 0; i < ARRAY_LENGTH(dma_buf_fds); i++) {
		if (dma_buf_fds[i] != -1)
			close(dma_buf_fds[i]);
	}
err:
	free(ret);
	return NULL;
}

void buffer_egl_destroy(struct device *device, struct buffer *buffer)
{
	static PFNEGLDESTROYIMAGEKHRPROC destroy_img = NULL;
	EGLBoolean ret;

	ret = eglMakeCurrent(device->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
			     buffer->output->egl.ctx);
	assert(ret);

	if (!destroy_img) {
		destroy_img = (PFNEGLDESTROYIMAGEKHRPROC)
			eglGetProcAddress("eglDestroyImageKHR");
	}
	assert(destroy_img);

	destroy_img(device->egl_dpy, buffer->gbm.img);
	glDeleteFramebuffers(1, &buffer->gbm.fbo_id);
	glDeleteTextures(1, &buffer->gbm.tex_id);
	gbm_bo_destroy(buffer->gbm.bo);
}

/*
 * Fills the vertex and color data to draw one of four quadrants of the
 * full-screen checkerboard pattern.
 *
 * The border separating the quadrants is animated from the upper-left to the
 * lower-right, using `frame_num` to drive the animation. Parameter `loc`
 * defines the quadrant for which to get the data for.
 *
 * A quadrant is drawn as a two-triangle triangle-fan. The vertex array's
 * coordinates will be interpreted directly as X/Y normalized device
 * coordinates where
 *
 *     x = [-1 .. 1] (left ... right)
 *     y = [-1 .. 1] (bottom ... top)
 *
 */
static void fill_verts(GLfloat *verts, GLfloat *col, float anim_progress, int loc)
{
	/* factor is in range [-1 .. 1] */
	float factor = anim_progress * 2.0 - 1.0f;
	GLfloat top, bottom, left, right;

	assert(loc >= 0 && loc < 4);

	switch (loc) {
	case 0:
		/* upper-left black quadrant */
		col[0] = 0.0f;
		col[1] = 0.0f;
		col[2] = 0.0f;
		col[3] = 1.0f;
		top = 1.0f;
		left = -1.0f;
		bottom = -factor;
		right = factor;
		break;
	case 1:
		/* upper-right red quadrant */
		col[0] = 1.0f;
		col[1] = 0.0f;
		col[2] = 0.0f;
		col[3] = 1.0f;
		top = 1.0f;
		left = factor;
		right = 1.0f;
		bottom = -factor;
		break;
	case 2:
		/* lower-left blue quadrant */
		col[0] = 0.0f;
		col[1] = 0.0f;
		col[2] = 1.0f;
		col[3] = 1.0f;
		top = -factor;
		left = -1.0f;
		bottom = -1.0f;
		right = factor;
		break;
	case 3:
		/* lower-right purpose quadrant */
		col[0] = 1.0f;
		col[1] = 0.0f;
		col[2] = 1.0f;
		col[3] = 1.0f;
		top = -factor;
		left = factor;
		bottom = -1.0f;
		right = 1.0f;
		break;
	}

	verts[0] = left;
	verts[1] = bottom;
	verts[2] = left;
	verts[3] = top;
	verts[4] = right;
	verts[5] = top;
	verts[6] = right;
	verts[7] = bottom;
}

void
buffer_egl_fill(struct buffer *buffer, float anim_progress)
{
	struct output *output = buffer->output;
	struct device *device = output->device;
	static PFNEGLCREATESYNCKHRPROC create_sync = NULL;
	static PFNEGLWAITSYNCKHRPROC wait_sync = NULL;
	static PFNEGLDESTROYSYNCKHRPROC destroy_sync = NULL;
	static PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence_fd = NULL;
	EGLSyncKHR sync;
	EGLBoolean ret;

	ret = eglMakeCurrent(device->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
			     output->egl.ctx);
	assert(ret);

	if (output->explicit_fencing) {
		if (!create_sync) {
			create_sync = (PFNEGLCREATESYNCKHRPROC)
				eglGetProcAddress("eglCreateSyncKHR");
		}
		assert(create_sync);

		if (!wait_sync) {
			wait_sync = (PFNEGLWAITSYNCKHRPROC)
				eglGetProcAddress("eglWaitSyncKHR");
		}
		assert(wait_sync);

		if (!destroy_sync) {
			destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)
				eglGetProcAddress("eglDestroySyncKHR");
		}
		assert(destroy_sync);

		if (!dup_fence_fd) {
			dup_fence_fd = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
				eglGetProcAddress("eglDupNativeFenceFDANDROID");
		}
		assert(dup_fence_fd);

		/*
		 * If this buffer was previously used by KMS, insert a sync
		 * wait before we use it, to ensure that the GPU doesn't render
		 * to the buffer whilst KMS is still using it.
		 *
		 * This isn't actually necessary with our current model, since we
		 * have more buffers than we need, and we wait in software until
		 * they've been released. But if you want to start rendering
		 * ahead of time, this fence will protect us.
		 */
		if (buffer->kms_fence_fd >= 0) {
			EGLint attribs[] = {
				EGL_SYNC_NATIVE_FENCE_FD_ANDROID, buffer->kms_fence_fd,
				EGL_NONE,
			};

			assert(linux_sync_file_is_valid(buffer->kms_fence_fd));
			sync = create_sync(device->egl_dpy,
					   EGL_SYNC_NATIVE_FENCE_ANDROID,
					   attribs);
			assert(sync);
			buffer->kms_fence_fd = -1;
			ret = wait_sync(device->egl_dpy, sync, 0);
			assert(ret);
			destroy_sync(device->egl_dpy, sync);
			sync = EGL_NO_SYNC_KHR;
		}
	}

	glBindFramebuffer(GL_FRAMEBUFFER, buffer->gbm.fbo_id);
	glViewport(0, 0, buffer->width, buffer->height);

	for (unsigned int i = 0; i < 4; i++) {
		GLfloat col[4];
		GLfloat verts[8];
		GLuint err = glGetError();
		fill_verts(verts, col, anim_progress, i);
		glBindBuffer(GL_ARRAY_BUFFER, output->egl.vbo);
		/* glBufferSubData is most supported across GLES2 / Core profile,
		 * Core profile / GLES3 might have better ways */
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(GLfloat) * 8, verts);
		glBindBuffer(GL_ARRAY_BUFFER, 0);

		if (output->egl.use_vao) {
			glBindVertexArray(output->egl.vao);
		} else {
			glBindBuffer(GL_ARRAY_BUFFER, output->egl.vbo);
			glVertexAttribPointer(output->egl.pos_attr, 2, GL_FLOAT, GL_FALSE, 0, NULL);
			glEnableVertexAttribArray(output->egl.pos_attr);
			glBindBuffer(GL_ARRAY_BUFFER, 0);
		}

		glUniform4f(output->egl.col_uniform, col[0], col[1], col[2], col[3]);
		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

		if (output->egl.use_vao)
			glBindVertexArray(0);
		else
			glDisableVertexAttribArray(output->egl.pos_attr);

		err = glGetError();
		if (err != GL_NO_ERROR)
			debug("GL error state 0x%x\n", err);
	}

	/*
	 * All our rendering has now been prepared. Create an EGLSyncKHR
	 * object which we _will_ extract a native fence FD from, but not
	 * yet.
	 *
	 * Since none of our commands have yet been flushed, we insert an
	 * explicit flush before we pull the native fence FD.
	 *
	 * This flush also acts as our guarantee when using implicit fencing
	 * that the rendering will actually be issued.
	 */
	if (output->explicit_fencing) {
		EGLint attribs[] = {
			EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID,
			EGL_NONE,
		};

		sync = create_sync(device->egl_dpy,
				   EGL_SYNC_NATIVE_FENCE_ANDROID,
				   attribs);
		assert(sync);
	}

	glFlush();

	/*
	 * Now we've flushed, we can get the fence FD associated with our
	 * rendering, which we can pass to KMS to wait for.
	 */
	if (output->explicit_fencing) {
		int fd = dup_fence_fd(device->egl_dpy, sync);
		assert(fd >= 0);
		assert(linux_sync_file_is_valid(fd));
		fd_replace(&buffer->render_fence_fd, fd);
		destroy_sync(device->egl_dpy, sync);
	}
}
