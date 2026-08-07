/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#pragma once

#include "remote.h"
#include "ssi.h"

typedef enum ssi_transport_pool_policy_t {
	SSI_TRANSPORT_WAIT_FOR_POOL = 0,
	SSI_TRANSPORT_FALLBACK_IF_BUSY,
} ssi_transport_pool_policy_t;

typedef struct ssi_transport_stats_t {
	i32 pool_opens;
	i32 pool_reuses;
	i32 pool_waits;
	i32 control_reuses;
	i32 control_fallback_handshakes;
} ssi_transport_stats_t;

#ifdef __cplusplus
extern "C" {
#endif

http_response_t* ssi_transport_request(const ssi_endpoint_t* endpoint,
		const image_t* image, ssi_transport_pool_policy_t pool_policy,
		remote_http_method_t method, const char* path, const char* accept,
		const char* content_type, const void* body, size_t body_size,
		const remote_request_options_t* options);
void ssi_transport_get_stats(ssi_transport_stats_t* stats);

#ifdef __cplusplus
}
#endif
