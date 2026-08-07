/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "ssi_transport.h"

#include "image.h"
#include "stringutils.h"

/* The local SlideCenter benchmark saturated near twelve concurrent requests. */
#define SSI_CONNECTION_POOL_SIZE 12

typedef struct ssi_connection_slot_t {
	tls_connection_t* connection;
	char hostname[256];
	i32 port;
	bool use_tls;
	bool in_use;
} ssi_connection_slot_t;

static platform_mutex_t ssi_connection_pool_mutex = PLATFORM_MUTEX_INITIALIZER;
static ssi_connection_slot_t ssi_connection_pool[SSI_CONNECTION_POOL_SIZE];
static volatile i32 ssi_pool_open_count;
static volatile i32 ssi_pool_reuse_count;
static volatile i32 ssi_pool_wait_count;
static volatile i32 ssi_control_reuse_count;
static volatile i32 ssi_control_fallback_handshake_count;

static bool ssi_connection_matches(const ssi_connection_slot_t* slot,
		const ssi_endpoint_t* endpoint) {
	return slot->connection && strcmp(slot->hostname, endpoint->hostname) == 0 &&
	       slot->port == endpoint->port && slot->use_tls == endpoint->use_tls;
}

static ssi_connection_slot_t* ssi_try_acquire_pool_slot(
		const ssi_endpoint_t* endpoint) {
	ssi_connection_slot_t* selected = NULL;
	platform_mutex_lock(&ssi_connection_pool_mutex);
	for (i32 i = 0; i < SSI_CONNECTION_POOL_SIZE; ++i) {
		ssi_connection_slot_t* candidate = ssi_connection_pool + i;
		if (!candidate->in_use && ssi_connection_matches(candidate, endpoint)) {
			selected = candidate;
			break;
		}
	}
	if (!selected) {
		for (i32 i = 0; i < SSI_CONNECTION_POOL_SIZE; ++i) {
			if (!ssi_connection_pool[i].in_use) {
				selected = ssi_connection_pool + i;
				break;
			}
		}
	}
	if (selected) selected->in_use = true;
	platform_mutex_unlock(&ssi_connection_pool_mutex);
	return selected;
}

static void ssi_release_pool_slot(ssi_connection_slot_t* slot, bool reusable) {
	if (!slot) return;
	if (!reusable && slot->connection) {
		remote_connection_close_and_free(slot->connection);
		slot->connection = NULL;
		slot->hostname[0] = '\0';
	}
	platform_mutex_lock(&ssi_connection_pool_mutex);
	slot->in_use = false;
	platform_mutex_unlock(&ssi_connection_pool_mutex);
}

static bool ssi_prepare_pool_slot(ssi_connection_slot_t* slot,
		const ssi_endpoint_t* endpoint, bool is_control) {
	if (ssi_connection_matches(slot, endpoint)) {
		atomic_increment(&ssi_pool_reuse_count);
		if (is_control) atomic_increment(&ssi_control_reuse_count);
		return true;
	}
	if (slot->connection) {
		remote_connection_close_and_free(slot->connection);
		slot->connection = NULL;
	}
	slot->connection = remote_connection_open_http(
		endpoint->hostname, endpoint->port, endpoint->use_tls);
	if (!slot->connection) {
		ssi_release_pool_slot(slot, false);
		return false;
	}
	copy_cstring(slot->hostname, endpoint->hostname, sizeof(slot->hostname));
	slot->port = endpoint->port;
	slot->use_tls = endpoint->use_tls;
	atomic_increment(&ssi_pool_open_count);
	return true;
}

http_response_t* ssi_transport_request(const ssi_endpoint_t* endpoint,
		const image_t* image, ssi_transport_pool_policy_t pool_policy,
		remote_http_method_t method, const char* path, const char* accept,
		const char* content_type, const void* body, size_t body_size,
		const remote_request_options_t* options) {
	if (!endpoint || !path) return NULL;
	bool is_control = pool_policy == SSI_TRANSPORT_FALLBACK_IF_BUSY;
	bool reported_wait = false;
	for (i32 attempt = 0; attempt < (is_control ? 2 : 1); ++attempt) {
		ssi_connection_slot_t* slot = NULL;
		while (!(slot = ssi_try_acquire_pool_slot(endpoint))) {
			if (is_control) break;
			if (image && image->is_deleted) return NULL;
			if (!reported_wait) {
				atomic_increment(&ssi_pool_wait_count);
				reported_wait = true;
			}
			platform_sleep(1);
		}

		tls_connection_t* connection = NULL;
		bool temporary = slot == NULL;
		if (temporary) {
			connection = remote_connection_open_http(
				endpoint->hostname, endpoint->port, endpoint->use_tls);
			if (!connection) return NULL;
			atomic_increment(&ssi_control_fallback_handshake_count);
		} else {
			if (!ssi_prepare_pool_slot(slot, endpoint, is_control)) {
				if (is_control && attempt == 0) continue;
				return NULL;
			}
			connection = slot->connection;
		}

		http_response_t* response = remote_connection_request_http(
			connection, endpoint->host_header, method, path, accept, content_type,
			body, body_size, NULL, temporary, options);
		if (temporary) {
			remote_connection_close_and_free(connection);
		} else {
			ssi_release_pool_slot(slot, response && response->connection_reusable);
		}
		/* A server may have timed out an apparently idle pooled socket. */
		if (is_control && attempt == 0 &&
		    (!response || response->status_code <= 0)) {
			if (response) http_response_destroy(response);
			continue;
		}
		return response;
	}
	return NULL;
}

void ssi_transport_get_stats(ssi_transport_stats_t* stats) {
	if (!stats) return;
	stats->pool_opens = ssi_pool_open_count;
	stats->pool_reuses = ssi_pool_reuse_count;
	stats->pool_waits = ssi_pool_wait_count;
	stats->control_reuses = ssi_control_reuse_count;
	stats->control_fallback_handshakes = ssi_control_fallback_handshake_count;
}
