/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#ifndef SLIDESCAPE_ARCHIVE_HTTP_H
#define SLIDESCAPE_ARCHIVE_HTTP_H

#include "archive_repository.h"
#include "common.h"

int archive_http_serve(const archive_repository_t* repository, u16 port,
		size_t max_requests);

#endif
