/*
 * Copyright © 2016 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "wayland-client.h"
#include "wayland-server.h"
#include "test-runner.h"

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof(a)[0])

/* Ensure the connection doesn't fail due to lack of XDG_RUNTIME_DIR. */
static const char *
require_xdg_runtime_dir(void)
{
	char *val = getenv("XDG_RUNTIME_DIR");
	assert(val && val[0] == '/' && "set $XDG_RUNTIME_DIR to run this test");

	return val;
}

struct expected_compositor_message {
	enum wl_protocol_logger_type type;
	const char *class;
	int opcode;
	const char *message_name;
	int args_count;
};

struct compositor {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wl_protocol_logger *logger;

	struct expected_compositor_message *expected_msg;
	int expected_msg_count;
	int actual_msg_count;
	struct wl_client *client;
};

struct expected_client_message {
	enum wl_client_message_type type;
	enum wl_client_message_discarded_reason discarded_reason;
	const char *queue_name;
	const char *class;
	int opcode;
	const char *message_name;
	int args_count;
};

struct client {
	struct wl_display *display;
	struct wl_callback *cb;
	struct wl_client_observer *sequence_observer;
	struct wl_client_observer *stderr_logger;

	struct expected_client_message *expected_msg;
	int expected_msg_count;
	int actual_msg_count;
};

static int
safe_strcmp(const char *s1, const char *s2) {
	if (s1 == NULL && s2 == NULL)
		return 0;
	if (s1 == NULL && s2 != NULL)
		return 1;
	if (s1 != NULL && s2 == NULL)
		return -1;
	return strcmp(s1, s2);
}

#define ASSERT_LT(arg1, arg2, ...)                                            \
	if (arg1 >= arg2)                                                     \
		fprintf(stderr, __VA_ARGS__);                                 \
	assert(arg1 < arg2)

#define ASSERT_EQ(arg1, arg2, ...)                                            \
	if (arg1 != arg2)                                                     \
		fprintf(stderr, __VA_ARGS__);                                 \
	assert(arg1 == arg2)

#define ASSERT_STR_EQ(arg1, arg2, ...)                                        \
	if (safe_strcmp(arg1, arg2) != 0)                                          \
		fprintf(stderr, __VA_ARGS__);                                 \
	assert(safe_strcmp(arg1, arg2) == 0)

static void
compositor_sequence_observer_func(
	void *user_data, enum wl_protocol_logger_type actual_type,
	const struct wl_protocol_logger_message *actual_msg)
{
	struct compositor *c = user_data;
	struct expected_compositor_message *expected_msg;
	int actual_msg_count = c->actual_msg_count++;
	char details_msg[256];

	c->client = wl_resource_get_client(actual_msg->resource);

	if (!c->expected_msg)
		return;

	ASSERT_LT(actual_msg_count, c->expected_msg_count,
		  "actual count %d exceeds expected count %d\n",
		  actual_msg_count, c->expected_msg_count);

	expected_msg = &c->expected_msg[actual_msg_count];

	snprintf(details_msg, sizeof details_msg,
		 "compositor msg %d of %d actual [%d, '%s', %d, '%s', %d] vs "
		 "expected [%d, '%s', %d, '%s', %d]\n",
		 c->actual_msg_count, c->expected_msg_count, actual_type,
		 wl_resource_get_class(actual_msg->resource),
		 actual_msg->message_opcode, actual_msg->message->name,
		 actual_msg->arguments_count, expected_msg->type,
		 expected_msg->class, expected_msg->opcode,
		 expected_msg->message_name, expected_msg->args_count);

	ASSERT_EQ(expected_msg->type, actual_type, "type mismatch: %s",
		  details_msg);
	ASSERT_STR_EQ(expected_msg->class,
		      wl_resource_get_class(actual_msg->resource),
		      "class mismatch: %s", details_msg);
	ASSERT_EQ(expected_msg->opcode, actual_msg->message_opcode,
		  "opcode mismatch: %s", details_msg);
	ASSERT_STR_EQ(expected_msg->message_name, actual_msg->message->name,
		      "message name mismatch: %s", details_msg);
	ASSERT_EQ(expected_msg->args_count, actual_msg->arguments_count,
		  "arg count mismatch: %s", details_msg);
}

static void
client_sequence_observer_func(
	void *user_data, enum wl_client_message_type actual_type,
	const struct wl_client_observed_message *actual_msg)
{
	struct client *c = user_data;
	struct expected_client_message *expected_msg;
	int actual_msg_count = c->actual_msg_count++;
	char details_msg[256];

	if (!c->expected_msg)
		return;

	ASSERT_LT(actual_msg_count, c->expected_msg_count,
		  "actual count %d exceeds expected count %d\n",
		  actual_msg_count, c->expected_msg_count);
	expected_msg = &c->expected_msg[actual_msg_count];

	snprintf(details_msg, sizeof details_msg,
		 "client msg %d of %d actual [%d, %d, '%s', '%s', %d, '%s', %d] vs "
		 "expected [%d, %d, '%s', '%s', %d, '%s', %d]\n",
		 c->actual_msg_count, c->expected_msg_count, actual_type,
		 actual_msg->discarded_reason,
		 actual_msg->queue_name ? actual_msg->queue_name : "NULL",
		 wl_proxy_get_class(actual_msg->proxy),
		 actual_msg->message_opcode, actual_msg->message->name,
		 actual_msg->arguments_count, expected_msg->type,
		 expected_msg->discarded_reason,
		 expected_msg->queue_name ? expected_msg->queue_name : "NULL",
		 expected_msg->class, expected_msg->opcode,
		 expected_msg->message_name, expected_msg->args_count);

	ASSERT_EQ(expected_msg->type, actual_type, "type mismatch: %s",
		  details_msg);
	ASSERT_EQ(expected_msg->discarded_reason, actual_msg->discarded_reason,
		  "discarded reason mismatch: %s", details_msg);
	ASSERT_STR_EQ(expected_msg->queue_name, actual_msg->queue_name,
		      "queue name mismatch: %s", details_msg);
	ASSERT_STR_EQ(expected_msg->class,
		      wl_proxy_get_class(actual_msg->proxy),
		      "class mismatch: %s", details_msg);
	ASSERT_EQ(expected_msg->opcode, actual_msg->message_opcode,
		  "opcode mismatch: %s", details_msg);
	ASSERT_STR_EQ(expected_msg->message_name, actual_msg->message->name,
		      "message name mismatch: %s", details_msg);
	ASSERT_EQ(expected_msg->args_count, actual_msg->arguments_count,
		  "arg count mismatch: %s", details_msg);
}

// A slightly simplified version of get_next_argument() from src/connection.c
static const char *
get_next_argument_type(const char *signature, char *type)
{
	for (; *signature; ++signature) {
		assert(strchr("iufsonah?", *signature) != NULL);
		switch (*signature) {
		case 'i':
		case 'u':
		case 'f':
		case 's':
		case 'o':
		case 'n':
		case 'a':
		case 'h':
			*type = *signature;
			return signature + 1;
		case '?':
			break;
		}
	}
	*type = 0;
	return signature;
}

// This duplicates what the internal wl_closure_print function does, and can be
// used as a starting point for a client or server that wants to log messages.
static void
client_log_to_stderr_demo(void *user_data, enum wl_client_message_type type,
			  const struct wl_client_observed_message *message)
{
	int i;
	char arg_type;
	const char *signature = message->message->signature;
	const union wl_argument *args = message->arguments;
	struct wl_proxy *arg_proxy;
	const char *arg_class;
	struct timespec tp;
	unsigned int time;
	FILE *f;
	char *buffer;
	size_t buffer_length;

	f = open_memstream(&buffer, &buffer_length);
	if (f == NULL)
		return;

	clock_gettime(CLOCK_REALTIME, &tp);
	time = (tp.tv_sec * 1000000L) + (tp.tv_nsec / 1000);

	// Note: server logger will be given message->resource, and should
	// use wl_resource_get_class and wl_resource_get_id.
	fprintf(f, "[%7u.%03u] %s%s%s%s%s%s%s%s#%u.%s(", time / 1000, time % 1000,
		(message->queue_name != NULL) ? "{" : "",
		(message->queue_name != NULL) ? message->queue_name : "",
		(message->queue_name != NULL) ? "} " : "",
		(message->discarded_reason_str ? "discarded[" : ""),
		(message->discarded_reason_str ? message->discarded_reason_str
					       : ""),
		(message->discarded_reason_str ? "] " : ""),
		(type == WL_CLIENT_MESSAGE_REQUEST) ? " -> " : "",
		wl_proxy_get_class(message->proxy),
		wl_proxy_get_id(message->proxy), message->message->name);

	for (i = 0; i < message->arguments_count; i++) {
		signature = get_next_argument_type(signature, &arg_type);
		if (i > 0)
			fprintf(f, ", ");

		switch (arg_type) {
		case 'u':
			fprintf(f, "%u", args[i].u);
			break;
		case 'i':
			fprintf(f, "%d", args[i].i);
			break;
		case 'f':
			fprintf(f, "%f", wl_fixed_to_double(args[i].f));
			break;
		case 's':
			if (args[i].s)
				fprintf(f, "\"%s\"", args[i].s);
			else
				fprintf(f, "nil");
			break;
		case 'o':
			if (args[i].o) {
				// Note: server logger should instead use
				// wl_resource_from_object, and then
				// wl_resource_get_class and
				// wl_resource_get_id.
				arg_proxy = wl_proxy_from_object(args[i].o);
				arg_class = wl_proxy_get_class(arg_proxy);

				fprintf(f, "%s#%u",
					arg_class ? arg_class : "[unknown]",
					wl_proxy_get_id(arg_proxy));
			} else {
				fprintf(f, "nil");
			}
			break;
		case 'n':
			fprintf(f, "new id %s#",
				(message->message->types[i])
					? message->message->types[i]->name
					: "[unknown]");
			if (args[i].n != 0)
				fprintf(f, "%u", args[i].n);
			else
				fprintf(f, "nil");
			break;
		case 'a':
			fprintf(f, "array");
			break;
		case 'h':
			fprintf(f, "fd %d", args[i].h);
			break;
		}
	}

	fprintf(f, ")\n");

	if (fclose(f) == 0) {
		fprintf(stderr, "%s", buffer);
		free(buffer);
	}
}

static void
callback_done(void *data, struct wl_callback *cb, uint32_t time)
{
	wl_callback_destroy(cb);
}

static const struct wl_callback_listener callback_listener = {
	callback_done,
};

static void
logger_setup(struct compositor *compositor, struct client *client)
{
	const char *socket;

	require_xdg_runtime_dir();

	compositor->display = wl_display_create();
	compositor->loop = wl_display_get_event_loop(compositor->display);
	socket = wl_display_add_socket_auto(compositor->display);

	compositor->logger = wl_display_add_protocol_logger(
		compositor->display, compositor_sequence_observer_func,
		compositor);

	client->display = wl_display_connect(socket);
	client->sequence_observer = wl_display_create_client_observer(
		client->display, client_sequence_observer_func, client);
	client->stderr_logger = wl_display_create_client_observer(
		client->display, client_log_to_stderr_demo, client);
}

static void
logger_teardown(struct compositor *compositor, struct client *client)
{
	wl_client_observer_destroy(client->sequence_observer);
	wl_client_observer_destroy(client->stderr_logger);
	wl_display_disconnect(client->display);

	wl_client_destroy(compositor->client);
	wl_protocol_logger_destroy(compositor->logger);
	wl_display_destroy(compositor->display);
}

TEST(logger)
{
	test_set_timeout(1);

	struct expected_compositor_message compositor_messages[] = {
		{
			.type = WL_PROTOCOL_LOGGER_REQUEST,
			.class = "wl_display",
			.opcode = 0,
			.message_name = "sync",
			.args_count = 1,
		},
		{
			.type = WL_PROTOCOL_LOGGER_EVENT,
			.class = "wl_callback",
			.opcode = 0,
			.message_name = "done",
			.args_count = 1,
		},
		{
			.type = WL_PROTOCOL_LOGGER_EVENT,
			.class = "wl_display",
			.opcode = 1,
			.message_name = "delete_id",
			.args_count = 1,
		},
	};
	struct expected_client_message client_messages[] = {
		{
			.type = WL_CLIENT_MESSAGE_REQUEST,
			.discarded_reason = WL_CLIENT_MESSAGE_NOT_DISCARDED,
			.queue_name = "Default Queue",
			.class = "wl_display",
			.opcode = 0,
			.message_name = "sync",
			.args_count = 1,
		},
		{
			.type = WL_CLIENT_MESSAGE_EVENT,
			.discarded_reason = WL_CLIENT_MESSAGE_NOT_DISCARDED,
			.queue_name = "Display Queue",
			.class = "wl_display",
			.opcode = 1,
			.message_name = "delete_id",
			.args_count = 1,
		},
		{
			.type = WL_CLIENT_MESSAGE_EVENT,
			.discarded_reason = WL_CLIENT_MESSAGE_NOT_DISCARDED,
			.queue_name = "Default Queue",
			.class = "wl_callback",
			.opcode = 0,
			.message_name = "done",
			.args_count = 1,
		},
	};
	struct compositor compositor = { 0 };
	struct client client = { 0 };

	logger_setup(&compositor, &client);

	compositor.expected_msg = &compositor_messages[0];
	compositor.expected_msg_count = ARRAY_LENGTH(compositor_messages);

	client.expected_msg = &client_messages[0];
	client.expected_msg_count = ARRAY_LENGTH(client_messages);

	client.cb = wl_display_sync(client.display);
	wl_callback_add_listener(client.cb, &callback_listener, NULL);
	wl_display_flush(client.display);

	while (compositor.actual_msg_count < compositor.expected_msg_count) {
		wl_event_loop_dispatch(compositor.loop, -1);
		wl_display_flush_clients(compositor.display);
	}

	while (client.actual_msg_count < client.expected_msg_count) {
		wl_display_dispatch(client.display);
	}

	logger_teardown(&compositor, &client);
}

TEST(client_discards_if_dead_on_dispatch)
{
	test_set_timeout(1);

	struct expected_client_message client_messages[] = {
		{
			.type = WL_CLIENT_MESSAGE_REQUEST,
			.discarded_reason = WL_CLIENT_MESSAGE_NOT_DISCARDED,
			.queue_name = "Default Queue",
			.class = "wl_display",
			.opcode = 0,
			.message_name = "sync",
			.args_count = 1,
		},
		{
			.type = WL_CLIENT_MESSAGE_EVENT,
			.discarded_reason = WL_CLIENT_MESSAGE_NOT_DISCARDED,
			.queue_name = "Display Queue",
			.class = "wl_display",
			.opcode = 1,
			.message_name = "delete_id",
			.args_count = 1,
		},
		{
			.type = WL_CLIENT_MESSAGE_EVENT,
			.discarded_reason =
				WL_CLIENT_MESSAGE_DISCARD_DEAD_PROXY_ON_DISPATCH,
			.queue_name = "Default Queue",
			.class = "wl_callback",
			.opcode = 0,
			.message_name = "done",
			.args_count = 1,
		},
	};
	struct compositor compositor = { 0 };
	struct client client = { 0 };

	logger_setup(&compositor, &client);

	compositor.expected_msg_count = 3;

	client.expected_msg = &client_messages[0];
	client.expected_msg_count = ARRAY_LENGTH(client_messages);

	client.cb = wl_display_sync(client.display);
	wl_callback_add_listener(client.cb, &callback_listener, NULL);
	wl_display_flush(client.display);

	while (compositor.actual_msg_count < compositor.expected_msg_count) {
		wl_event_loop_dispatch(compositor.loop, -1);
		wl_display_flush_clients(compositor.display);
	}

	wl_display_prepare_read(client.display);
	wl_display_read_events(client.display);

	// To get a WL_CLIENT_MESSAGE_DISCARD_DEAD_PROXY_ON_DISPATCH, we
	// destroy the callback after reading client events, but before
	// dispatching them.
	wl_callback_destroy(client.cb);

	while (client.actual_msg_count < client.expected_msg_count) {
		wl_display_dispatch(client.display);
	}

	logger_teardown(&compositor, &client);
}

TEST(client_discards_if_no_listener_on_dispatch)
{
	test_set_timeout(1);

	struct expected_client_message client_messages[] = {
		{
			.type = WL_CLIENT_MESSAGE_REQUEST,
			.discarded_reason = WL_CLIENT_MESSAGE_NOT_DISCARDED,
			.queue_name = "Default Queue",
			.class = "wl_display",
			.opcode = 0,
			.message_name = "sync",
			.args_count = 1,
		},
		{
			.type = WL_CLIENT_MESSAGE_EVENT,
			.discarded_reason = WL_CLIENT_MESSAGE_NOT_DISCARDED,
			.queue_name = "Display Queue",
			.class = "wl_display",
			.opcode = 1,
			.message_name = "delete_id",
			.args_count = 1,
		},
		{
			.type = WL_CLIENT_MESSAGE_EVENT,
			.discarded_reason =
				WL_CLIENT_MESSAGE_DISCARD_NO_LISTENER_ON_DISPATCH,
			.queue_name = "Default Queue",
			.class = "wl_callback",
			.opcode = 0,
			.message_name = "done",
			.args_count = 1,
		},
	};
	struct compositor compositor = { 0 };
	struct client client = { 0 };

	logger_setup(&compositor, &client);

	compositor.expected_msg_count = 3;

	client.expected_msg = &client_messages[0];
	client.expected_msg_count = ARRAY_LENGTH(client_messages);

	client.cb = wl_display_sync(client.display);
	wl_display_flush(client.display);

	while (compositor.actual_msg_count < compositor.expected_msg_count) {
		wl_event_loop_dispatch(compositor.loop, -1);
		wl_display_flush_clients(compositor.display);
	}

	while (client.actual_msg_count < client.expected_msg_count) {
		wl_display_dispatch(client.display);
	}

	wl_callback_destroy(client.cb);

	logger_teardown(&compositor, &client);
}
