/*
 * Copyright 2016 - Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <glib.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>

#include <babeltrace/compat/send-internal.h>
#include <babeltrace/compiler-internal.h>

#include "lttng-live-internal.h"
#include "viewer-connection.h"
#include "lttng-viewer-abi.h"
#include "data-stream.h"
#include "metadata.h"

#define PRINT_ERR_STREAM	viewer_connection->error_fp
#define PRINT_PREFIX		"lttng-live-viewer-connection"
#define PRINT_DBG_CHECK		lttng_live_debug
#include "../print.h"

static ssize_t lttng_live_recv(int fd, void *buf, size_t len)
{
	ssize_t ret;
	size_t copied = 0, to_copy = len;

	do {
		ret = recv(fd, buf + copied, to_copy, 0);
		if (ret > 0) {
			assert(ret <= to_copy);
			copied += ret;
			to_copy -= ret;
		}
	} while ((ret > 0 && to_copy > 0)
		|| (ret < 0 && errno == EINTR));
	if (ret > 0)
		ret = copied;
	/* ret = 0 means orderly shutdown, ret < 0 is error. */
	return ret;
}

static ssize_t lttng_live_send(int fd, const void *buf, size_t len)
{
	ssize_t ret;

	do {
		ret = bt_send_nosigpipe(fd, buf, len);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

/*
 * hostname parameter needs to hold MAXNAMLEN chars.
 */
static int parse_url(struct bt_live_viewer_connection *viewer_connection)
{
	char remain[3][MAXNAMLEN];
	int ret = -1, proto, proto_offset = 0;
	const char *path = viewer_connection->url->str;
	size_t path_len;

	if (!path) {
		goto end;
	}
	path_len = strlen(path); /* not accounting \0 */

	/*
	 * Since sscanf API does not allow easily checking string length
	 * against a size defined by a macro. Test it beforehand on the
	 * input. We know the output is always <= than the input length.
	 */
	if (path_len >= MAXNAMLEN) {
		goto end;
	}
	ret = sscanf(path, "net%d://", &proto);
	if (ret < 1) {
		proto = 4;
		/* net:// */
		proto_offset = strlen("net://");
	} else {
		/* net4:// or net6:// */
		proto_offset = strlen("netX://");
	}
	if (proto_offset > path_len) {
		goto end;
	}
	if (proto == 6) {
		PERR("[error] IPv6 is currently unsupported by lttng-live\n");
		goto end;
	}
	/* TODO : parse for IPv6 as well */
	/* Parse the hostname or IP */
	ret = sscanf(&path[proto_offset], "%[a-zA-Z.0-9%-]%s",
		viewer_connection->relay_hostname, remain[0]);
	if (ret == 2) {
		/* Optional port number */
		switch (remain[0][0]) {
		case ':':
			ret = sscanf(remain[0], ":%d%s", &viewer_connection->port, remain[1]);
			/* Optional session ID with port number */
			if (ret == 2) {
				ret = sscanf(remain[1], "/%s", remain[2]);
				/* Accept 0 or 1 (optional) */
				if (ret < 0) {
					goto end;
				}
			} else if (ret == 0) {
				PERR("[error] Missing port number after delimitor ':'\n");
				ret = -1;
				goto end;
			}
			break;
		case '/':
			/* Optional session ID */
			ret = sscanf(remain[0], "/%s", remain[2]);
			/* Accept 0 or 1 (optional) */
			if (ret < 0) {
				goto end;
			}
			break;
		default:
			PERR("[error] wrong delimitor : %c\n", remain[0][0]);
			ret = -1;
			goto end;
		}
	}

	if (viewer_connection->port < 0) {
		viewer_connection->port = LTTNG_DEFAULT_NETWORK_VIEWER_PORT;
	}

	if (strlen(remain[2]) == 0) {
		PDBG("Connecting to hostname : %s, port : %d, "
				"proto : IPv%d\n",
				viewer_connection->relay_hostname,
				viewer_connection->port,
				proto);
		ret = 0;
		goto end;
	}
	ret = sscanf(remain[2], "host/%[a-zA-Z.0-9%-]/%s",
			viewer_connection->target_hostname,
			viewer_connection->session_name);
	if (ret != 2) {
		PERR("[error] Format : "
			"net://<hostname>/host/<target_hostname>/<session_name>\n");
		goto end;
	}

	PDBG("Connecting to hostname : %s, port : %d, "
			"target hostname : %s, session name : %s, "
			"proto : IPv%d\n",
			viewer_connection->relay_hostname,
			viewer_connection->port,
			viewer_connection->target_hostname,
			viewer_connection->session_name, proto);
	ret = 0;

end:
	return ret;
}

static int lttng_live_handshake(struct bt_live_viewer_connection *viewer_connection)
{
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_connect connect;
	int ret;
	ssize_t ret_len;

	cmd.cmd = htobe32(LTTNG_VIEWER_CONNECT);
	cmd.data_size = htobe64((uint64_t) sizeof(connect));
	cmd.cmd_version = htobe32(0);

	connect.viewer_session_id = -1ULL;	/* will be set on recv */
	connect.major = htobe32(LTTNG_LIVE_MAJOR);
	connect.minor = htobe32(LTTNG_LIVE_MINOR);
	connect.type = htobe32(LTTNG_VIEWER_CLIENT_COMMAND);

	ret_len = lttng_live_send(viewer_connection->control_sock, &cmd, sizeof(cmd));
	if (ret_len < 0) {
		PERR("Error sending cmd: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(cmd));

	ret_len = lttng_live_send(viewer_connection->control_sock, &connect, sizeof(connect));
	if (ret_len < 0) {
		PERR("Error sending version: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(connect));

	ret_len = lttng_live_recv(viewer_connection->control_sock, &connect, sizeof(connect));
	if (ret_len == 0) {
		PERR("Remote side has closed connection\n");
		goto error;
	}
	if (ret_len < 0) {
		PERR("[error] Error receiving version: %s", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(connect));

	PDBG("Received viewer session ID : %" PRIu64 "\n",
			be64toh(connect.viewer_session_id));
	PDBG("Relayd version : %u.%u\n", be32toh(connect.major),
			be32toh(connect.minor));

	if (LTTNG_LIVE_MAJOR != be32toh(connect.major)) {
		PERR("Incompatible lttng-relayd protocol\n");
		goto error;
	}
	/* Use the smallest protocol version implemented. */
	if (LTTNG_LIVE_MINOR > be32toh(connect.minor)) {
		viewer_connection->minor =  be32toh(connect.minor);
	} else {
		viewer_connection->minor =  LTTNG_LIVE_MINOR;
	}
	viewer_connection->major = LTTNG_LIVE_MAJOR;
	ret = 0;
	return ret;

error:
	PERR("Unable to establish connection\n");
	return -1;
}

static int lttng_live_connect_viewer(struct bt_live_viewer_connection *viewer_connection)
{
	struct hostent *host;
	struct sockaddr_in server_addr;
	int ret;

	if (parse_url(viewer_connection)) {
		goto error;
	}

	host = gethostbyname(viewer_connection->relay_hostname);
	if (!host) {
		PERR("[error] Cannot lookup hostname %s\n",
			viewer_connection->relay_hostname);
		goto error;
	}

	if ((viewer_connection->control_sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		PERR("[error] Socket creation failed: %s\n", strerror(errno));
		goto error;
	}

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(viewer_connection->port);
	server_addr.sin_addr = *((struct in_addr *) host->h_addr);
	memset(&(server_addr.sin_zero), 0, 8);

	if (connect(viewer_connection->control_sock, (struct sockaddr *) &server_addr,
				sizeof(struct sockaddr)) == -1) {
		PERR("[error] Connection failed: %s\n", strerror(errno));
		goto error;
	}
	if (lttng_live_handshake(viewer_connection)) {
		goto error;
	}

	ret = 0;

	return ret;

error:
	if (viewer_connection->control_sock >= 0) {
		if (close(viewer_connection->control_sock)) {
			PERR("Close: %s", strerror(errno));
		}
	}
	viewer_connection->control_sock = -1;
	return -1;
}

static void lttng_live_disconnect_viewer(struct bt_live_viewer_connection *viewer_connection)
{
	if (viewer_connection->control_sock < 0) {
		return;
	}
	if (close(viewer_connection->control_sock)) {
		PERR("Close: %s", strerror(errno));
		viewer_connection->control_sock = -1;
	}
}

static void connection_release(struct bt_object *obj)
{
	struct bt_live_viewer_connection *conn =
		container_of(obj, struct bt_live_viewer_connection, obj);

	bt_live_viewer_connection_destroy(conn);
}

static
enum bt_value_status list_update_session(struct bt_value *results,
		const struct lttng_viewer_session *session,
		bool *_found)
{
	enum bt_value_status ret = BT_VALUE_STATUS_OK;
	struct bt_value *map = NULL;
	struct bt_value *hostname = NULL;
	struct bt_value *session_name = NULL;
	struct bt_value *btval = NULL;
	int i, len;
	bool found = false;

	len = bt_value_array_size(results);
	if (len < 0) {
		ret = BT_VALUE_STATUS_ERROR;
		goto end;
	}
	for (i = 0; i < len; i++) {
		const char *hostname_str = NULL;
		const char *session_name_str = NULL;

		map = bt_value_array_get(results, (size_t) i);
		if (!map) {
			ret = BT_VALUE_STATUS_ERROR;
			goto end;
		}
		hostname = bt_value_map_get(map, "target-hostname");
		if (!hostname) {
			ret = BT_VALUE_STATUS_ERROR;
			goto end;
		}
		session_name = bt_value_map_get(map, "session-name");
		if (!session_name) {
			ret = BT_VALUE_STATUS_ERROR;
			goto end;
		}
		ret = bt_value_string_get(hostname, &hostname_str);
		if (ret != BT_VALUE_STATUS_OK) {
			goto end;
		}
		ret = bt_value_string_get(session_name, &session_name_str);
		if (ret != BT_VALUE_STATUS_OK) {
			goto end;
		}

		if (!strcmp(session->hostname, hostname_str)
				&& !strcmp(session->session_name,
					session_name_str)) {
			int64_t val;
			uint32_t streams = be32toh(session->streams);
			uint32_t clients = be32toh(session->clients);

			found = true;

			btval = bt_value_map_get(map, "stream-count");
			if (!btval) {
				ret = BT_VALUE_STATUS_ERROR;
				goto end;
			}
			ret = bt_value_integer_get(btval, &val);
			if (ret != BT_VALUE_STATUS_OK) {
				goto end;
			}
			/* sum */
			val += streams;
			ret = bt_value_integer_set(btval, val);
			if (ret != BT_VALUE_STATUS_OK) {
				goto end;
			}
			BT_PUT(btval);

			btval = bt_value_map_get(map, "client-count");
			if (!btval) {
				ret = BT_VALUE_STATUS_ERROR;
				goto end;
			}
			ret = bt_value_integer_get(btval, &val);
			if (ret != BT_VALUE_STATUS_OK) {
				goto end;
			}
			/* max */
			val = max_t(int64_t, clients, val);
			ret = bt_value_integer_set(btval, val);
			if (ret != BT_VALUE_STATUS_OK) {
				goto end;
			}
			BT_PUT(btval);
		}

		BT_PUT(hostname);
		BT_PUT(session_name);
		BT_PUT(map);

		if (found) {
			break;
		}
	}
end:
	BT_PUT(btval);
	BT_PUT(hostname);
	BT_PUT(session_name);
	BT_PUT(map);
	*_found = found;
	return ret;
}

static
enum bt_value_status list_append_session(struct bt_value *results,
		GString *base_url,
		const struct lttng_viewer_session *session)
{
	enum bt_value_status ret = BT_VALUE_STATUS_OK;
	struct bt_value *map = NULL;
	GString *url = NULL;
	bool found = false;

	/*
	 * If the session already exists, add the stream count to it,
	 * and do max of client counts.
	 */
	ret = list_update_session(results, session, &found);
	if (ret != BT_VALUE_STATUS_OK || found) {
		goto end;
	}

	map = bt_value_map_create();
	if (!map) {
		ret = BT_VALUE_STATUS_ERROR;
		goto end;
	}

	if (base_url->len < 1) {
		ret = BT_VALUE_STATUS_ERROR;
		goto end;
	}
	/*
	 * key = "url",
	 * value = <string>,
	 */
	url = g_string_new(base_url->str);
	g_string_append(url, "/host/");
	g_string_append(url, session->hostname);
	g_string_append_c(url, '/');
	g_string_append(url, session->session_name);

	ret = bt_value_map_insert_string(map, "url", url->str);
	if (ret != BT_VALUE_STATUS_OK) {
		goto end;
	}

	/*
	 * key = "target-hostname",
	 * value = <string>,
	 */
	ret = bt_value_map_insert_string(map, "target-hostname",
		session->hostname);
	if (ret != BT_VALUE_STATUS_OK) {
		goto end;
	}

	/*
	 * key = "session-name",
	 * value = <string>,
	 */
	ret = bt_value_map_insert_string(map, "session-name",
		session->session_name);
	if (ret != BT_VALUE_STATUS_OK) {
		goto end;
	}

	/*
	 * key = "timer-us",
	 * value = <integer>,
	 */
	{
		uint32_t live_timer = be32toh(session->live_timer);

		ret = bt_value_map_insert_integer(map, "timer-us",
			live_timer);
		if (ret != BT_VALUE_STATUS_OK) {
			goto end;
		}
	}

	/*
	 * key = "stream-count",
	 * value = <integer>,
	 */
	{
		uint32_t streams = be32toh(session->streams);

		ret = bt_value_map_insert_integer(map, "stream-count",
			streams);
		if (ret != BT_VALUE_STATUS_OK) {
			goto end;
		}
	}


	/*
	 * key = "client-count",
	 * value = <integer>,
	 */
	{
		uint32_t clients = be32toh(session->clients);

		ret = bt_value_map_insert_integer(map, "client-count",
			clients);
		if (ret != BT_VALUE_STATUS_OK) {
			goto end;
		}
	}

	ret = bt_value_array_append(results, map);
end:
	if (url) {
		g_string_free(url, TRUE);
	}
	BT_PUT(map);
	return ret;
}

/*
 * Data structure returned:
 *
 * {
 *   <array> = {
 *     [n] = {
 *       <map> = {
 *         {
 *           key = "url",
 *           value = <string>,
 *         },
 *         {
 *           key = "target-hostname",
 *           value = <string>,
 *         },
 *         {
 *           key = "session-name",
 *           value = <string>,
 *         },
 *         {
 *           key = "timer-us",
 *           value = <integer>,
 *         },
 *         {
 *           key = "stream-count",
 *           value = <integer>,
 *         },
 *         {
 *           key = "client-count",
 *           value = <integer>,
 *         },
 *       },
 *     }
 *   }
 */

BT_HIDDEN
struct bt_value *bt_live_viewer_connection_list_sessions(struct bt_live_viewer_connection *viewer_connection)
{
	struct bt_value *results = NULL;
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_list_sessions list;
	uint32_t i, sessions_count;
	ssize_t ret_len;

	if (lttng_live_handshake(viewer_connection)) {
		goto error;
	}

	results = bt_value_array_create();
	if (!results) {
		fprintf(stderr, "Error creating array\n");
		goto error;
	}

	cmd.cmd = htobe32(LTTNG_VIEWER_LIST_SESSIONS);
	cmd.data_size = htobe64((uint64_t) 0);
	cmd.cmd_version = htobe32(0);

	ret_len = lttng_live_send(viewer_connection->control_sock, &cmd, sizeof(cmd));
	if (ret_len < 0) {
		fprintf(stderr, "Error sending cmd: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(cmd));

	ret_len = lttng_live_recv(viewer_connection->control_sock, &list, sizeof(list));
	if (ret_len == 0) {
		fprintf(stderr, "Remote side has closed connection\n");
		goto error;
	}
	if (ret_len < 0) {
		fprintf(stderr, "Error receiving session list: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(list));

	sessions_count = be32toh(list.sessions_count);
	for (i = 0; i < sessions_count; i++) {
		struct lttng_viewer_session lsession;

		ret_len = lttng_live_recv(viewer_connection->control_sock,
				&lsession, sizeof(lsession));
		if (ret_len == 0) {
			fprintf(stderr, "Remote side has closed connection\n");
			goto error;
		}
		if (ret_len < 0) {
			fprintf(stderr, "Error receiving session: %s\n", strerror(errno));
			goto error;
		}
		assert(ret_len == sizeof(lsession));
		lsession.hostname[LTTNG_VIEWER_HOST_NAME_MAX - 1] = '\0';
		lsession.session_name[LTTNG_VIEWER_NAME_MAX - 1] = '\0';
		if (list_append_session(results,
				viewer_connection->url, &lsession)
				!= BT_VALUE_STATUS_OK) {
			goto error;
		}
	}
	goto end;
error:
	BT_PUT(results);
end:
	return results;
}

static
int lttng_live_query_session_ids(struct lttng_live_component *lttng_live)
{
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_list_sessions list;
	struct lttng_viewer_session lsession;
	uint32_t i, sessions_count;
	ssize_t ret_len;
	uint64_t session_id;
	struct bt_live_viewer_connection *viewer_connection =
			lttng_live->viewer_connection;

	cmd.cmd = htobe32(LTTNG_VIEWER_LIST_SESSIONS);
	cmd.data_size = htobe64((uint64_t) 0);
	cmd.cmd_version = htobe32(0);

	ret_len = lttng_live_send(viewer_connection->control_sock, &cmd, sizeof(cmd));
	if (ret_len < 0) {
		PERR("Error sending cmd: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(cmd));

	ret_len = lttng_live_recv(viewer_connection->control_sock, &list, sizeof(list));
	if (ret_len == 0) {
		PERR("Remote side has closed connection\n");
		goto error;
	}
	if (ret_len < 0) {
		PERR("Error receiving session list: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(list));

	sessions_count = be32toh(list.sessions_count);
	for (i = 0; i < sessions_count; i++) {
		ret_len = lttng_live_recv(viewer_connection->control_sock,
				&lsession, sizeof(lsession));
		if (ret_len == 0) {
			PERR("Remote side has closed connection\n");
			goto error;
		}
		if (ret_len < 0) {
			PERR("Error receiving session: %s\n", strerror(errno));
			goto error;
		}
		assert(ret_len == sizeof(lsession));
		lsession.hostname[LTTNG_VIEWER_HOST_NAME_MAX - 1] = '\0';
		lsession.session_name[LTTNG_VIEWER_NAME_MAX - 1] = '\0';
		session_id = be64toh(lsession.id);

		if ((strncmp(lsession.session_name,
			viewer_connection->session_name,
			MAXNAMLEN) == 0) && (strncmp(lsession.hostname,
				viewer_connection->target_hostname,
				MAXNAMLEN) == 0)) {
			if (lttng_live_add_session(lttng_live, session_id)) {
				goto error;
			}
		}
	}

	return 0;

error:
	PERR("Unable to query session ids\n");
	return -1;
}

BT_HIDDEN
int lttng_live_create_viewer_session(struct lttng_live_component *lttng_live)
{
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_create_session_response resp;
	ssize_t ret_len;
	struct bt_live_viewer_connection *viewer_connection =
			lttng_live->viewer_connection;

	cmd.cmd = htobe32(LTTNG_VIEWER_CREATE_SESSION);
	cmd.data_size = htobe64((uint64_t) 0);
	cmd.cmd_version = htobe32(0);

	ret_len = lttng_live_send(viewer_connection->control_sock, &cmd, sizeof(cmd));
	if (ret_len < 0) {
		PERR("Error sending cmd: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(cmd));

	ret_len = lttng_live_recv(viewer_connection->control_sock, &resp, sizeof(resp));
	if (ret_len == 0) {
		PERR("Remote side has closed connection\n");
		goto error;
	}
	if (ret_len < 0) {
		PERR("Error receiving create session reply: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(resp));

	if (be32toh(resp.status) != LTTNG_VIEWER_CREATE_SESSION_OK) {
		PERR("Error creating viewer session\n");
		goto error;
	}
	if (lttng_live_query_session_ids(lttng_live)) {
		goto error;
	}

	return 0;

error:
	return -1;
}

static
int receive_streams(struct lttng_live_session *session,
		uint32_t stream_count)
{
	ssize_t ret_len;
	uint32_t i;
	struct lttng_live_component *lttng_live = session->lttng_live;
	struct bt_live_viewer_connection *viewer_connection =
			lttng_live->viewer_connection;

	PDBG("Getting %" PRIu32 " new streams:\n", stream_count);
	for (i = 0; i < stream_count; i++) {
		struct lttng_viewer_stream stream;
		struct lttng_live_stream_iterator *live_stream;
		uint64_t stream_id;
		uint64_t ctf_trace_id;

		ret_len = lttng_live_recv(viewer_connection->control_sock, &stream, sizeof(stream));
		if (ret_len == 0) {
			PERR("Remote side has closed connection\n");
			goto error;
		}
		if (ret_len < 0) {
			PERR("Error receiving stream\n");
			goto error;
		}
		assert(ret_len == sizeof(stream));
		stream.path_name[LTTNG_VIEWER_PATH_MAX - 1] = '\0';
		stream.channel_name[LTTNG_VIEWER_NAME_MAX - 1] = '\0';
		stream_id = be64toh(stream.id);
		ctf_trace_id = be64toh(stream.ctf_trace_id);

		if (stream.metadata_flag) {
			PDBG("    metadata stream %" PRIu64 " : %s/%s\n",
					stream_id, stream.path_name,
					stream.channel_name);
			if (lttng_live_metadata_create_stream(session,
					ctf_trace_id, stream_id)) {
				PERR("Error creating metadata stream\n");

				goto error;
			}
			session->lazy_stream_notif_init = true;
		} else {
			PDBG("    stream %" PRIu64 " : %s/%s\n",
					stream_id, stream.path_name,
					stream.channel_name);
			live_stream = lttng_live_stream_iterator_create(session,
				ctf_trace_id, stream_id);
			if (!live_stream) {
				PERR("Error creating stream\n");
				goto error;
			}
		}
	}
	return 0;

error:
	return -1;
}

BT_HIDDEN
int lttng_live_attach_session(struct lttng_live_session *session)
{
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_attach_session_request rq;
	struct lttng_viewer_attach_session_response rp;
	ssize_t ret_len;
	struct lttng_live_component *lttng_live = session->lttng_live;
	struct bt_live_viewer_connection *viewer_connection =
			lttng_live->viewer_connection;
	uint64_t session_id = session->id;
	uint32_t streams_count;

	if (session->attached) {
		return 0;
	}

	cmd.cmd = htobe32(LTTNG_VIEWER_ATTACH_SESSION);
	cmd.data_size = htobe64((uint64_t) sizeof(rq));
	cmd.cmd_version = htobe32(0);

	memset(&rq, 0, sizeof(rq));
	rq.session_id = htobe64(session_id);
	// TODO: add cmd line parameter to select seek beginning
	// rq.seek = htobe32(LTTNG_VIEWER_SEEK_BEGINNING);
	rq.seek = htobe32(LTTNG_VIEWER_SEEK_LAST);

	ret_len = lttng_live_send(viewer_connection->control_sock, &cmd, sizeof(cmd));
	if (ret_len < 0) {
		PERR("Error sending cmd: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(cmd));

	ret_len = lttng_live_send(viewer_connection->control_sock, &rq, sizeof(rq));
	if (ret_len < 0) {
		PERR("Error sending attach request: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(rq));

	ret_len = lttng_live_recv(viewer_connection->control_sock, &rp, sizeof(rp));
	if (ret_len == 0) {
		PERR("Remote side has closed connection\n");
		goto error;
	}
	if (ret_len < 0) {
		PERR("Error receiving attach response: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(rp));

	streams_count = be32toh(rp.streams_count);
	switch(be32toh(rp.status)) {
	case LTTNG_VIEWER_ATTACH_OK:
		break;
	case LTTNG_VIEWER_ATTACH_UNK:
		PERR("Session id %" PRIu64 " is unknown\n", session_id);
		goto error;
	case LTTNG_VIEWER_ATTACH_ALREADY:
		PERR("There is already a viewer attached to this session\n");
		goto error;
	case LTTNG_VIEWER_ATTACH_NOT_LIVE:
		PERR("Not a live session\n");
		goto error;
	case LTTNG_VIEWER_ATTACH_SEEK_ERR:
		PERR("Wrong seek parameter\n");
		goto error;
	default:
		PERR("Unknown attach return code %u\n", be32toh(rp.status));
		goto error;
	}

	/* We receive the initial list of streams. */
	if (receive_streams(session, streams_count)) {
		goto error;
	}

	session->attached = true;
	session->new_streams_needed = false;

	return 0;

error:
	return -1;
}

BT_HIDDEN
int lttng_live_detach_session(struct lttng_live_session *session)
{
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_detach_session_request rq;
	struct lttng_viewer_detach_session_response rp;
	ssize_t ret_len;
	struct lttng_live_component *lttng_live = session->lttng_live;
	struct bt_live_viewer_connection *viewer_connection =
			lttng_live->viewer_connection;
	uint64_t session_id = session->id;

	if (!session->attached) {
		return 0;
	}

	cmd.cmd = htobe32(LTTNG_VIEWER_DETACH_SESSION);
	cmd.data_size = htobe64((uint64_t) sizeof(rq));
	cmd.cmd_version = htobe32(0);

	memset(&rq, 0, sizeof(rq));
	rq.session_id = htobe64(session_id);

	ret_len = lttng_live_send(viewer_connection->control_sock, &cmd, sizeof(cmd));
	if (ret_len < 0) {
		PERR("Error sending cmd: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(cmd));

	ret_len = lttng_live_send(viewer_connection->control_sock, &rq, sizeof(rq));
	if (ret_len < 0) {
		PERR("Error sending detach request: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(rq));

	ret_len = lttng_live_recv(viewer_connection->control_sock, &rp, sizeof(rp));
	if (ret_len == 0) {
		PERR("Remote side has closed connection\n");
		goto error;
	}
	if (ret_len < 0) {
		PERR("Error receiving detach response: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(rp));

	switch(be32toh(rp.status)) {
	case LTTNG_VIEWER_DETACH_SESSION_OK:
		break;
	case LTTNG_VIEWER_DETACH_SESSION_UNK:
		PERR("Session id %" PRIu64 " is unknown\n", session_id);
		goto error;
	case LTTNG_VIEWER_DETACH_SESSION_ERR:
		PERR("Error detaching session id %" PRIu64 "\n", session_id);
		goto error;
	default:
		PERR("Unknown detach return code %u\n", be32toh(rp.status));
		goto error;
	}

	session->attached = false;

	return 0;

error:
	return -1;
}

BT_HIDDEN
ssize_t lttng_live_get_one_metadata_packet(struct lttng_live_trace *trace,
		FILE *fp)
{
	uint64_t len = 0;
	int ret;
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_get_metadata rq;
	struct lttng_viewer_metadata_packet rp;
	char *data = NULL;
	ssize_t ret_len;
	struct lttng_live_session *session = trace->session;
	struct lttng_live_component *lttng_live = session->lttng_live;
	struct lttng_live_metadata *metadata = trace->metadata;
	struct bt_live_viewer_connection *viewer_connection =
			lttng_live->viewer_connection;

	rq.stream_id = htobe64(metadata->stream_id);
	cmd.cmd = htobe32(LTTNG_VIEWER_GET_METADATA);
	cmd.data_size = htobe64((uint64_t) sizeof(rq));
	cmd.cmd_version = htobe32(0);

	ret_len = lttng_live_send(viewer_connection->control_sock, &cmd, sizeof(cmd));
	if (ret_len < 0) {
		PERR("Error sending cmd: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(cmd));

	ret_len = lttng_live_send(viewer_connection->control_sock, &rq, sizeof(rq));
	if (ret_len < 0) {
		PERR("Error sending get_metadata request: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(rq));

	ret_len = lttng_live_recv(viewer_connection->control_sock, &rp, sizeof(rp));
	if (ret_len == 0) {
		PERR("Remote side has closed connection\n");
		goto error;
	}
	if (ret_len < 0) {
		PERR("Error receiving get_metadata response: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(rp));

	switch (be32toh(rp.status)) {
		case LTTNG_VIEWER_METADATA_OK:
			PDBG("get_metadata : OK\n");
			break;
		case LTTNG_VIEWER_NO_NEW_METADATA:
			PDBG("get_metadata : NO NEW\n");
			ret = 0;
			goto end;
		case LTTNG_VIEWER_METADATA_ERR:
			PDBG("get_metadata : ERR\n");
			goto error;
		default:
			PDBG("get_metadata : UNKNOWN\n");
			goto error;
	}

	len = be64toh(rp.len);
	PDBG("Writing %" PRIu64" bytes to metadata\n", len);
	if (len <= 0) {
		goto error;
	}

	data = zmalloc(len);
	if (!data) {
		PERR("relay data zmalloc: %s", strerror(errno));
		goto error;
	}
	ret_len = lttng_live_recv(viewer_connection->control_sock, data, len);
	if (ret_len == 0) {
		PERR("[error] Remote side has closed connection\n");
		goto error_free_data;
	}
	if (ret_len < 0) {
		PERR("[error] Error receiving trace packet: %s", strerror(errno));
		goto error_free_data;
	}
	assert(ret_len == len);

	do {
		ret_len = fwrite(data, 1, len, fp);
	} while (ret_len < 0 && errno == EINTR);
	if (ret_len < 0) {
		PERR("[error] Writing in the metadata fp\n");
		goto error_free_data;
	}
	assert(ret_len == len);
	free(data);
	ret = len;
end:
	return ret;

error_free_data:
	free(data);
error:
	return -1;
}

/*
 * Assign the fields from a lttng_viewer_index to a packet_index.
 */
static
void lttng_index_to_packet_index(struct lttng_viewer_index *lindex,
		struct packet_index *pindex)
{
	assert(lindex);
	assert(pindex);

	pindex->offset = be64toh(lindex->offset);
	pindex->packet_size = be64toh(lindex->packet_size);
	pindex->content_size = be64toh(lindex->content_size);
	pindex->ts_cycles.timestamp_begin = be64toh(lindex->timestamp_begin);
	pindex->ts_cycles.timestamp_end = be64toh(lindex->timestamp_end);
	pindex->events_discarded = be64toh(lindex->events_discarded);
}

BT_HIDDEN
enum bt_ctf_lttng_live_iterator_status lttng_live_get_next_index(struct lttng_live_component *lttng_live,
		struct lttng_live_stream_iterator *stream,
		struct packet_index *index)
{
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_get_next_index rq;
	ssize_t ret_len;
	struct lttng_viewer_index rp;
	uint32_t flags, status;
	enum bt_ctf_lttng_live_iterator_status retstatus =
			BT_CTF_LTTNG_LIVE_ITERATOR_STATUS_OK;
	struct bt_live_viewer_connection *viewer_connection =
			lttng_live->viewer_connection;
	struct lttng_live_trace *trace = stream->trace;

	cmd.cmd = htobe32(LTTNG_VIEWER_GET_NEXT_INDEX);
	cmd.data_size = htobe64((uint64_t) sizeof(rq));
	cmd.cmd_version = htobe32(0);

	memset(&rq, 0, sizeof(rq));
	rq.stream_id = htobe64(stream->viewer_stream_id);

	ret_len = lttng_live_send(viewer_connection->control_sock, &cmd, sizeof(cmd));
	if (ret_len < 0) {
		PERR("Error sending cmd: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(cmd));

	ret_len = lttng_live_send(viewer_connection->control_sock, &rq, sizeof(rq));
	if (ret_len < 0) {
		PERR("Error sending get_next_index request: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(rq));

	ret_len = lttng_live_recv(viewer_connection->control_sock, &rp, sizeof(rp));
	if (ret_len == 0) {
		PERR("Remote side has closed connection\n");
		goto error;
	}
	if (ret_len < 0) {
		PERR("Error receiving get_next_index response: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(rp));

	flags = be32toh(rp.flags);
	status = be32toh(rp.status);

	switch (status) {
	case LTTNG_VIEWER_INDEX_INACTIVE:
	{
		uint64_t ctf_stream_class_id;

		PDBG("get_next_index: inactive\n");
		memset(index, 0, sizeof(struct packet_index));
		index->ts_cycles.timestamp_end = be64toh(rp.timestamp_end);
		stream->current_inactivity_timestamp = index->ts_cycles.timestamp_end;
		ctf_stream_class_id = be64toh(rp.stream_id);
		if (stream->ctf_stream_class_id != -1ULL) {
			assert(stream->ctf_stream_class_id ==
				ctf_stream_class_id);
		} else {
			stream->ctf_stream_class_id = ctf_stream_class_id;
		}
		stream->state = LTTNG_LIVE_STREAM_QUIESCENT;
		break;
	}
	case LTTNG_VIEWER_INDEX_OK:
	{
		uint64_t ctf_stream_class_id;

		PDBG("get_next_index: OK\n");
		lttng_index_to_packet_index(&rp, index);
		ctf_stream_class_id = be64toh(rp.stream_id);
		if (stream->ctf_stream_class_id != -1ULL) {
			assert(stream->ctf_stream_class_id ==
				ctf_stream_class_id);
		} else {
			stream->ctf_stream_class_id = ctf_stream_class_id;
		}

		stream->state = LTTNG_LIVE_STREAM_ACTIVE_DATA;
		stream->current_packet_end_timestamp =
			index->ts_cycles.timestamp_end;

		if (flags & LTTNG_VIEWER_FLAG_NEW_METADATA) {
			PDBG("get_next_index: new metadata needed\n");
			trace->new_metadata_needed = true;
		}
		if (flags & LTTNG_VIEWER_FLAG_NEW_STREAM) {
			PDBG("get_next_index: new streams needed\n");
			lttng_live_need_new_streams(lttng_live);
		}
		break;
	}
	case LTTNG_VIEWER_INDEX_RETRY:
		PDBG("get_next_index: retry\n");
		memset(index, 0, sizeof(struct packet_index));
		retstatus = BT_CTF_LTTNG_LIVE_ITERATOR_STATUS_AGAIN;
		stream->state = LTTNG_LIVE_STREAM_ACTIVE_NO_DATA;
		goto end;
	case LTTNG_VIEWER_INDEX_HUP:
		PDBG("get_next_index: stream hung up\n");
		memset(index, 0, sizeof(struct packet_index));
		index->offset = EOF;
		retstatus = BT_CTF_LTTNG_LIVE_ITERATOR_STATUS_END;
		stream->state = LTTNG_LIVE_STREAM_EOF;
		break;
	case LTTNG_VIEWER_INDEX_ERR:
		PERR("get_next_index: error\n");
		memset(index, 0, sizeof(struct packet_index));
		stream->state = LTTNG_LIVE_STREAM_ACTIVE_NO_DATA;
		goto error;
	default:
		PERR("get_next_index: unkwown value\n");
		memset(index, 0, sizeof(struct packet_index));
		stream->state = LTTNG_LIVE_STREAM_ACTIVE_NO_DATA;
		goto error;
	}
end:
	return retstatus;

error:
	retstatus = BT_CTF_LTTNG_LIVE_ITERATOR_STATUS_ERROR;
	return retstatus;
}

BT_HIDDEN
enum bt_ctf_notif_iter_medium_status lttng_live_get_stream_bytes(struct lttng_live_component *lttng_live,
		struct lttng_live_stream_iterator *stream, uint8_t *buf, uint64_t offset,
		uint64_t req_len, uint64_t *recv_len)
{
	enum bt_ctf_notif_iter_medium_status retstatus = BT_CTF_NOTIF_ITER_MEDIUM_STATUS_OK;
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_get_packet rq;
	struct lttng_viewer_trace_packet rp;
	ssize_t ret_len;
	uint32_t flags, status;
	struct bt_live_viewer_connection *viewer_connection =
			lttng_live->viewer_connection;
	struct lttng_live_trace *trace = stream->trace;

	PDBG("lttng_live_get_stream_bytes: offset=%" PRIu64 ", req_len=%" PRIu64 "\n",
			offset, req_len);
	cmd.cmd = htobe32(LTTNG_VIEWER_GET_PACKET);
	cmd.data_size = htobe64((uint64_t) sizeof(rq));
	cmd.cmd_version = htobe32(0);

	memset(&rq, 0, sizeof(rq));
	rq.stream_id = htobe64(stream->viewer_stream_id);
	rq.offset = htobe64(offset);
	rq.len = htobe32(req_len);

	ret_len = lttng_live_send(viewer_connection->control_sock, &cmd, sizeof(cmd));
	if (ret_len < 0) {
		PERR("Error sending cmd: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(cmd));

	ret_len = lttng_live_send(viewer_connection->control_sock, &rq, sizeof(rq));
	if (ret_len < 0) {
		PERR("Error sending get_data request: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(rq));

	ret_len = lttng_live_recv(viewer_connection->control_sock, &rp, sizeof(rp));
	if (ret_len == 0) {
		PERR("Remote side has closed connection\n");
		goto error;
	}
	if (ret_len < 0) {
		PERR("Error receiving get_data response: %s\n", strerror(errno));
		goto error;
	}
	if (ret_len != sizeof(rp)) {
		PERR("[error] get_data_packet: expected %zu"
				", received %zd\n", sizeof(rp),
				ret_len);
		goto error;
	}

	flags = be32toh(rp.flags);
	status = be32toh(rp.status);

	switch (status) {
	case LTTNG_VIEWER_GET_PACKET_OK:
		req_len = be32toh(rp.len);
		PDBG("get_data_packet: Ok, packet size : %" PRIu64 "\n", req_len);
		break;
	case LTTNG_VIEWER_GET_PACKET_RETRY:
		/* Unimplemented by relay daemon */
		PDBG("get_data_packet: retry\n");
		retstatus = BT_CTF_NOTIF_ITER_MEDIUM_STATUS_AGAIN;
		goto end;
	case LTTNG_VIEWER_GET_PACKET_ERR:
		if (flags & LTTNG_VIEWER_FLAG_NEW_METADATA) {
			PDBG("get_data_packet: new metadata needed, try again later\n");
			trace->new_metadata_needed = true;
		}
		if (flags & LTTNG_VIEWER_FLAG_NEW_STREAM) {
			PDBG("get_data_packet: new streams needed, try again later\n");
			lttng_live_need_new_streams(lttng_live);
		}
		if (flags & (LTTNG_VIEWER_FLAG_NEW_METADATA
				| LTTNG_VIEWER_FLAG_NEW_STREAM)) {
			retstatus = BT_CTF_NOTIF_ITER_MEDIUM_STATUS_AGAIN;
			goto end;
		}
		PERR("get_data_packet: error\n");
		goto error;
	case LTTNG_VIEWER_GET_PACKET_EOF:
		retstatus = BT_CTF_NOTIF_ITER_MEDIUM_STATUS_EOF;
		goto end;
	default:
		PDBG("get_data_packet: unknown\n");
		goto error;
	}

	if (req_len == 0) {
		goto error;
	}

	ret_len = lttng_live_recv(viewer_connection->control_sock, buf, req_len);
	if (ret_len == 0) {
		PERR("Remote side has closed connection\n");
		goto error;
	}
	if (ret_len < 0) {
		PERR("Error receiving trace packet: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == req_len);
	*recv_len = ret_len;
end:
	return retstatus;

error:
	retstatus = BT_CTF_NOTIF_ITER_MEDIUM_STATUS_ERROR;
	return retstatus;
}

/*
 * Request new streams for a session.
 */
BT_HIDDEN
enum bt_ctf_lttng_live_iterator_status lttng_live_get_new_streams(
		struct lttng_live_session *session)
{
	enum bt_ctf_lttng_live_iterator_status status =
			BT_CTF_LTTNG_LIVE_ITERATOR_STATUS_OK;
	struct lttng_viewer_cmd cmd;
	struct lttng_viewer_new_streams_request rq;
	struct lttng_viewer_new_streams_response rp;
	ssize_t ret_len;
	struct lttng_live_component *lttng_live = session->lttng_live;
	struct bt_live_viewer_connection *viewer_connection =
			lttng_live->viewer_connection;
	uint32_t streams_count;

	if (!session->new_streams_needed) {
		return BT_CTF_LTTNG_LIVE_ITERATOR_STATUS_OK;
	}

	cmd.cmd = htobe32(LTTNG_VIEWER_GET_NEW_STREAMS);
	cmd.data_size = htobe64((uint64_t) sizeof(rq));
	cmd.cmd_version = htobe32(0);

	memset(&rq, 0, sizeof(rq));
	rq.session_id = htobe64(session->id);

	ret_len = lttng_live_send(viewer_connection->control_sock, &cmd, sizeof(cmd));
	if (ret_len < 0) {
		PERR("Error sending cmd: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(cmd));

	ret_len = lttng_live_send(viewer_connection->control_sock, &rq, sizeof(rq));
	if (ret_len < 0) {
		PERR("Error sending get_new_streams request: %s\n", strerror(errno));
		goto error;
	}
	assert(ret_len == sizeof(rq));

	ret_len = lttng_live_recv(viewer_connection->control_sock, &rp, sizeof(rp));
	if (ret_len == 0) {
		PERR("Remote side has closed connection\n");
		goto error;
	}
	if (ret_len < 0) {
		PERR("Error receiving get_new_streams response\n");
		goto error;
	}
	assert(ret_len == sizeof(rp));

	streams_count = be32toh(rp.streams_count);

	switch(be32toh(rp.status)) {
	case LTTNG_VIEWER_NEW_STREAMS_OK:
		session->new_streams_needed = false;
		break;
	case LTTNG_VIEWER_NEW_STREAMS_NO_NEW:
		session->new_streams_needed = false;
		goto end;
	case LTTNG_VIEWER_NEW_STREAMS_HUP:
		session->new_streams_needed = false;
		session->closed = true;
		status = BT_CTF_LTTNG_LIVE_ITERATOR_STATUS_END;
		goto end;
	case LTTNG_VIEWER_NEW_STREAMS_ERR:
		PERR("get_new_streams error\n");
		goto error;
	default:
		PERR("Unknown return code %u\n", be32toh(rp.status));
		goto error;
	}

	if (receive_streams(session, streams_count)) {
		goto error;
	}
end:
	return status;

error:
	status = BT_CTF_LTTNG_LIVE_ITERATOR_STATUS_ERROR;
	return status;
}

BT_HIDDEN
struct bt_live_viewer_connection *
	bt_live_viewer_connection_create(const char *url, FILE *error_fp)
{
	struct bt_live_viewer_connection *viewer_connection;

	viewer_connection = g_new0(struct bt_live_viewer_connection, 1);

	bt_object_init(&viewer_connection->obj, connection_release);
	viewer_connection->control_sock = -1;
	viewer_connection->port = -1;
	viewer_connection->error_fp = error_fp;
	viewer_connection->url = g_string_new(url);
	if (!viewer_connection->url) {
		goto error;
	}

	PDBG("Establishing connection to url \"%s\"...\n", url);
	if (lttng_live_connect_viewer(viewer_connection)) {
		goto error_report;
	}
	PDBG("Connection to url \"%s\" is established\n", url);
	return viewer_connection;

error_report:
	printf_verbose("Failure to establish connection to url \"%s\"\n", url);
error:
	g_free(viewer_connection);
	return NULL;
}

BT_HIDDEN
void bt_live_viewer_connection_destroy(struct bt_live_viewer_connection *viewer_connection)
{
	PDBG("Closing connection to url \"%s\"\n", viewer_connection->url->str);
	lttng_live_disconnect_viewer(viewer_connection);
	g_string_free(viewer_connection->url, TRUE);
	g_free(viewer_connection);
}
