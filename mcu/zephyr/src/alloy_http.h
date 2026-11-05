/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 *
 * Internal: a small HTTP/1.1 client over Zephyr BSD sockets (TLS via the socket's mbedTLS
 * backend). One persistent keep-alive connection: a verified handshake costs about 1.5 s on a
 * single-core ESP32-C3, so reconnecting per chunk could not keep up with several channels.
 */

#ifndef ALLOY_STORAGE_HTTP_H_
#define ALLOY_STORAGE_HTTP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/net/tls_credentials.h>

struct alloy_http {
	char host[64];
	char base[64]; /* path prefix from the URL, without a trailing slash */
	char port[6];
	bool tls;
	bool insecure;
	int sock;
	sec_tag_t sec_tag;
	int timeout_ms;
};

struct alloy_http_header {
	const char *name;
	const char *value;
};

struct alloy_http_response {
	int status;
	char date[32]; /* Date header, "" when absent */
};

/* Parses http(s)://host[:port][/base]. Returns 0 or -EINVAL. */
int alloy_http_init(struct alloy_http *h, const char *url, bool insecure, sec_tag_t sec_tag,
		    int timeout_ms);

/* Sends one request on the persistent connection, reconnecting once if a reused connection
 * turns out to be dead. Returns the HTTP status, or a negative errno for transport errors (the
 * connection is closed then, so the next request starts a fresh handshake).
 */
int alloy_http_request(struct alloy_http *h, const char *method, const char *route,
		       const struct alloy_http_header *headers, size_t n_headers,
		       const char *content_type, const uint8_t *body, size_t len,
		       struct alloy_http_response *resp);

void alloy_http_close(struct alloy_http *h);

#endif /* ALLOY_STORAGE_HTTP_H_ */
