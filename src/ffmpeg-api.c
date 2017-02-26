#include <kore/kore.h>
#include <kore/http.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include "assets.h"
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>

/*
 * Handles uploading, and transcoding of assets
 * Will broadcast transcoding progress over websocket
 *
 * pv is required for google cloud functions.
 * It is compiled against amd64 linux
 *
 * kore.io
 *
 */

struct ffmpeg_params {
	char output[1000];
	char input[1000];
	char rnd_str[6];
	char output_fn[1000];
};

int page(struct http_request *);
int page_ws_connect(struct http_request *);

void websocket_connect(struct connection *);
void websocket_disconnect(struct connection *);
void transcode_video();
char *removeExtension(char *mystr);
void uploadToS3(struct ffmpeg_params *params);
int upload(struct http_request *req);
void rand_str(char *dest, size_t length);

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
//	uploadToS3("wefwef.mp4");
//	return 1;

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

	/*
	 * I am producing the input and output file
	 * paths to ffmpeg below. The output file path should be
	 * prefixed with a random dir name to avoid collisions with
	 * other uploads. This will also allow upload to S3 w/o changing the file name
	 */

	char *prefix[6];
	rand_str(prefix, 5);

	char *cwd[1024];
	getcwd(cwd, sizeof(cwd));

	char *outputFilePath[1000];
	strcpy(outputFilePath, cwd);
	strcat(outputFilePath, "/");
	strcat(outputFilePath, prefix);
	strcat(outputFilePath, "/");

	char *outputFileName[1000];
	strcat(outputFileName, removeExtension(file->filename));
	strcat(outputFileName, ".mp4");

	char *outputFileFull[1000];
	strcpy(outputFileFull, outputFilePath);
	strcat(outputFileFull, outputFileName);

	struct stat st = {0};

	if (stat(outputFilePath, &st) == -1) {
		mkdir(outputFilePath, 0700);
	}

	char *symlinkpath = file->filename;
	char actualpath[PATH_MAX + 1];
	char *ptr;
	ptr = realpath(symlinkpath, actualpath);

	struct ffmpeg_params *t_params;
	t_params = malloc(sizeof(*t_params));
	strcpy(t_params->output, outputFileFull);
	strcpy(t_params->rnd_str, prefix);
	strcpy(t_params->output_fn, outputFileName);
	strcpy(t_params->input, ptr);

	pthread_t tid;
	pthread_create(&tid, NULL, transcode_video, t_params);

	return (KORE_RESULT_OK);
}

void transcode_video(struct ffmpeg_params *params) {
	int buf_size = 256;

	char *cmd[10000];
	strcpy(cmd, "(pv ");
	strcat(cmd, params->input);
	strcat(cmd, " --numeric | ffmpeg -i pipe:0 -v warning ");
	strcat(cmd, params->output);
	strcat(cmd, ") 2>&1");

	kore_log(LOG_WARNING, "transcoding instruction: (%s)", cmd);

	char buf[buf_size];
	FILE *fp;

	if ((fp = popen(cmd, "r")) == NULL) {
		kore_log(LOG_NOTICE, "Error opening pipe!\n");
	}

	while (fgets(buf, buf_size, fp) != NULL) {
		char sbuf[15];
		sprintf(sbuf, "%s", buf);

		kore_websocket_broadcast(c, WEBSOCKET_OP_TEXT, sbuf, sizeof(sbuf), WEBSOCKET_BROADCAST_GLOBAL);
		kore_log(LOG_NOTICE, "output: %s", sbuf);

		if (strcmp(sbuf, "100\n") == 0) {
			sleep(1); // we need to allow ffmpeg to release the file handle before uploading to s3
			uploadToS3(params);
		}
	}

	if (pclose(fp)) {
		kore_log(LOG_NOTICE, "Command not found or exited with error status\n");
	}
}

void uploadToS3(struct ffmpeg_params *params) {
	int buf_size = 256;

	char *s3Path[1000];
	strcpy(s3Path, params->rnd_str);
	strcat(s3Path, "/");
	strcat(s3Path, params->output_fn);

	char *cmd[10000];
	strcpy(cmd, "./moveToS3.sh ");
	strcat(cmd, s3Path);

	kore_log(LOG_WARNING, "upload instruction: (%s)", cmd);

	char buf[buf_size];
	FILE *fp;

	if ((fp = popen(cmd, "r")) == NULL) {
		kore_log(LOG_NOTICE, "Error opening pipe!\n");
	}

	while (fgets(buf, buf_size, fp) != NULL) {
		char sbuf[15];
		sprintf(sbuf, "%s", buf);

		char spath[150];
		sprintf(spath, "%s", s3Path);

		if (strcmp(sbuf, "200")) {

			kore_websocket_broadcast(c, WEBSOCKET_OP_TEXT, spath, sizeof(spath),
									 WEBSOCKET_BROADCAST_GLOBAL);
			kore_log(LOG_NOTICE, "OUTPUT: %s", buf);
		} else {
			kore_log(LOG_ERR, "CURL ERROR UPLOAD TO S3: %s", buf);
			return;
		}
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

// http://stackoverflow.com/a/15768317/4603498
void rand_str(char *dest, size_t length) {
	srand ( time(NULL) );
	char charset[] = "0123456789"
			"abcdefghijklmnopqrstuvwxyz"
			"ABCDEFGHIJKLMNOPQRSTUVWXYZ";

	while (length-- > 0) {
		size_t index = (double) rand() / RAND_MAX * (sizeof charset - 1);
		*dest++ = charset[index];
	}
	*dest = '\0';
}