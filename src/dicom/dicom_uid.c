/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2026  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#include "dicom_uid.h"

bool dicom_uid_is_valid_string(const char* value, size_t length) {
	if (!value || !length || length > 64 || value[0] == '.' || value[length - 1] == '.') return false;
	for (size_t i = 0; i < length; ++i) {
		if ((value[i] < '0' || value[i] > '9') && value[i] != '.') return false;
		if (value[i] == '.' && i > 0 && value[i - 1] == '.') return false;
		if (value[i] == '0' && (i == 0 || value[i - 1] == '.') &&
		    i + 1 < length && value[i + 1] != '.') return false;
	}
	return true;
}
