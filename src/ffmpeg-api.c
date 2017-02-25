#include <kore/kore.h>
#include <kore/http.h>
#include <fcntl.h>
#include <unistd.h>
#include "assets.h"

// pv RAW-VIDEO-INPUT-UNTOUCHED.mp4 --numeric | ffmpeg -i pipe:0 -v warning test234241.mp4
int page(struct http_request *);
int page_ws_connect(struct http_request *);

void websocket_connect(struct connection *);
void websocket_disconnect(struct connection *);
void websocket_message(struct connection *,
					   u_int8_t, void *, size_t);


int upload(struct http_request *req);

/* Websocket callbacks. */
struct kore_wscbs wscbs = {
		websocket_connect,
		websocket_message,
		websocket_disconnect
};

/* Called whenever we get a new websocket connection. */
void
websocket_connect(struct connection *c) {
	kore_log(LOG_NOTICE, "%p: connected", c);
}

void
websocket_message(struct connection *c, u_int8_t op, void *data, size_t len) {
	kore_log(LOG_NOTICE, "%p: received message", data);
	kore_websocket_broadcast(c, op, data, len, WEBSOCKET_BROADCAST_GLOBAL);
}

void
websocket_disconnect(struct connection *c) {
	kore_log(LOG_NOTICE, "%p: disconnecting", c);
}

int
page(struct http_request *req) {
	http_response_header(req, "content-type", "text/html");
	http_response(req, 200, asset_index_html, asset_len_index_html);

	return (KORE_RESULT_OK);
}

int
page_ws_connect(struct http_request *req) {
	/* Perform the websocket handshake, passing our callbacks. */
	kore_websocket_handshake(req, &wscbs);

	return (KORE_RESULT_OK);
}

int
upload(struct http_request *req) {
	int fd;
	struct http_file *file;
	u_int8_t buf[BUFSIZ];
	ssize_t ret, written;
	char *fname;
	struct kore_buf *in;

	/* Only deal with POSTs. */
	if (req->method != HTTP_METHOD_POST) {
		http_response(req, 405, NULL, 0);
		return (KORE_RESULT_OK);
	}

	/* Parse the multipart data that was present. */
	http_populate_multipart_form(req);

	/* Find our file. */
	if ((file = http_file_lookup(req, "file")) == NULL) {
		http_response(req, 400, "NO INPUT FILE PROVIDED", sizeof("NO INPUT FILE PROVIDED"));
		return (KORE_RESULT_OK);
	}

	/* Open dump file where we will write file contents. */
	fd = open(file->filename, O_CREAT | O_TRUNC | O_WRONLY, 0700);
	if (fd == -1) {
		http_response(req, 500, NULL, 0);
		return (KORE_RESULT_OK);
	}

	/* While we have data from http_file_read(), write it. */
	/* Alternatively you could look at file->offset and file->length. */
	ret = KORE_RESULT_ERROR;
	for (;;) {
		ret = http_file_read(file, buf, sizeof(buf));
		if (ret == -1) {
			kore_log(LOG_ERR, "failed to read from file");
			http_response(req, 500, NULL, 0);
			goto cleanup;
		}

		if (ret == 0)
			break;

		written = write(fd, buf, ret);
		if (written == -1) {
			kore_log(LOG_ERR, "write(%s): %s",
					 file->filename, errno_s);
			http_response(req, 500, NULL, 0);
			goto cleanup;
		}

		if (written != ret) {
			kore_log(LOG_ERR, "partial write on %s",
					 file->filename);
			http_response(req, 500, NULL, 0);
			goto cleanup;
		}
	}

	ret = KORE_RESULT_OK;
	http_response(req, 200, NULL, 0);
	kore_log(LOG_INFO, "file '%s' successfully received",
			 file->filename);

	cleanup:
	if (close(fd) == -1)
		kore_log(LOG_WARNING, "close(%s): %s", file->filename, errno_s);

	if (ret == KORE_RESULT_ERROR) {
		if (unlink(file->filename) == -1) {
			kore_log(LOG_WARNING, "unlink(%s): %s",
					 file->filename, errno_s);
		}
		ret = KORE_RESULT_OK;
	}

	return (KORE_RESULT_OK);
}
