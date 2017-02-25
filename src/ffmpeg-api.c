#include <kore/kore.h>
#include <kore/http.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include "assets.h"


/*
 * Handles uploading, and transcoding of assets
 *
 *
 * Most of this is from the Kore.io framework
 */


int page(struct http_request *);
int page_ws_connect(struct http_request *);

void websocket_connect(struct connection *);
void websocket_disconnect(struct connection *);
void transcode_video();
char *removeExtension(char *mystr);

int upload(struct http_request *req);

/* Websocket callbacks. */
struct kore_wscbs wscbs = {
		websocket_connect,
		websocket_disconnect
};

// Websocket Connection
struct connection *c;

/* Called whenever we get a new websocket connection. */
void
websocket_connect(struct connection *c) {
	kore_log(LOG_NOTICE, "%p: connected", c);
	c = c;
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

	char *symlinkpath = file->filename;
	char actualpath[PATH_MAX + 1];
	char *ptr;
	ptr = realpath(symlinkpath, actualpath);

	pthread_t tid;
	pthread_create(&tid, NULL, transcode_video, ptr);

	return (KORE_RESULT_OK);
}

// pv inputfilepath--numeric | ffmpeg -i pipe:0 -v warning outputfilepath
void transcode_video(const char *fpath) {
	int buf_size = 256;

	char *newFileName[10000];
	strcpy(newFileName, removeExtension(fpath));
	strcat(newFileName, ".mp4");

	char *cmd[10000];
	strcpy(cmd, "pv ");
	strcat(cmd, fpath);
	strcat(cmd, " --numeric | ffmpeg -i pipe:0 -v warning ");
	strcat(cmd, newFileName);

	kore_log(LOG_WARNING, "transcoding instruction: (%s)", cmd);

	if (strcmp(fpath, newFileName) == 0) {
		kore_log(LOG_WARNING, "Skipping job - file extensions are the same");
		kore_websocket_broadcast(c, 1, 100, sizeof(100), WEBSOCKET_BROADCAST_GLOBAL);
		return;
	}


	char buf[buf_size];
	FILE *fp;


	if ((fp = popen(cmd, "r")) == NULL) {
		kore_log(LOG_NOTICE, "Error opening pipe!\n");
	}

	while (fgets(buf, buf_size, fp) != NULL) {
		kore_websocket_broadcast(c, 1, buf, sizeof(buf), WEBSOCKET_BROADCAST_GLOBAL);
		kore_log(LOG_NOTICE, "OUTPUT: %s", buf);
	}

	if (pclose(fp)) {
		kore_log(LOG_NOTICE, "Command not found or exited with error status\n");
	}
}

// http://stackoverflow.com/a/2736841/4603498
char *removeExtension(char *mystr) {
	char *retstr;
	char *lastdot;
	if (mystr == NULL)
		return NULL;
	if ((retstr = malloc(strlen(mystr) + 1)) == NULL)
		return NULL;
	strcpy(retstr, mystr);
	lastdot = strrchr(retstr, '.');
	if (lastdot != NULL)
		*lastdot = '\0';
	return retstr;
}