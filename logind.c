/*
 * This file implements basic logind integration to acquire/release devices in
 * a secure and robust way.
 *
 * This file is mostly a copy of wlroot's logind session backend:
 * https://github.com/swaywm/wlroots/blob/master/backend/session/logind.c
 */

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#if HAVE_SYSTEMD
	#include <systemd/sd-bus.h>
	#include <systemd/sd-login.h>
#elif HAVE_ELOGIND
	#include <elogind/sd-bus.h>
	#include <elogind/sd-login.h>
#endif

enum { DRM_MAJOR = 226 };

struct logind {
    /*
	 * 0 if virtual terminals are not supported
	 * i.e. seat != "seat0"
	 */
    unsigned vtnr;

    char seat[256];

	sd_bus *bus;
	struct wl_event_source *event;

	char *id;
	char *path;

	// specifies whether a drm device was taken
	// if so, the session will be (de)activated with the drm fd,
	// otherwise with the dbus PropertiesChanged on "active" signal
	bool has_drm;
	bool active;
};

int logind_take_device(struct logind *session, const char *path) {
	int fd = -1;
	int ret;
	sd_bus_message *msg = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;

	struct stat st;
	if (stat(path, &st) < 0) {
		fprintf(stderr, "Failed to stat '%s'\n", path);
		return -1;
	}

	if (major(st.st_rdev) == DRM_MAJOR) {
		session->has_drm = true;
	}

	ret = sd_bus_call_method(session->bus, "org.freedesktop.login1",
		session->path, "org.freedesktop.login1.Session", "TakeDevice",
		&error, &msg, "uu", major(st.st_rdev), minor(st.st_rdev));
	if (ret < 0) {
		fprintf(stderr, "Failed to take device '%s': %s\n", path,
			error.message);
		goto out;
	}

	int paused = 0;
	ret = sd_bus_message_read(msg, "hb", &fd, &paused);
	if (ret < 0) {
		fprintf(stderr, "Failed to parse D-Bus response for '%s': %s\n",
			path, strerror(-ret));
		goto out;
	}

	// The original fd seems to be closed when the message is freed
	// so we just clone it.
	fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (fd < 0) {
		fprintf(stderr, "Failed to clone file descriptor for '%s': %s\n",
			path, strerror(errno));
		goto out;
	}

out:
	sd_bus_error_free(&error);
	sd_bus_message_unref(msg);
	return fd;
}

void logind_release_device(struct logind *session, int fd) {
	struct stat st;
	if (fstat(fd, &st) < 0) {
		fprintf(stderr, "Failed to stat device '%d': %s\n", fd,
			strerror(errno));
		return;
	}

	sd_bus_message *msg = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	int ret = sd_bus_call_method(session->bus, "org.freedesktop.login1",
		session->path, "org.freedesktop.login1.Session", "ReleaseDevice",
		&error, &msg, "uu", major(st.st_rdev), minor(st.st_rdev));
	if (ret < 0) {
		fprintf(stderr, "Failed to release device '%d': %s\n", fd,
			error.message);
	}

	sd_bus_error_free(&error);
	sd_bus_message_unref(msg);
	close(fd);
}

static bool find_session_path(struct logind *session) {
	int ret;
	sd_bus_message *msg = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;

	ret = sd_bus_call_method(session->bus, "org.freedesktop.login1",
			"/org/freedesktop/login1", "org.freedesktop.login1.Manager",
			"GetSession", &error, &msg, "s", session->id);
	if (ret < 0) {
		fprintf(stderr, "Failed to get session path: %s\n", error.message);
		goto out;
	}

	const char *path;
	ret = sd_bus_message_read(msg, "o", &path);
	if (ret < 0) {
		fprintf(stderr, "Could not parse session path: %s\n", error.message);
		goto out;
	}
	session->path = strdup(path);

out:
	sd_bus_error_free(&error);
	sd_bus_message_unref(msg);

	return ret >= 0;
}

static bool session_activate(struct logind *session) {
	int ret;
	sd_bus_message *msg = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;

	ret = sd_bus_call_method(session->bus, "org.freedesktop.login1",
		session->path, "org.freedesktop.login1.Session", "Activate",
		&error, &msg, "");
	if (ret < 0) {
		fprintf(stderr, "Failed to activate session: %s\n", error.message);
	}

	sd_bus_error_free(&error);
	sd_bus_message_unref(msg);
	return ret >= 0;
}

static bool take_control(struct logind *session) {
	int ret;
	sd_bus_message *msg = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;

	ret = sd_bus_call_method(session->bus, "org.freedesktop.login1",
		session->path, "org.freedesktop.login1.Session", "TakeControl",
		&error, &msg, "b", false);
	if (ret < 0) {
		fprintf(stderr, "Failed to take control of session: %s\n",
			error.message);
	}

	sd_bus_error_free(&error);
	sd_bus_message_unref(msg);
	return ret >= 0;
}

static void release_control(struct logind *session) {
	int ret;
	sd_bus_message *msg = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;

	ret = sd_bus_call_method(session->bus, "org.freedesktop.login1",
		session->path, "org.freedesktop.login1.Session", "ReleaseControl",
		&error, &msg, "");
	if (ret < 0) {
		fprintf(stderr, "Failed to release control of session: %s\n",
			error.message);
	}

	sd_bus_error_free(&error);
	sd_bus_message_unref(msg);
}

void logind_destroy(struct logind *session) {
	release_control(session);

	sd_bus_unref(session->bus);
	free(session->id);
	free(session->path);
	free(session);
}

static bool contains_str(const char *needle, const char **haystack) {
	for (int i = 0; haystack[i]; i++) {
		if (strcmp(haystack[i], needle) == 0) {
			return true;
		}
	}

	return false;
}

static bool get_greeter_session(char **session_id) {
	char *class = NULL;
	char **user_sessions = NULL;
	int user_session_count = sd_uid_get_sessions(getuid(), 1, &user_sessions);

	if (user_session_count < 0) {
		fprintf(stderr, "Failed to get sessions\n");
		goto out;
	}

	if (user_session_count == 0) {
		fprintf(stderr, "User has no sessions\n");
		goto out;
	}

	for (int i = 0; i < user_session_count; ++i) {
		int ret = sd_session_get_class(user_sessions[i], &class);
		if (ret < 0) {
			continue;
		}

		if (strcmp(class, "greeter") == 0) {
			*session_id = strdup(user_sessions[i]);
			goto out;
		}
	}

out:
	free(class);
	for (int i = 0; i < user_session_count; ++i) {
		free(user_sessions[i]);
	}
	free(user_sessions);

	return *session_id != NULL;
}

static bool get_display_session(char **session_id) {
	assert(session_id != NULL);
	int ret;

	char *type = NULL;
	char *state = NULL;
	char *xdg_session_id = getenv("XDG_SESSION_ID");

	if (xdg_session_id) {
		// This just checks whether the supplied session ID is valid
		if (sd_session_is_active(xdg_session_id) < 0) {
			fprintf(stderr, "Invalid XDG_SESSION_ID: '%s'\n", xdg_session_id);
			goto error;
		}
		*session_id = strdup(xdg_session_id);
		return true;
	}

	// If there's a session active for the current process then just use that
	ret = sd_pid_get_session(getpid(), session_id);
	if (ret == 0) {
		return true;
	}

	// Find any active sessions for the user if the process isn't part of an
	// active session itself
	ret = sd_uid_get_display(getuid(), session_id);
	if (ret < 0 && ret != -ENODATA) {
		fprintf(stderr, "Failed to get display: %s\n", strerror(-ret));
		goto error;
	}

	if (ret != 0 && !get_greeter_session(session_id)) {
		fprintf(stderr, "Couldn't find an active session or a greeter session\n");
		goto error;
	}

	assert(*session_id != NULL);

	// Check that the available session is graphical
	ret = sd_session_get_type(*session_id, &type);
	if (ret < 0) {
		fprintf(stderr, "Couldn't get a type for session '%s': %s\n",
				*session_id, strerror(-ret));
		goto error;
	}

	const char *graphical_session_types[] = {"wayland", "x11", "mir", NULL};
	if (!contains_str(type, graphical_session_types)) {
		fprintf(stderr, "Session '%s' isn't a graphical session (type: '%s')\n",
				*session_id, type);
		goto error;
	}

	// Check that the session is active
	ret = sd_session_get_state(*session_id, &state);
	if (ret < 0) {
		fprintf(stderr, "Couldn't get state for session '%s': %s\n",
				*session_id, strerror(-ret));
		goto error;
	}

	const char *active_states[] = {"active", "online", NULL};
	if (!contains_str(state, active_states)) {
		fprintf(stderr, "Session '%s' is not active\n", *session_id);
		goto error;
	}

	free(type);
	free(state);
	return true;

error:
	free(type);
	free(state);
	free(*session_id);
	*session_id = NULL;

	return false;
}

struct logind *logind_create(void) {
	int ret;
	struct logind *session = calloc(1, sizeof(*session));
	if (!session) {
		fprintf(stderr, "Allocation failed: %s\n", strerror(errno));
		return NULL;
	}

	if (!get_display_session(&session->id)) {
		goto error;
	}

	char *seat;
	ret = sd_session_get_seat(session->id, &seat);
	if (ret < 0) {
		fprintf(stderr, "Failed to get seat id: %s\n", strerror(-ret));
		goto error;
	}
	snprintf(session->seat, sizeof(session->seat), "%s\n", seat);

	if (strcmp(seat, "seat0") == 0) {
		ret = sd_session_get_vt(session->id, &session->vtnr);
		if (ret < 0) {
			fprintf(stderr, "Session not running in virtual terminal\n");
			goto error;
		}
	}
	free(seat);

	ret = sd_bus_default_system(&session->bus);
	if (ret < 0) {
		fprintf(stderr, "Failed to open D-Bus connection: %s\n", strerror(-ret));
		goto error;
	}

	if (!find_session_path(session)) {
		sd_bus_unref(session->bus);
		goto error;
	}

	if (!session_activate(session)) {
		goto error_bus;
	}

	if (!take_control(session)) {
		goto error_bus;
	}

	printf("Successfully loaded logind session\n");

	return session;

error_bus:
	sd_bus_unref(session->bus);
	free(session->path);

error:
	free(session->id);
	return NULL;
}

