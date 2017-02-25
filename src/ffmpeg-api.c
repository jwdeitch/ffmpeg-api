#include <kore/kore.h>
#include <kore/http.h>

#include "assets.h"

int page(struct http_request *);
int page_ws_connect(struct http_request *);

void websocket_connect(struct connection *);
void websocket_disconnect(struct connection *);
void websocket_message(struct connection *,
					   u_int8_t, void *, size_t);

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