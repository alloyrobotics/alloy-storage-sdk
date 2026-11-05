/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 */

#include "alloy_http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(alloy_storage, CONFIG_ALLOY_STORAGE_LOG_LEVEL);

#define ALLOY_USER_AGENT "alloy-storage-zephyr/" ALLOY_STORAGE_VERSION

int alloy_http_init(struct alloy_http *h, const char *url, bool insecure, sec_tag_t sec_tag,
		    int timeout_ms)
{
	const char *p;
	const char *end;
	size_t n;

	memset(h, 0, sizeof(*h));
	h->sock = -1;
	h->insecure = insecure;
	h->sec_tag = sec_tag;
	h->timeout_ms = timeout_ms;

	if (!url) {
		return -EINVAL;
	}
	if (strncmp(url, "https://", 8) == 0) {
		h->tls = true;
		p = url + 8;
		strcpy(h->port, "443");
	} else if (strncmp(url, "http://", 7) == 0) {
		h->tls = false;
		p = url + 7;
		strcpy(h->port, "80");
	} else {
		return -EINVAL;
	}

	end = p;
	while (*end && *end != ':' && *end != '/') {
		end++;
	}
	n = (size_t)(end - p);
	if (n == 0 || n >= sizeof(h->host)) {
		return -EINVAL;
	}
	memcpy(h->host, p, n);
	h->host[n] = '\0';

	if (*end == ':') {
		const char *q = ++end;

		while (*end >= '0' && *end <= '9') {
			end++;
		}
		n = (size_t)(end - q);
		if (n == 0 || n >= sizeof(h->port) || (*end && *end != '/')) {
			return -EINVAL;
		}
		memcpy(h->port, q, n);
		h->port[n] = '\0';
	}

	n = strlen(end);
	while (n > 0 && end[n - 1] == '/') {
		n--;
	}
	if (n >= sizeof(h->base)) {
		return -EINVAL;
	}
	memcpy(h->base, end, n);
	h->base[n] = '\0';
	return 0;
}

void alloy_http_close(struct alloy_http *h)
{
	if (h->sock >= 0) {
		zsock_close(h->sock);
		h->sock = -1;
	}
}

static int set_timeouts(int sock, int timeout_ms)
{
	struct zsock_timeval tv = {
		.tv_sec = timeout_ms / 1000,
		.tv_usec = (timeout_ms % 1000) * 1000,
	};

	if (zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
	    zsock_setsockopt(sock, ZSOCK_SOL_SOCKET, ZSOCK_SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
		return -errno;
	}
	return 0;
}

static int setup_tls(struct alloy_http *h, int sock)
{
	sec_tag_t tags[] = {h->sec_tag};
	int verify = h->insecure ? ZSOCK_TLS_PEER_VERIFY_NONE : ZSOCK_TLS_PEER_VERIFY_REQUIRED;

	if (zsock_setsockopt(sock, ZSOCK_SOL_TLS, ZSOCK_TLS_SEC_TAG_LIST, tags, sizeof(tags)) < 0 ||
	    zsock_setsockopt(sock, ZSOCK_SOL_TLS, ZSOCK_TLS_HOSTNAME, h->host,
			     strlen(h->host) + 1) < 0 ||
	    zsock_setsockopt(sock, ZSOCK_SOL_TLS, ZSOCK_TLS_PEER_VERIFY, &verify,
			     sizeof(verify)) < 0) {
		return -errno;
	}
	return 0;
}

static int open_connection(struct alloy_http *h)
{
	struct zsock_addrinfo hints = {
		.ai_family = NET_AF_UNSPEC,
		.ai_socktype = NET_SOCK_STREAM,
	};
	struct zsock_addrinfo *res = NULL;
	int rc;

	rc = zsock_getaddrinfo(h->host, h->port, &hints, &res);
	if (rc != 0) {
		LOG_WRN("DNS lookup for %s failed (%d)", h->host, rc);
		return -EHOSTUNREACH;
	}

	rc = -EHOSTUNREACH;
	for (struct zsock_addrinfo *ai = res; ai; ai = ai->ai_next) {
		int proto = h->tls ? NET_IPPROTO_TLS_1_2 : NET_IPPROTO_TCP;
		int sock = zsock_socket(ai->ai_family, NET_SOCK_STREAM, proto);
		int64_t t0 = k_uptime_get();

		if (sock < 0) {
			rc = -errno;
			continue;
		}
		rc = set_timeouts(sock, h->timeout_ms);
		if (rc == 0 && h->tls) {
			rc = setup_tls(h, sock);
		}
		if (rc == 0 && zsock_connect(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
			rc = -errno;
		}
		if (rc == 0) {
			LOG_DBG("connected to %s in %lld ms", h->host, k_uptime_get() - t0);
			h->sock = sock;
			break;
		}
		LOG_WRN("connect to %s failed (%d)", h->host, rc);
		zsock_close(sock);
	}
	zsock_freeaddrinfo(res);
	return rc;
}

static int send_all(int sock, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len > 0) {
		ssize_t n = zsock_send(sock, p, len, 0);

		if (n < 0) {
			return -errno;
		}
		if (n == 0) {
			return -EIO;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

struct rx {
	int sock;
	uint8_t buf[256];
	size_t pos;
	size_t len;
	size_t total; /* bytes received for this response */
};

static int rx_byte(struct rx *r, uint8_t *c)
{
	if (r->pos == r->len) {
		ssize_t n = zsock_recv(r->sock, r->buf, sizeof(r->buf), 0);

		if (n < 0) {
			return -errno;
		}
		if (n == 0) {
			return -ECONNRESET;
		}
		r->pos = 0;
		r->len = (size_t)n;
	}
	*c = r->buf[r->pos++];
	r->total++;
	return 0;
}

/* Reads one line without its CRLF into line (truncated to cap - 1). Returns its length. */
static int rx_line(struct rx *r, char *line, size_t cap)
{
	size_t n = 0;

	for (;;) {
		uint8_t c;
		int rc = rx_byte(r, &c);

		if (rc < 0) {
			return rc;
		}
		if (c == '\n') {
			break;
		}
		if (n + 1 < cap) {
			line[n++] = (char)c;
		}
	}
	if (n > 0 && line[n - 1] == '\r') {
		n--;
	}
	line[n] = '\0';
	return (int)n;
}

static int rx_skip(struct rx *r, size_t n)
{
	while (n-- > 0) {
		uint8_t c;
		int rc = rx_byte(r, &c);

		if (rc < 0) {
			return rc;
		}
	}
	return 0;
}

static int rx_chunked(struct rx *r)
{
	char line[32];

	for (;;) {
		int rc = rx_line(r, line, sizeof(line));
		unsigned long size;

		if (rc < 0) {
			return rc;
		}
		size = strtoul(line, NULL, 16);
		if (size == 0) {
			break;
		}
		rc = rx_skip(r, size + 2); /* data + CRLF */
		if (rc < 0) {
			return rc;
		}
	}
	/* Trailer lines until the empty line. */
	for (;;) {
		int rc = rx_line(r, line, sizeof(line));

		if (rc <= 0) {
			return rc;
		}
	}
}

static bool header_is(const char *line, const char *name, const char **value)
{
	size_t n = strlen(name);

	if (strncasecmp(line, name, n) != 0 || line[n] != ':') {
		return false;
	}
	*value = line + n + 1;
	while (**value == ' ' || **value == '\t') {
		(*value)++;
	}
	return true;
}

static bool contains_token(const char *value, const char *token)
{
	size_t n = strlen(token);

	for (const char *p = value; *p; p++) {
		if (strncasecmp(p, token, n) == 0) {
			return true;
		}
	}
	return false;
}

static int read_response(struct alloy_http *h, struct rx *r, bool head,
			 struct alloy_http_response *resp)
{
	char line[128];
	int status;
	long content_length;
	bool chunked;
	bool close_after;
	int rc;

	do {
		rc = rx_line(r, line, sizeof(line));
		if (rc < 0) {
			return rc;
		}
		if (strncmp(line, "HTTP/1.", 7) != 0 || strlen(line) < 12) {
			return -EBADMSG;
		}
		status = atoi(line + 9);
		if (status < 100 || status > 599) {
			return -EBADMSG;
		}

		content_length = -1;
		chunked = false;
		close_after = strncmp(line, "HTTP/1.0", 8) == 0;
		resp->date[0] = '\0';
		for (;;) {
			const char *v;

			rc = rx_line(r, line, sizeof(line));
			if (rc < 0) {
				return rc;
			}
			if (rc == 0) {
				break;
			}
			if (header_is(line, "Content-Length", &v)) {
				content_length = atol(v);
			} else if (header_is(line, "Transfer-Encoding", &v)) {
				chunked = contains_token(v, "chunked");
			} else if (header_is(line, "Connection", &v)) {
				if (contains_token(v, "close")) {
					close_after = true;
				} else if (contains_token(v, "keep-alive")) {
					close_after = false;
				}
			} else if (header_is(line, "Date", &v)) {
				size_t n = strlen(v);

				while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t')) {
					n--;
				}
				if (n < sizeof(resp->date)) {
					memcpy(resp->date, v, n);
					resp->date[n] = '\0';
				}
			}
		}
	} while (status < 200); /* skip interim 1xx responses */

	if (head || status == 204 || status == 304) {
		rc = 0;
	} else if (chunked) {
		rc = rx_chunked(r);
	} else if (content_length >= 0) {
		rc = rx_skip(r, (size_t)content_length);
	} else {
		/* Body delimited by connection close: read to EOF, then drop the connection. */
		uint8_t c;

		while ((rc = rx_byte(r, &c)) == 0) {
		}
		rc = rc == -ECONNRESET ? 0 : rc;
		close_after = true;
	}
	if (rc < 0) {
		return rc;
	}
	if (close_after) {
		alloy_http_close(h);
	}
	resp->status = status;
	return status;
}

static int request_once(struct alloy_http *h, const char *method, const char *route,
			const struct alloy_http_header *headers, size_t n_headers,
			const char *content_type, const uint8_t *body, size_t len,
			struct alloy_http_response *resp, size_t *received)
{
	char req[640];
	struct rx r = {.sock = h->sock};
	int n;
	int rc;

	n = snprintf(req, sizeof(req),
		     "%s %s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " ALLOY_USER_AGENT
		     "\r\nConnection: keep-alive\r\n",
		     method, h->base, route, h->host);
	for (size_t i = 0; i < n_headers && n > 0 && (size_t)n < sizeof(req); i++) {
		n += snprintf(req + n, sizeof(req) - (size_t)n, "%s: %s\r\n", headers[i].name,
			      headers[i].value);
	}
	if (n > 0 && (size_t)n < sizeof(req) && content_type) {
		n += snprintf(req + n, sizeof(req) - (size_t)n, "Content-Type: %s\r\n",
			      content_type);
	}
	if (n > 0 && (size_t)n < sizeof(req) && (body || strcmp(method, "POST") == 0)) {
		n += snprintf(req + n, sizeof(req) - (size_t)n, "Content-Length: %u\r\n",
			      (unsigned int)len);
	}
	if (n > 0 && (size_t)n < sizeof(req)) {
		n += snprintf(req + n, sizeof(req) - (size_t)n, "\r\n");
	}
	if (n <= 0 || (size_t)n >= sizeof(req)) {
		return -E2BIG;
	}

	rc = send_all(h->sock, req, (size_t)n);
	if (rc == 0 && body && len > 0) {
		rc = send_all(h->sock, body, len);
	}
	if (rc == 0) {
		rc = read_response(h, &r, strcmp(method, "HEAD") == 0, resp);
	}
	*received = r.total;
	return rc;
}

int alloy_http_request(struct alloy_http *h, const char *method, const char *route,
		       const struct alloy_http_header *headers, size_t n_headers,
		       const char *content_type, const uint8_t *body, size_t len,
		       struct alloy_http_response *resp)
{
	for (int attempt = 0; attempt < 2; attempt++) {
		bool reused = h->sock >= 0;
		size_t received = 0;
		int rc;

		if (!reused) {
			rc = open_connection(h);
			if (rc < 0) {
				return rc;
			}
		}
		rc = request_once(h, method, route, headers, n_headers, content_type, body, len,
				  resp, &received);
		if (rc >= 0) {
			return rc;
		}
		alloy_http_close(h);
		/* A kept-alive connection the server already closed fails before any response
		 * byte: reconnect and resend once. Anything else is the caller's retry.
		 */
		if (!reused || received > 0) {
			LOG_DBG("%s %s failed (%d)", method, route, rc);
			return rc;
		}
	}
	return -ECONNRESET;
}
