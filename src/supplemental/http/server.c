//
// Copyright 2018 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "core/nng_impl.h"
#include "supplemental/tls/tls.h"

#include "http.h"

static int  http_server_sys_init(void);
static void http_server_sys_fini(void);

static nni_initializer http_server_initializer = {
	.i_init = http_server_sys_init,
	.i_fini = http_server_sys_fini,
	.i_once = 0,
};

typedef struct http_handler {
	nni_list_node node;
	void *        h_arg;
	char *        h_path;
	char *        h_method;
	char *        h_host;
	bool          h_is_upgrader;
	bool          h_is_dir;
	int           h_refcnt;
	void (*h_cb)(nni_aio *);
	void (*h_free)(void *);
} http_handler;

typedef struct http_sconn {
	nni_list_node    node;
	nni_http *       http;
	nni_http_server *server;
	nni_http_req *   req;
	nni_http_res *   res;
	bool             close;
	bool             closed;
	bool             finished;
	nni_aio *        cbaio;
	nni_aio *        rxaio;
	nni_aio *        txaio;
	nni_aio *        txdataio;
	nni_reap_item    reap;
} http_sconn;

struct nni_http_server {
	nng_sockaddr     addr;
	nni_list_node    node;
	int              refcnt;
	int              starts;
	nni_list         handlers;
	nni_list         conns;
	nni_mtx          mtx;
	nni_cv           cv;
	bool             closed;
	nng_tls_config * tls;
	nni_aio *        accaio;
	nni_plat_tcp_ep *tep;
	nni_url *        url;
};

static nni_list http_servers;
static nni_mtx  http_servers_lk;
static void     http_handler_fini(http_handler *);

static void
http_sconn_reap(void *arg)
{
	http_sconn *     sc = arg;
	nni_http_server *s  = sc->server;
	NNI_ASSERT(!sc->finished);
	sc->finished = true;
	nni_aio_stop(sc->rxaio);
	nni_aio_stop(sc->txaio);
	nni_aio_stop(sc->txdataio);
	nni_aio_stop(sc->cbaio);

	if (sc->http != NULL) {
		nni_http_fini(sc->http);
	}
	if (sc->req != NULL) {
		nni_http_req_fini(sc->req);
	}
	if (sc->res != NULL) {
		nni_http_res_fini(sc->res);
	}
	nni_aio_fini(sc->rxaio);
	nni_aio_fini(sc->txaio);
	nni_aio_fini(sc->txdataio);
	nni_aio_fini(sc->cbaio);

	// Now it is safe to release our reference on the server.
	nni_mtx_lock(&s->mtx);
	if (nni_list_node_active(&sc->node)) {
		nni_list_remove(&s->conns, sc);
		if (nni_list_empty(&s->conns)) {
			nni_cv_wake(&s->cv);
		}
	}
	nni_mtx_unlock(&s->mtx);

	NNI_FREE_STRUCT(sc);
}

static void
http_sconn_fini(http_sconn *sc)
{
	nni_reap(&sc->reap, http_sconn_reap, sc);
}

static void
http_sconn_close_locked(http_sconn *sc)
{
	nni_http_server *s;
	s = sc->server;
	nni_http *h;

	if (sc->closed) {
		return;
	}
	NNI_ASSERT(!sc->finished);

	sc->closed = true;
	nni_aio_cancel(sc->rxaio, NNG_ECLOSED);
	nni_aio_cancel(sc->txaio, NNG_ECLOSED);
	nni_aio_cancel(sc->txdataio, NNG_ECLOSED);
	nni_aio_cancel(sc->cbaio, NNG_ECLOSED);

	if ((h = sc->http) != NULL) {
		nni_http_close(h);
	}
	http_sconn_fini(sc);
}

static void
http_sconn_close(http_sconn *sc)
{
	nni_http_server *s;
	s = sc->server;

	nni_mtx_lock(&s->mtx);
	http_sconn_close_locked(sc);
	nni_mtx_unlock(&s->mtx);
}

static void
http_sconn_txdatdone(void *arg)
{
	http_sconn *sc  = arg;
	nni_aio *   aio = sc->txdataio;

	if (nni_aio_result(aio) != 0) {
		http_sconn_close(sc);
		return;
	}

	if (sc->res != NULL) {
		nni_http_res_fini(sc->res);
		sc->res = NULL;
	}

	if (sc->close) {
		http_sconn_close(sc);
		return;
	}

	nni_http_req_reset(sc->req);
	nni_http_read_req(sc->http, sc->req, sc->rxaio);
}

static void
http_sconn_txdone(void *arg)
{
	http_sconn *sc  = arg;
	nni_aio *   aio = sc->txaio;
	int         rv;
	void *      data;
	size_t      size;

	if ((rv = nni_aio_result(aio)) != 0) {
		http_sconn_close(sc);
		return;
	}

	// For HEAD requests, we just treat like "GET" but don't send
	// the data.  (Required per HTTP.)
	if (strcmp(nni_http_req_get_method(sc->req), "HEAD") == 0) {
		size = 0;
	} else {
		nni_http_res_get_data(sc->res, &data, &size);
	}
	if (size) {
		// Submit data.
		sc->txdataio->a_niov           = 1;
		sc->txdataio->a_iov[0].iov_buf = data;
		sc->txdataio->a_iov[0].iov_len = size;
		nni_http_write_full(sc->http, sc->txdataio);
		return;
	}

	if (sc->close) {
		http_sconn_close(sc);
		return;
	}

	if (sc->res != NULL) {
		nni_http_res_fini(sc->res);
		sc->res = NULL;
	}
	nni_http_req_reset(sc->req);
	nni_http_read_req(sc->http, sc->req, sc->rxaio);
}

static char
http_hexval(char c)
{
	if ((c >= '0') && (c <= '9')) {
		return (c - '0');
	}
	if ((c >= 'a') && (c <= 'f')) {
		return ((c - 'a') + 10);
	}
	if ((c >= 'A') && (c <= 'F')) {
		return ((c - 'A') + 10);
	}
	return (0);
}

static char *
http_uri_canonify(char *path)
{
	char *tmp;
	char *dst;

	// Chomp off query string.
	if ((tmp = strchr(path, '?')) != NULL) {
		*tmp = '\0';
	}
	// If the URI was absolute, make it relative.
	if ((nni_strncasecmp(path, "http://", strlen("http://")) == 0) ||
	    (nni_strncasecmp(path, "https://", strlen("https://")) == 0)) {
		// Skip past the ://
		path = strchr(path, ':');
		path += 3;

		// scan for the end of the host, distinguished by a /
		// path delimiter.  There might not be one, in which case
		// the whole thing is the host and we assume the path is
		// just /.
		if ((path = strchr(path, '/')) == NULL) {
			return ("/");
		}
	}

	// Now we have to unescape things.  Unescaping is a shrinking
	// operation (strictly), so this is safe.  This is just URL decode.
	// Note that paths with an embedded NUL are going to be treated as
	// though truncated.  Don't be that guy that sends %00 in a URL.
	tmp = path;
	dst = path;
	while (*tmp != '\0') {
		char c;
		if ((c = *tmp) != '%') {
			*dst++ = c;
			tmp++;
			continue;
		}
		if (isxdigit(tmp[1]) && isxdigit(tmp[2])) {
			c = http_hexval(tmp[1]);
			c *= 16;
			c += http_hexval(tmp[2]);
			*dst++ = c;
			tmp += 3;
		}
		// garbage in, garbage out
		*dst++ = c;
		tmp++;
	}
	*dst = '\0';
	return (path);
}

static void
http_sconn_error(http_sconn *sc, uint16_t err)
{
	nni_http_res *res;

	if (nni_http_res_init_error(&res, err) != 0) {
		http_sconn_close(sc);
		return;
	}

	if (sc->close) {
		if (nni_http_res_set_header(res, "Connection", "close") != 0) {
			http_sconn_close(sc);
		}
	}
	sc->res = res;
	nni_http_write_res(sc->http, res, sc->txaio);
}

static void
http_sconn_rxdone(void *arg)
{
	http_sconn *     sc  = arg;
	nni_http_server *s   = sc->server;
	nni_aio *        aio = sc->rxaio;
	int              rv;
	http_handler *   h;
	const char *     val;
	nni_http_req *   req = sc->req;
	char *           uri;
	size_t           urisz;
	char *           path;
	bool             badmeth = false;

	if ((rv = nni_aio_result(aio)) != 0) {
		http_sconn_close(sc);
		return;
	}

	// Validate the request -- it has to at least look like HTTP 1.x
	// We flatly refuse to deal with HTTP 0.9, and we can't cope with
	// HTTP/2.
	if ((val = nni_http_req_get_version(req)) == NULL) {
		sc->close = true;
		http_sconn_error(sc, NNI_HTTP_STATUS_BAD_REQUEST);
		return;
	}
	if (strncmp(val, "HTTP/1.", 7) != 0) {
		sc->close = true;
		http_sconn_error(sc, NNI_HTTP_STATUS_HTTP_VERSION_NOT_SUPP);
		return;
	}
	if (strcmp(val, "HTTP/1.1") != 0) {
		// We treat HTTP/1.0 connections as non-persistent.
		// No effort is made to handle "persistent" HTTP/1.0
		// since that was not standard.  (Everyone is at 1.1 now
		// anyways.)
		sc->close = true;
	}

	// If the connection was 1.0, or a connection: close was requested,
	// then mark this close on our end.
	if ((val = nni_http_req_get_header(req, "Connection")) != NULL) {
		// HTTP 1.1 says these have to be case insensitive (7230)
		if (nni_strcasestr(val, "close") != NULL) {
			// In theory this could falsely match some other weird
			// connection header that included the word close not
			// as part of a whole token.  No such legal definitions
			// exist, and so anyone who does that gets what they
			// deserve. (Fairly harmless actually, since it only
			// prevents persistent connections.)
			sc->close = true;
		}
	}

	val   = nni_http_req_get_uri(req);
	urisz = strlen(val) + 1;
	if ((uri = nni_alloc(urisz)) == NULL) {
		http_sconn_close(sc); // out of memory
		return;
	}
	strncpy(uri, val, urisz);
	path = http_uri_canonify(uri);

	nni_mtx_lock(&s->mtx);
	NNI_LIST_FOREACH (&s->handlers, h) {
		size_t len;
		if (h->h_host != NULL) {
			val = nni_http_req_get_header(req, "Host");
			if (val == NULL) {
				// We insist on a matching Host: line for
				// virtual hosting.  This leaves HTTP/1.0
				// out in the cold basically.
				continue;
			}

			// A few ways hosts can match.  They might have
			// a port attached -- we ignore that.  (We don't
			// run multiple ports, so if you got here, presumably
			// the port at least is correct!)  It might also have
			// a lone trailing dot, so that is ok too.

			// Ignore the trailing dot if the handler supplied it.
			len = strlen(h->h_host);
			if ((len > 0) && (h->h_host[len - 1] == '.')) {
				len--;
			}
			if ((nni_strncasecmp(val, h->h_host, len) != 0)) {
				continue;
			}
			if ((val[len] != '\0') && (val[len] != ':') &&
			    ((val[len] != '.') || (val[len + 1] != '\0'))) {
				continue;
			}
		}

		NNI_ASSERT(h->h_method != NULL);

		len = strlen(h->h_path);
		if (strncmp(path, h->h_path, len) != 0) {
			continue;
		}
		switch (path[len]) {
		case '\0':
			break;
		case '/':
			if ((path[len + 1] != '\0') && (!h->h_is_dir)) {
				// trailing component and not a directory.
				// Note that this should force a failure.
				continue;
			}
			break;
		default:
			continue; // some other substring, not matched.
		}

		// So, what about the method?
		val = nni_http_req_get_method(req);
		if (strcmp(val, h->h_method) == 0) {
			break;
		}
		// HEAD is remapped to GET.
		if ((strcmp(val, "HEAD") == 0) &&
		    (strcmp(h->h_method, "GET") == 0)) {
			break;
		}
		badmeth = 1;
	}

	nni_free(uri, urisz);
	if (h == NULL) {
		nni_mtx_unlock(&s->mtx);
		if (badmeth) {
			http_sconn_error(
			    sc, NNI_HTTP_STATUS_METHOD_NOT_ALLOWED);
		} else {
			http_sconn_error(sc, NNI_HTTP_STATUS_NOT_FOUND);
		}
		return;
	}

	nni_aio_set_input(sc->cbaio, 0, sc->http);
	nni_aio_set_input(sc->cbaio, 1, sc->req);
	nni_aio_set_input(sc->cbaio, 2, h->h_arg);

	// Technically, probably callback should initialize this with
	// start, but we do it instead.

	if (nni_aio_start(sc->cbaio, NULL, NULL) == 0) {
		nni_aio_set_data(sc->cbaio, 1, h);
		h->h_refcnt++;
		h->h_cb(sc->cbaio);
	}
	nni_mtx_unlock(&s->mtx);
}

static void
http_sconn_cbdone(void *arg)
{
	http_sconn *     sc  = arg;
	nni_aio *        aio = sc->cbaio;
	nni_http_res *   res;
	http_handler *   h;
	nni_http_server *s = sc->server;
	bool             upgrader;

	if (nni_aio_result(aio) != 0) {
		// Hard close, no further feedback.
		http_sconn_close(sc);
		return;
	}

	h   = nni_aio_get_data(aio, 1);
	res = nni_aio_get_output(aio, 0);

	nni_mtx_lock(&s->mtx);
	upgrader = h->h_is_upgrader;
	h->h_refcnt--;
	if (h->h_refcnt == 0) {
		http_handler_fini(h);
	}
	nni_mtx_unlock(&s->mtx);

	// If its an upgrader, and they didn't give us back a response, it
	// means that they took over, and we should just discard this session,
	// without closing the underlying channel.
	if (upgrader && (res == NULL)) {
		sc->http = NULL; // the underlying HTTP is not closed
		sc->req  = NULL;
		sc->res  = NULL;
		http_sconn_close(sc); // discard server session though
		return;
	}
	if (res != NULL) {

		const char *val;
		val = nni_http_res_get_header(res, "Connection");
		if ((val != NULL) && (strstr(val, "close") != NULL)) {
			sc->close = true;
		}
		if (sc->close) {
			nni_http_res_set_header(res, "Connection", "close");
		}
		sc->res = res;
		nni_http_write_res(sc->http, res, sc->txaio);
	} else if (sc->close) {
		http_sconn_close(sc);
	} else {
		// Presumably client already sent a response.
		// Wait for another request.
		nni_http_req_reset(sc->req);
		nni_http_read_req(sc->http, sc->req, sc->rxaio);
	}
}

static int
http_sconn_init(http_sconn **scp, nni_http_server *s, nni_plat_tcp_pipe *tcp)
{
	http_sconn *sc;
	int         rv;

	if ((sc = NNI_ALLOC_STRUCT(sc)) == NULL) {
		nni_plat_tcp_pipe_fini(tcp);
		return (NNG_ENOMEM);
	}

	if (((rv = nni_http_req_init(&sc->req)) != 0) ||
	    ((rv = nni_aio_init(&sc->rxaio, http_sconn_rxdone, sc)) != 0) ||
	    ((rv = nni_aio_init(&sc->txaio, http_sconn_txdone, sc)) != 0) ||
	    ((rv = nni_aio_init(&sc->txdataio, http_sconn_txdatdone, sc)) !=
	        0) ||
	    ((rv = nni_aio_init(&sc->cbaio, http_sconn_cbdone, sc)) != 0)) {
		// Can't even accept the incoming request.  Hard close.
		http_sconn_close(sc);
		return (rv);
	}

	if (s->tls != NULL) {
		rv = nni_http_init_tls(&sc->http, s->tls, tcp);
	} else {
		rv = nni_http_init_tcp(&sc->http, tcp);
	}
	if (rv != 0) {
		http_sconn_close(sc);
		return (rv);
	}
	*scp = sc;
	return (0);
}

static void
http_server_acccb(void *arg)
{
	nni_http_server *  s   = arg;
	nni_aio *          aio = s->accaio;
	nni_plat_tcp_pipe *tcp;
	http_sconn *       sc;
	int                rv;

	nni_mtx_lock(&s->mtx);
	if ((rv = nni_aio_result(aio)) != 0) {
		if (!s->closed) {
			// try again?
			nni_plat_tcp_ep_accept(s->tep, s->accaio);
		}
		nni_mtx_unlock(&s->mtx);
		return;
	}
	tcp = nni_aio_get_pipe(aio);
	if (s->closed) {
		// If we're closing, then reject this one.
		nni_plat_tcp_pipe_fini(tcp);
		nni_mtx_unlock(&s->mtx);
		return;
	}
	if (http_sconn_init(&sc, s, tcp) != 0) {
		// The TCP structure is already cleaned up.
		// Start another accept attempt.
		nni_plat_tcp_ep_accept(s->tep, s->accaio);
		nni_mtx_unlock(&s->mtx);
		return;
	}
	sc->server = s;
	nni_list_append(&s->conns, sc);

	nni_http_read_req(sc->http, sc->req, sc->rxaio);
	nni_plat_tcp_ep_accept(s->tep, s->accaio);
	nni_mtx_unlock(&s->mtx);
}

static void
http_handler_fini(http_handler *h)
{
	nni_strfree(h->h_path);
	nni_strfree(h->h_host);
	nni_strfree(h->h_method);
	if (h->h_free != NULL) {
		h->h_free(h->h_arg);
	}
	NNI_FREE_STRUCT(h);
}

static void
http_server_fini(nni_http_server *s)
{
	http_handler *h;

	nni_aio_stop(s->accaio);

	nni_mtx_lock(&s->mtx);
	while (!nni_list_empty(&s->conns)) {
		nni_cv_wait(&s->cv);
	}
	if (s->tep != NULL) {
		nni_plat_tcp_ep_fini(s->tep);
	}
	while ((h = nni_list_first(&s->handlers)) != NULL) {
		nni_list_remove(&s->handlers, h);
		http_handler_fini(h);
	}
	nni_mtx_unlock(&s->mtx);
	if (s->url != NULL) {
		nni_url_free(s->url);
	}
#ifdef NNG_SUPP_TLS
	if (s->tls != NULL) {
		nni_tls_config_fini(s->tls);
	}
#endif
	nni_aio_fini(s->accaio);
	nni_cv_fini(&s->cv);
	nni_mtx_fini(&s->mtx);
	NNI_FREE_STRUCT(s);
}

void
nni_http_server_fini(nni_http_server *s)
{
	nni_mtx_lock(&http_servers_lk);
	s->refcnt--;
	if (s->refcnt == 0) {
		nni_list_remove(&http_servers, s);
		http_server_fini(s);
	}
	nni_mtx_unlock(&http_servers_lk);
}

static int
http_server_init(nni_http_server **serverp, nni_url *url)
{
	nni_http_server *s;
	int              rv;
	const char *     host;
	const char *     port;
	nni_aio *        aio;

	host = url->u_hostname;
	if (strlen(host) == 0) {
		host = NULL;
	}

	port = url->u_port;
	if ((strcmp(url->u_scheme, "http") != 0) &&
#ifdef NNG_SUPP_TLS
	    (strcmp(url->u_scheme, "https") != 0) &&
	    (strcmp(url->u_scheme, "wss") != 0) &&
#endif
	    (strcmp(url->u_scheme, "ws") != 0)) {
		nni_url_free(url);
		return (NNG_EADDRINVAL);
	}
	if ((s = NNI_ALLOC_STRUCT(s)) == NULL) {
		nni_url_free(url);
		return (NNG_ENOMEM);
	}
	s->url = url;
	nni_mtx_init(&s->mtx);
	nni_cv_init(&s->cv, &s->mtx);
	NNI_LIST_INIT(&s->handlers, http_handler, node);
	NNI_LIST_INIT(&s->conns, http_sconn, node);
	if ((rv = nni_aio_init(&s->accaio, http_server_acccb, s)) != 0) {
		http_server_fini(s);
		return (rv);
	}
#ifdef NNG_SUPP_TLS
	if ((strcmp(url->u_scheme, "https") == 0) ||
	    (strcmp(url->u_scheme, "wss") == 0)) {
		rv = nni_tls_config_init(&s->tls, NNG_TLS_MODE_SERVER);
		if (rv != 0) {
			http_server_fini(s);
			return (rv);
		}
	}
#endif

	// Do the DNS lookup *now*.  This means that this is synchronous,
	// but it should be fast, since it should either resolve as a number,
	// or resolve locally, without having to hit up DNS.
	if ((rv = nni_aio_init(&aio, NULL, NULL)) != 0) {
		http_server_fini(s);
		return (rv);
	}
	aio->a_addr = &s->addr;
	host        = (strlen(url->u_hostname) != 0) ? url->u_hostname : NULL;
	port        = (strlen(url->u_port) != 0) ? url->u_port : NULL;
	nni_plat_tcp_resolv(host, port, NNG_AF_UNSPEC, true, aio);
	nni_aio_wait(aio);
	rv = nni_aio_result(aio);
	nni_aio_fini(aio);
	if (rv != 0) {
		http_server_fini(s);
		return (rv);
	}
	s->refcnt = 1;
	*serverp  = s;
	return (0);
}

int
nni_http_server_init(nni_http_server **serverp, const char *urlstr)
{
	int              rv;
	nni_http_server *s;
	nni_url *        url;

	if ((rv = nni_url_parse(&url, urlstr)) != 0) {
		return (rv);
	}

	nni_initialize(&http_server_initializer);

	nni_mtx_lock(&http_servers_lk);
	NNI_LIST_FOREACH (&http_servers, s) {
		if ((strcmp(url->u_port, s->url->u_port) == 0) &&
		    (strcmp(url->u_hostname, s->url->u_hostname) == 0)) {
			nni_url_free(url);
			*serverp = s;
			s->refcnt++;
			nni_mtx_unlock(&http_servers_lk);
			return (0);
		}
	}

	// We didn't find a server, try to make a new one.
	if ((rv = http_server_init(&s, url)) == 0) {
		nni_list_append(&http_servers, s);
		*serverp = s;
	} else {
		nni_url_free(url);
	}

	nni_mtx_unlock(&http_servers_lk);
	return (rv);
}

static int
http_server_start(nni_http_server *s)
{
	int rv;
	rv = nni_plat_tcp_ep_init(&s->tep, &s->addr, NULL, NNI_EP_MODE_LISTEN);
	if (rv != 0) {
		return (rv);
	}
	if ((rv = nni_plat_tcp_ep_listen(s->tep)) != 0) {
		nni_plat_tcp_ep_fini(s->tep);
		s->tep = NULL;
		return (rv);
	}
	nni_plat_tcp_ep_accept(s->tep, s->accaio);
	return (0);
}

int
nni_http_server_start(nni_http_server *s)
{
	int rv = 0;

	nni_mtx_lock(&s->mtx);
	if (s->starts == 0) {
		rv = http_server_start(s);
	}
	if (rv == 0) {
		s->starts++;
	}
	nni_mtx_unlock(&s->mtx);
	return (rv);
}

static void
http_server_stop(nni_http_server *s)
{
	http_sconn *sc;

	if (s->closed) {
		return;
	}

	s->closed = true;
	// Close the TCP endpoint that is listening.
	if (s->tep) {
		nni_plat_tcp_ep_close(s->tep);
	}

	// Stopping the server is a hard stop -- it aborts any work being
	// done by clients.  (No graceful shutdown).
	NNI_LIST_FOREACH (&s->conns, sc) {
		http_sconn_close_locked(sc);
	}
	nni_cv_wake(&s->cv);
}

void
nni_http_server_stop(nni_http_server *s)
{
	nni_mtx_lock(&s->mtx);
	s->starts--;
	if (s->starts == 0) {
		http_server_stop(s);
	}
	nni_mtx_unlock(&s->mtx);
}

int
http_server_add_handler(void **hp, nni_http_server *s, nni_http_handler *hh,
    void *arg, void (*freeit)(void *))
{
	http_handler *h, *h2;
	size_t        l1, l2;

	// Must have a legal method (and not one that is HEAD), path,
	// and handler.  (The reason HEAD is verboten is that we supply
	// it automatically as part of GET support.)
	if ((hh->h_method == NULL) || (hh->h_path == NULL) ||
	    (hh->h_cb == NULL) || (strcmp(hh->h_method, "HEAD") == 0)) {
		return (NNG_EINVAL);
	}
	if ((h = NNI_ALLOC_STRUCT(h)) == NULL) {
		return (NNG_ENOMEM);
	}
	h->h_arg         = arg;
	h->h_cb          = hh->h_cb;
	h->h_is_dir      = hh->h_is_dir;
	h->h_is_upgrader = hh->h_is_upgrader;
	h->h_free        = freeit;

	// When registering the handler, only register *host*, not port.
	if (hh->h_host != NULL) {
		if ((h->h_host = nni_strdup(hh->h_host)) == NULL) {
			http_handler_fini(h);
			return (NNG_ENOMEM);
		}
	}

	if (((h->h_method = nni_strdup(hh->h_method)) == NULL) ||
	    ((h->h_path = nni_strdup(hh->h_path)) == NULL)) {
		http_handler_fini(h);
		return (NNG_ENOMEM);
	}

	l1 = strlen(h->h_path);
	// Chop off trailing "/"
	while (l1 > 0) {
		if (h->h_path[l1 - 1] != '/') {
			break;
		}
		l1--;
		h->h_path[l1] = '\0';
	}

	nni_mtx_lock(&s->mtx);
	// General rule for finding a conflict is that if either string
	// is a strict substring of the other, then we have a
	// collision.  (But only if the methods match, and the host
	// matches.)  Note that a wild card host matches both.
	NNI_LIST_FOREACH (&s->handlers, h2) {
		if ((h2->h_host != NULL) && (h->h_host != NULL) &&
		    (nni_strcasecmp(h2->h_host, h->h_host) != 0)) {
			// Hosts don't match, so we are safe.
			continue;
		}
		if (strcmp(h2->h_method, h->h_method) != 0) {
			// Different methods, so again we are fine.
			continue;
		}
		l2 = strlen(h2->h_path);
		if (l1 < l2) {
			l2 = l1;
		}
		if (strncmp(h2->h_path, h->h_path, l2) == 0) {
			// Path collision.  NNG_EADDRINUSE.
			nni_mtx_unlock(&s->mtx);
			http_handler_fini(h);
			return (NNG_EADDRINUSE);
		}
	}
	h->h_refcnt = 1;
	nni_list_append(&s->handlers, h);
	nni_mtx_unlock(&s->mtx);
	if (hp != NULL) {
		*hp = h;
	}
	return (0);
}

int
nni_http_server_add_handler(
    void **hp, nni_http_server *s, nni_http_handler *hh, void *arg)
{
	return (http_server_add_handler(hp, s, hh, arg, NULL));
}

void
nni_http_server_del_handler(nni_http_server *s, void *harg)
{
	http_handler *h = harg;

	nni_mtx_lock(&s->mtx);
	nni_list_node_remove(&h->node);
	h->h_refcnt--;
	if (h->h_refcnt == 0) {
		http_handler_fini(h);
	}
	nni_mtx_unlock(&s->mtx);
}

// Very limited MIME type map.  Used only if the handler does not
// supply it's own.
static struct content_map {
	const char *ext;
	const char *typ;
} content_map[] = {
	// clang-format off
	{ ".ai", "application/postscript" },
	{ ".aif", "audio/aiff" },
	{ ".aiff", "audio/aiff" },
	{ ".avi", "video/avi" },
	{ ".au", "audio/basic" },
	{ ".bin", "application/octet-stream" },
	{ ".bmp", "image/bmp" },
	{ ".css", "text/css" },
	{ ".eps", "application/postscript" },
	{ ".gif", "image/gif" },
	{ ".htm", "text/html" },
	{ ".html", "text/html" },
	{ ".ico", "image/x-icon" },
	{ ".jpeg", "image/jpeg" },
	{ ".jpg", "image/jpeg" },
	{ ".js", "application/javascript" },
	{ ".md", "text/markdown" },
	{ ".mp2", "video/mpeg" },
	{ ".mp3", "audio/mpeg3" },
	{ ".mpeg", "video/mpeg" },
	{ ".mpg", "video/mpeg" },
	{ ".pdf", "application/pdf" },
	{ ".png", "image/png" },
	{ ".ps", "application/postscript" },
	{ ".rtf", "text/rtf" },
	{ ".text", "text/plain" },
	{ ".tif", "image/tiff" },
	{ ".tiff", "image/tiff" },
	{ ".txt", "text/plain" },
	{ ".wav", "audio/wav"},
	{ "README", "text/plain" },
	{ NULL, NULL },
	// clang-format on
};

const char *
http_lookup_type(const char *path)
{
	size_t l1 = strlen(path);
	for (int i = 0; content_map[i].ext != NULL; i++) {
		size_t l2 = strlen(content_map[i].ext);
		if (l2 > l1) {
			continue;
		}
		if (nni_strcasecmp(&path[l1 - l2], content_map[i].ext) == 0) {
			return (content_map[i].typ);
		}
	}
	return (NULL);
}

typedef struct http_file {
	char *typ;
	char *pth;
} http_file;

static void
http_handle_file(nni_aio *aio)
{
	http_file *   f   = nni_aio_get_input(aio, 2);
	nni_http_res *res = NULL;
	void *        data;
	size_t        size;
	int           rv;

	if ((rv = nni_plat_file_get(f->pth, &data, &size)) != 0) {
		uint16_t status;
		switch (rv) {
		case NNG_ENOMEM:
			status = NNI_HTTP_STATUS_INTERNAL_SERVER_ERROR;
			break;
		case NNG_ENOENT:
			status = NNI_HTTP_STATUS_NOT_FOUND;
			break;
		case NNG_EPERM:
			status = NNI_HTTP_STATUS_FORBIDDEN;
			break;
		default:
			status = NNI_HTTP_STATUS_INTERNAL_SERVER_ERROR;
			break;
		}
		if ((rv = nni_http_res_init_error(&res, status)) != 0) {
			nni_aio_finish_error(aio, rv);
			return;
		}
	} else {
		if (((rv = nni_http_res_init(&res)) != 0) ||
		    ((rv = nni_http_res_set_status(
		          res, NNI_HTTP_STATUS_OK, "OK")) != 0) ||
		    ((rv = nni_http_res_set_header(
		          res, "Content-Type", f->typ)) != 0) ||
		    ((rv = nni_http_res_set_data(res, data, size)) != 0)) {
			nni_free(data, size);
			nni_aio_finish_error(aio, rv);
			return;
		}
	}
	nni_aio_set_output(aio, 0, res);
	nni_aio_finish(aio, 0, 0);
}

static void
http_free_file(void *arg)
{
	http_file *f = arg;
	nni_strfree(f->pth);
	nni_strfree(f->typ);
	NNI_FREE_STRUCT(f);
}

int
nni_http_server_add_file(nni_http_server *s, const char *host,
    const char *ctype, const char *uri, const char *path)
{
	nni_http_handler h;
	http_file *      f;
	int              rv;

	if ((f = NNI_ALLOC_STRUCT(f)) == NULL) {
		return (NNG_ENOMEM);
	}
	if (ctype == NULL) {
		ctype = http_lookup_type(path);
	}

	if (((f->pth = nni_strdup(path)) == NULL) ||
	    ((ctype != NULL) && ((f->typ = nni_strdup(ctype)) == NULL))) {
		http_free_file(f);
		return (NNG_ENOMEM);
	}
	h.h_method      = "GET";
	h.h_path        = uri;
	h.h_host        = host;
	h.h_cb          = http_handle_file;
	h.h_is_dir      = false;
	h.h_is_upgrader = false;

	if ((rv = http_server_add_handler(NULL, s, &h, f, http_free_file)) !=
	    0) {
		http_free_file(f);
		return (rv);
	}
	return (0);
}

typedef struct http_static {
	char * typ;
	void * data;
	size_t size;
} http_static;

static void
http_handle_static(nni_aio *aio)
{
	http_static * s = nni_aio_get_input(aio, 2);
	nni_http_res *r = NULL;
	int           rv;

	if (((rv = nni_http_res_init(&r)) != 0) ||
	    ((rv = nni_http_res_set_header(r, "Content-Type", s->typ)) != 0) ||
	    ((rv = nni_http_res_set_status(r, NNI_HTTP_STATUS_OK, "OK")) !=
	        0) ||
	    ((rv = nni_http_res_set_data(r, s->data, s->size)) != 0)) {
		nni_aio_finish_error(aio, rv);
		return;
	}

	nni_aio_set_output(aio, 0, r);
	nni_aio_finish(aio, 0, 0);
}

static void
http_free_static(void *arg)
{
	http_static *s = arg;
	nni_strfree(s->typ);
	nni_free(s->data, s->size);
	NNI_FREE_STRUCT(s);
}

int
nni_http_server_add_static(nni_http_server *s, const char *host,
    const char *ctype, const char *uri, const void *data, size_t size)
{
	nni_http_handler h;
	http_static *    f;
	int              rv;

	if ((f = NNI_ALLOC_STRUCT(f)) == NULL) {
		return (NNG_ENOMEM);
	}
	if (ctype == NULL) {
		ctype = "application/octet-stream";
	}
	if (((f->data = nni_alloc(size)) == NULL) ||
	    ((f->typ = nni_strdup(ctype)) == NULL)) {
		http_free_static(f);
		return (NNG_ENOMEM);
	}

	f->size = size;
	memcpy(f->data, data, size);

	h.h_method      = "GET";
	h.h_path        = uri;
	h.h_host        = host;
	h.h_cb          = http_handle_static;
	h.h_is_dir      = false;
	h.h_is_upgrader = false;

	if ((rv = http_server_add_handler(NULL, s, &h, f, http_free_static)) !=
	    0) {
		http_free_static(f);
		return (rv);
	}
	return (0);
}

int
nni_http_server_set_tls(nni_http_server *s, nng_tls_config *tcfg)
{
#ifdef NNG_SUPP_TLS
	nng_tls_config *old;
	nni_mtx_lock(&s->mtx);
	if (s->starts) {
		nni_mtx_unlock(&s->mtx);
		return (NNG_EBUSY);
	}
	old    = s->tls;
	s->tls = tcfg;
	if (tcfg) {
		nni_tls_config_hold(tcfg);
	}
	nni_mtx_unlock(&s->mtx);
	if (old) {
		nni_tls_config_fini(old);
	}
	return (0);
#else
	return (NNG_ENOTSUP);
#endif
}

int
nni_http_server_get_tls(nni_http_server *s, nng_tls_config **tp)
{
#ifdef NNG_SUPP_TLS
	nni_mtx_lock(&s->mtx);
	if (s->tls == NULL) {
		nni_mtx_unlock(&s->mtx);
		return (NNG_EINVAL);
	}
	*tp = s->tls;
	nni_mtx_unlock(&s->mtx);
	return (0);
#else
	return (NNG_ENOTSUP);
#endif
}

static int
http_server_sys_init(void)
{
	NNI_LIST_INIT(&http_servers, nni_http_server, node);
	nni_mtx_init(&http_servers_lk);
	return (0);
}

static void
http_server_sys_fini(void)
{
	nni_mtx_fini(&http_servers_lk);
}