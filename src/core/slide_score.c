/*
  Slidescape, a whole-slide image viewer for digital pathology.
  Copyright (C) 2019-2024  Pieter Valkema

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "common.h"
#include "slide_score.h"
#include "remote.h"
#include "memrw.h"
#include "viewer.h"
#include "stringutils.h"

#include "json.h"

static void* slide_score_json_alloc(void* user_data, size_t size) {
    (void)user_data;
    return malloc(size);
}

web_api_binding_t slide_score_api_get_tile_server_bindings_template[] = {
        {"cookiePart", offsetof(slide_score_get_tile_server_result_t, cookie_part), FIELD_TYPE_STRING_256CHARS},
        {"urlPart", offsetof(slide_score_get_tile_server_result_t, url_part), FIELD_TYPE_STRING_256CHARS},
        {"expiresOn", offsetof(slide_score_get_tile_server_result_t, expires_on), FIELD_TYPE_STRING_256CHARS},
};
web_api_result_descriptor_t slide_score_api_get_tile_server_result_descriptor = {
        slide_score_api_get_tile_server_bindings_template, COUNT(slide_score_api_get_tile_server_bindings_template)
};

web_api_binding_t slide_score_api_get_image_metadata_bindings_template[] = {
        {"level0TileWidth", offsetof(slide_score_get_image_metadata_result_t, tile_width),       FIELD_TYPE_I32},
        {"Level0TileWidth", offsetof(slide_score_get_image_metadata_result_t, tile_width),       FIELD_TYPE_I32},
        {"level0TileHeight", offsetof(slide_score_get_image_metadata_result_t, tile_height),     FIELD_TYPE_I32},
        {"Level0TileHeight", offsetof(slide_score_get_image_metadata_result_t, tile_height),     FIELD_TYPE_I32},
        {"osdTileSize", offsetof(slide_score_get_image_metadata_result_t, osd_tile_size),        FIELD_TYPE_I32},
        {"OSDTileSize", offsetof(slide_score_get_image_metadata_result_t, osd_tile_size),        FIELD_TYPE_I32},
        {"mppX", offsetof(slide_score_get_image_metadata_result_t, mpp_x),                       FIELD_TYPE_FLOAT},
        {"MppX", offsetof(slide_score_get_image_metadata_result_t, mpp_x),                       FIELD_TYPE_FLOAT},
        {"mppY", offsetof(slide_score_get_image_metadata_result_t, mpp_y),                       FIELD_TYPE_FLOAT},
        {"MppY", offsetof(slide_score_get_image_metadata_result_t, mpp_y),                       FIELD_TYPE_FLOAT},
        {"objectivePower", offsetof(slide_score_get_image_metadata_result_t, objective_power),   FIELD_TYPE_FLOAT},
        {"ObjectivePower", offsetof(slide_score_get_image_metadata_result_t, objective_power),   FIELD_TYPE_FLOAT},
        {"backgroundColor", offsetof(slide_score_get_image_metadata_result_t, background_color), FIELD_TYPE_STRING_256CHARS},
        {"BackgroundColor", offsetof(slide_score_get_image_metadata_result_t, background_color), FIELD_TYPE_STRING_256CHARS},
        {"levelCount", offsetof(slide_score_get_image_metadata_result_t, level_count),           FIELD_TYPE_I32},
        {"LevelCount", offsetof(slide_score_get_image_metadata_result_t, level_count),           FIELD_TYPE_I32},
        {"zLayerCount", offsetof(slide_score_get_image_metadata_result_t, z_layer_count),        FIELD_TYPE_I32},
        {"ZLayerCount", offsetof(slide_score_get_image_metadata_result_t, z_layer_count),        FIELD_TYPE_I32},
        {"level0Width", offsetof(slide_score_get_image_metadata_result_t, level_0_width),        FIELD_TYPE_I64},
        {"Level0Width", offsetof(slide_score_get_image_metadata_result_t, level_0_width),        FIELD_TYPE_I64},
        {"level0Height", offsetof(slide_score_get_image_metadata_result_t, level_0_height),      FIELD_TYPE_I64},
        {"Level0Height", offsetof(slide_score_get_image_metadata_result_t, level_0_height),      FIELD_TYPE_I64},
        {"boundsX", offsetof(slide_score_get_image_metadata_result_t, bounds_x),                 FIELD_TYPE_I64},
        {"BoundsX", offsetof(slide_score_get_image_metadata_result_t, bounds_x),                 FIELD_TYPE_I64},
        {"boundsY", offsetof(slide_score_get_image_metadata_result_t, bounds_y),                 FIELD_TYPE_I64},
        {"BoundsY", offsetof(slide_score_get_image_metadata_result_t, bounds_y),                 FIELD_TYPE_I64},
        {"boundsWidth", offsetof(slide_score_get_image_metadata_result_t, bounds_width),         FIELD_TYPE_I64},
        {"BoundsWidth", offsetof(slide_score_get_image_metadata_result_t, bounds_width),         FIELD_TYPE_I64},
        {"boundsHeight", offsetof(slide_score_get_image_metadata_result_t, bounds_height),       FIELD_TYPE_I64},
        {"BoundsHeight", offsetof(slide_score_get_image_metadata_result_t, bounds_height),       FIELD_TYPE_I64},
        {"fileName", offsetof(slide_score_get_image_metadata_result_t, filename),                FIELD_TYPE_STRING_256CHARS},
        {"FileName", offsetof(slide_score_get_image_metadata_result_t, filename),                FIELD_TYPE_STRING_256CHARS},
};
web_api_result_descriptor_t slide_score_api_get_image_metadata_descriptor = {
        slide_score_api_get_image_metadata_bindings_template, COUNT(slide_score_api_get_image_metadata_bindings_template)
};


static const web_api_result_descriptor_t* slide_score_api_result_descriptors[SLIDE_SCORE_API_LAST] = {
        [SLIDE_SCORE_API_GET_IMAGE_METADATA] = &slide_score_api_get_image_metadata_descriptor,
        [SLIDE_SCORE_API_GET_TILE_SERVER] = &slide_score_api_get_tile_server_result_descriptor,
};

static const char* slide_score_api_names[SLIDE_SCORE_API_LAST] = {
        [SLIDE_SCORE_API_SCORES] = "Scores",
        [SLIDE_SCORE_API_GET_STUDIES_UPDATED] = "GetStudiesUpdated",
        [SLIDE_SCORE_API_STUDIES] = "Studies",
        [SLIDE_SCORE_API_IMAGES] = "Images",
        [SLIDE_SCORE_API_CASES] = "Cases",
        [SLIDE_SCORE_API_QUESTIONS] = "Questions",
        [SLIDE_SCORE_API_GET_SLIDE_PATH] = "GetSlidePath",
        [SLIDE_SCORE_API_GET_SLIDE_DESCRIPTION] = "GetSlideDescription",
        [SLIDE_SCORE_API_GET_SLIDE_DETAILS] = "GetSlideDetails",
        [SLIDE_SCORE_API_GET_CASE_DESCRIPTION] = "GetCaseDescription",
        [SLIDE_SCORE_API_PUBLISH] = "Publish",
        [SLIDE_SCORE_API_UNPUBLISH] = "Unpublish",
        [SLIDE_SCORE_API_DOWNLOAD_SLIDE] = "DownloadSlide",
        [SLIDE_SCORE_API_IS_SLIDE_OUT_OF_FOCUS] = "IsSlideOutOfFocus",
        [SLIDE_SCORE_API_UPSERT_DOMAIN] = "UpsertDomain",
        [SLIDE_SCORE_API_GET_DOMAIN_FOR_STUDY] = "GetDomainForStudy",
        [SLIDE_SCORE_API_SET_DOMAIN_FOR_STUDY] = "SetDomainForStudy",
        [SLIDE_SCORE_API_GENERATE_STUDENT_ACCOUNT] = "GenerateStudentAccount",
        [SLIDE_SCORE_API_GENERATE_LOGIN_LINK] = "GenerateLoginLink",
        [SLIDE_SCORE_API_GENERATE_SLIDE_FILE_URL] = "GenerateSlideFileURL",
        [SLIDE_SCORE_API_CREATE_STUDY] = "CreateStudy",
        [SLIDE_SCORE_API_UPDATE_STUDY] = "UpdateStudy",
        [SLIDE_SCORE_API_REQUEST_UPLOAD] = "RequestUpload",
        [SLIDE_SCORE_API_FINISH_UPLOAD] = "FinishUpload",
        [SLIDE_SCORE_API_ADD_SLIDE] = "AddSlide",
        [SLIDE_SCORE_API_UPLOAD_RESULTS] = "UploadResults",
        [SLIDE_SCORE_API_CONVERT_SCORE_VALUE_TO_ANNO2] = "ConvertScoreValueToAnno2",
        [SLIDE_SCORE_API_CREATE_ANNO2] = "CreateAnno2",
        [SLIDE_SCORE_API_SET_SLIDE_RESOLUTION] = "SetSlideResolution",
        [SLIDE_SCORE_API_SET_SLIDE_DESCRIPTION] = "SetSlideDescription",
        [SLIDE_SCORE_API_UPDATE_SLIDE_NAME] = "UpdateSlideName",
        [SLIDE_SCORE_API_UPDATE_CASE_NAME] = "UpdateCaseName",
        [SLIDE_SCORE_API_UPDATE_SLIDE_PATH] = "UpdateSlidePath",
        [SLIDE_SCORE_API_SET_IS_ARCHIVED] = "SetIsArchived",
        [SLIDE_SCORE_API_UPDATE_TMA_CORE_SIZE] = "UpdateTMACoreSize",
        [SLIDE_SCORE_API_DELETE_SLIDE] = "DeleteSlide",
        [SLIDE_SCORE_API_GET_QUPATH_TOKENS_FOR_STUDY] = "GetQupathTokensForStudy",
        [SLIDE_SCORE_API_ADD_CASE] = "AddCase",
        [SLIDE_SCORE_API_SET_IMAGE_CASE] = "SetImageCase",
        [SLIDE_SCORE_API_REMOVE_CASE] = "RemoveCase",
        [SLIDE_SCORE_API_DELETE_STUDY] = "DeleteStudy",
        [SLIDE_SCORE_API_UNDELETE_STUDY] = "UndeleteStudy",
        [SLIDE_SCORE_API_EXPORT_ASAP_ANNOTATIONS] = "ExportASAPAnnotations",
        [SLIDE_SCORE_API_UPLOAD_ASAP_ANNOTATIONS] = "UploadASAPAnnotations",
        [SLIDE_SCORE_API_GET_SCREENSHOT] = "GetScreenshot",
        [SLIDE_SCORE_API_SEND_EMAIL] = "SendEmail",
        [SLIDE_SCORE_API_ADD_USER] = "AddUser",
        [SLIDE_SCORE_API_REMOVE_USER] = "RemoveUser",
        [SLIDE_SCORE_API_CREATE_TMA_MAP] = "CreateTMAMap",
        [SLIDE_SCORE_API_SET_SLIDE_TMA_MAP] = "SetSlideTMAMap",
        [SLIDE_SCORE_API_ADD_QUESTION] = "AddQuestion",
        [SLIDE_SCORE_API_UPDATE_QUESTION] = "UpdateQuestion",
        [SLIDE_SCORE_API_REMOVE_QUESTION] = "RemoveQuestion",
        [SLIDE_SCORE_API_REIMPORT] = "Reimport",
        [SLIDE_SCORE_API_GET_IMAGE_METADATA] = "GetImageMetadata",
        [SLIDE_SCORE_API_GET_TILE_SERVER] = "GetTileServer",
        [SLIDE_SCORE_API_GET_TOKEN_EXPIRY] = "GetTokenExpiry",
        [SLIDE_SCORE_API_I_ENDPOINT] = "",
        [SLIDE_SCORE_API_GET_RAW_TILE] = "GetRawTile",
};

static char slide_score_last_status[512];
static bool slide_score_open_succeeded;
static slide_score_failure_kind_t slide_score_last_failure_kind;

static void slide_score_set_last_status(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(slide_score_last_status, sizeof(slide_score_last_status), fmt, args);
    va_end(args);

    console_print(slide_score_last_status);
}

const char* slide_score_get_last_status(void) {
    return slide_score_last_status;
}

bool slide_score_last_open_succeeded(void) {
    return slide_score_open_succeeded;
}

slide_score_failure_kind_t slide_score_get_last_failure_kind(void) {
	return slide_score_last_failure_kind;
}

slide_score_failure_kind_t slide_score_classify_http_failure(i32 status_code) {
	if (status_code == 401 || status_code == 403) {
		return SLIDE_SCORE_FAILURE_AUTHENTICATION;
	}
	return status_code > 0 ? SLIDE_SCORE_FAILURE_HTTP : SLIDE_SCORE_FAILURE_TRANSPORT;
}

bool slide_score_failure_requires_api_token(slide_score_failure_kind_t failure_kind) {
	return failure_kind == SLIDE_SCORE_FAILURE_CREDENTIALS_REQUIRED ||
	       failure_kind == SLIDE_SCORE_FAILURE_AUTHENTICATION;
}




void slide_score_client_init(slide_score_client_t* ss, const char* server_url_or_hostname, const char* api_key) {
    memset(ss, 0, sizeof(*ss));
    if (!server_url_or_hostname) return;

    const char* start = server_url_or_hostname;
    if (strncmp(start, "https://", 8) == 0) {
        start += 8;
    } else if (strncmp(start, "http://", 7) == 0) {
        start += 7;
    }

    size_t hostname_len = 0;
    while (start[hostname_len] && start[hostname_len] != '/' && start[hostname_len] != ':') {
        ++hostname_len;
    }
    hostname_len = ATMOST(hostname_len, sizeof(ss->server_name) - 1);
    memcpy(ss->server_name, start, hostname_len);
    ss->server_name[hostname_len] = 0;

    if (api_key) {
        copy_cstring(ss->api_key, api_key, sizeof(ss->api_key));
        trim_whitespace(ss->api_key);
    }
}

void web_api_call_destroy(web_api_call_t* call) {
    memrw_destroy(&call->url);
    memrw_destroy(&call->request);
}

web_api_call_t slide_score_build_api_call(slide_score_client_t* ss, slide_score_api_enum api, const char** par_names, const char** par_values, i32 par_count) {
    web_api_call_t call = {};
    if (api <= SLIDE_SCORE_API_UNKNOWN || api >= SLIDE_SCORE_API_LAST) {
        return call; // invalid
    }
    if (api == SLIDE_SCORE_API_I_ENDPOINT) {
        // stub
    } else {
        memrw_init(&call.url, 512);
        memrw_printf(&call.url, "/Api/%s", slide_score_api_names[api]);
        for (i32 i = 0; i < par_count; ++i) {
            char prefix = (i == 0) ? '?' : '&';
            memrw_putc(prefix, &call.url);
            memrw_write_string(par_names[i], &call.url);
            memrw_putc('=', &call.url);
            memrw_write_string_urlencode(par_values[i], &call.url);
        }
        memrw_putc(0, &call.url); // zero terminate
        call.is_valid = true;
    }

    // Build the HTTP request
    memrw_init(&call.request, 4092);


    // If the API token is provided, add it to the HTTP headers
    char token_header_string[4196] = "";
    if (ss->api_key[0] != '\0') {
        snprintf(token_header_string, sizeof(token_header_string), "Authorization: Bearer %s\r\n", ss->api_key);
    } else {
        token_header_string[0] = '\0';
    }

    static const char requestfmt[] =
            "GET %s HTTP/1.1\r\n"
            "Accept: application/json\r\n"
            //            "Accept-Encoding: gzip, deflate, br\r\n"
            "Accept-Language: en,nl;q=0.9,en-US;q=0.8,af;q=0.7\r\n%s"
            "Cache-Control: max-age=0\r\n"
            "Connection: close\r\n"
            "Host: %s\r\n"
            "Upgrade-Insecure-Requests: 1\r\n\r\n";
    memrw_printf(&call.request, requestfmt, call.url.data, token_header_string, ss->server_name);
    memrw_putc(0, &call.request); // zero terminate
//    i32 request_len = (i32)strlen((char*)call.request.data);

    return call;
}

void web_api_populate_struct_with_field(const char* field_name, const char* value, web_api_binding_t* bindings, i32 binding_count, slide_score_api_result_t* api_result) {
    u8* result_struct_generic = (u8*)&api_result->result_generic;
    for (i32 i = 0; i < binding_count; ++i) {
        web_api_binding_t* binding = bindings + i;
        if (!binding->already_filled) {
            if (strcmp(binding->name, field_name) == 0) {
                switch(binding->field_type) {
                    case FIELD_TYPE_I32: {
                        *(i32*)((result_struct_generic)+binding->offset) = atoi(value);
                    } break;
                    case FIELD_TYPE_I64: {
                        *(i64*)((result_struct_generic)+binding->offset) = atoll(value);
                    } break;
                    case FIELD_TYPE_FLOAT: {
                        *(float*)((result_struct_generic)+binding->offset) = atof(value);
                    } break;
                    case FIELD_TYPE_STRING_256CHARS: {
                        copy_cstring((((char*)result_struct_generic)+binding->offset), value, 256);
                    } break;
                    case FIELD_TYPE_STRING_512CHARS: {
                        copy_cstring((((char*)result_struct_generic)+binding->offset), value, 512);
                    } break;
                    default: break;
                }
                binding->already_filled = true;
                return;
            }
        }
    }

}

static const char* json_scalar_as_string(struct json_value_s* value) {
    if (!value) return NULL;
    if (value->type == json_type_number || value->type == json_type_string) {
        return ((struct json_string_s *)(value->payload))->string;
    }
    return NULL;
}

static bool slide_score_json_value_is_true(struct json_value_s* value) {
    return value && value->type == json_type_true;
}

static void slide_score_parse_metadata_array(const char* field_name, struct json_value_s* value, slide_score_api_result_t* api_result) {
    if ((strcmp(field_name, "downsamples") != 0 && strcmp(field_name, "Downsamples") != 0) || !value || value->type != json_type_array) return;

    slide_score_get_image_metadata_result_t* metadata = &api_result->get_image_metadata;
    struct json_array_s* array = (struct json_array_s*)value->payload;
    struct json_array_element_s* element = array->start;
    while (element && metadata->downsample_count < COUNT(metadata->downsamples)) {
        const char* number = json_scalar_as_string(element->value);
        if (number) {
            metadata->downsamples[metadata->downsample_count++] = atof(number);
        }
        element = element->next;
    }
}

slide_score_api_result_t debug_slide_score_api_handle_response(const char* json, size_t json_length, slide_score_api_enum api) {
    bool success = false;
    bool api_reported_success = false;

    const web_api_result_descriptor_t* result_descriptor = slide_score_api_result_descriptors[api];
    if (!result_descriptor) return (slide_score_api_result_t){ .api = api };

    size_t bindings_template_size = result_descriptor->binding_count * sizeof(web_api_binding_t);
    web_api_binding_t* bindings = alloca(bindings_template_size);
    memcpy(bindings, result_descriptor->bindings_template, bindings_template_size);
    slide_score_api_result_t parsed_api_result = {};
    parsed_api_result.api = api;

    struct json_value_s* root = json_parse_ex(json, json_length, json_parse_flags_default, slide_score_json_alloc, NULL, NULL);
    if (root) {
        if (root->type == json_type_object) {
            struct json_object_s* object = (struct json_object_s*)root->payload;
            struct json_object_element_s* element = object->start;
            while (element) {
                const char* element_name = element->name->string;
                // TODO: refactor code duplication
                enum json_type_e value_type = element->value->type;
                if (strcmp(element_name, "success") == 0) {
                    api_reported_success = slide_score_json_value_is_true(element->value);
                }
                if (value_type == json_type_number || value_type == json_type_string) {
                    const char* value = json_scalar_as_string(element->value);
                    web_api_populate_struct_with_field(element_name, value, bindings,
                                                       result_descriptor->binding_count, &parsed_api_result);
                }
                slide_score_parse_metadata_array(element_name, element->value, &parsed_api_result);
                // TODO: handle nested objects, arrays
                // Note: the metadata objects only exists for the GetTileMetadata call?
                if (strcmp(element_name, "metadata") == 0 && element->value->type == json_type_object) {
                    struct json_object_s* metadata_object = (struct json_object_s*)element->value->payload;
                    struct json_object_element_s* metadata_element = metadata_object->start;

                    while (metadata_element) {
                        const char* metadata_element_name = metadata_element->name->string;
                        enum json_type_e metadata_value_type = metadata_element->value->type;
                        if (metadata_value_type == json_type_number || metadata_value_type == json_type_string) {
                            const char* value = json_scalar_as_string(metadata_element->value);
                            web_api_populate_struct_with_field(metadata_element_name, value, bindings,
                                                               result_descriptor->binding_count, &parsed_api_result);
                        }
                        slide_score_parse_metadata_array(metadata_element_name, metadata_element->value, &parsed_api_result);

                        metadata_element = metadata_element->next;
                    }

                }
                element = element->next;
            }

        } else {
            DUMMY_STATEMENT;
        }


        success = api_reported_success || api == SLIDE_SCORE_API_GET_IMAGE_METADATA;
        free(root);
    }
    parsed_api_result.success = success;
    return parsed_api_result;
}

bool slide_score_request_api(slide_score_client_t* ss, slide_score_api_enum api, const char** par_names, const char** par_values, i32 par_count, slide_score_api_result_t* out_result) {
    bool success = false;
    web_api_call_t call = slide_score_build_api_call(ss, api, par_names, par_values, par_count);
    if (!call.is_valid) return false;
    if (ss->api_key[0] == '\0') {
        console_print_error("Slide Score API request has no API token configured.\n");
    }

    char url[1024];
    snprintf(url, sizeof(url), "https://%s%s", ss->server_name, (char*)call.url.data);
	http_response_error_t transport_error = HTTP_RESPONSE_ERROR_NONE;
	remote_request_options_t options = {
		.max_response_body_size = MEGABYTES(2),
		.out_error = &transport_error,
	};
    http_response_t* response = open_remote_uri_with_extra_headers_and_options(
		url, ss->api_key, NULL, &options);
    if (response) {
        if (response->status_code >= 200 && response->status_code < 300) {
            slide_score_api_result_t parsed = debug_slide_score_api_handle_response((const char*)response->buffer.data, response->content_length, api);
            if (out_result) *out_result = parsed;
            success = parsed.success || api == SLIDE_SCORE_API_GET_TILE_SERVER;
			if (success) {
				slide_score_last_failure_kind = SLIDE_SCORE_FAILURE_NONE;
			} else {
				slide_score_last_failure_kind = SLIDE_SCORE_FAILURE_INVALID_RESPONSE;
				slide_score_set_last_status("Slide Score %s returned an invalid response.",
				                            slide_score_api_names[api]);
			}
        } else {
			slide_score_last_failure_kind = slide_score_classify_http_failure(response->status_code);
			if (slide_score_last_failure_kind == SLIDE_SCORE_FAILURE_AUTHENTICATION) {
				slide_score_set_last_status("Slide Score rejected the API token (HTTP %d).",
				                            response->status_code);
			} else {
				slide_score_set_last_status("Slide Score %s request failed: HTTP %d.",
				                            slide_score_api_names[api], response->status_code);
			}
            console_print_error("Slide Score API request failed: HTTP %d for %s\n", response->status_code, url);
        }
        http_response_destroy(response);
	} else {
		slide_score_last_failure_kind = SLIDE_SCORE_FAILURE_TRANSPORT;
		slide_score_set_last_status("Slide Score %s request failed: %s.",
		                            slide_score_api_names[api],
		                            http_response_error_string(transport_error));
    }
    web_api_call_destroy(&call);
    return success;
}

bool slide_score_refresh_tile_server(slide_score_remote_image_t* remote) {
    char image_id_str[32];
    snprintf(image_id_str, sizeof(image_id_str), "%d", remote->image_id);
    const char* names[] = {"imageId"};
    const char* values[] = {image_id_str};
    slide_score_api_result_t result = {};
    if (!slide_score_request_api(&remote->client, SLIDE_SCORE_API_GET_TILE_SERVER, names, values, 1, &result)) {
        return false;
    }
    remote->tile_server = result.get_tile_server;
	bool valid = remote->tile_server.cookie_part[0] && remote->tile_server.url_part[0];
	if (!valid) {
		slide_score_last_failure_kind = SLIDE_SCORE_FAILURE_INVALID_RESPONSE;
		slide_score_set_last_status("Slide Score returned an invalid tile server response.");
	}
	return valid;
}

static i32 ceil_log2_i64(i64 value) {
    i32 result = 0;
    i64 v = 1;
    while (v < value && result < 62) {
        v <<= 1;
        ++result;
    }
    return result;
}

static bool parse_i32_after_key(const char* text, const char* key, i32* out_value);
static bool slide_score_host_is_supported(const char* host);
static bool slide_score_extract_host_from_https_uri(const char* uri, char* out_host, size_t out_host_size, const char** out_path);

char* slide_score_build_tile_url(char* buffer, size_t buffer_size, slide_score_remote_image_t* remote, i32 level, i32 tile_x, i32 tile_y) {
    double downsample = 1.0;
    if (level >= 0 && level < remote->metadata.downsample_count) {
        downsample = remote->metadata.downsamples[level];
    } else if (level > 0) {
        downsample = (double)(1 << level);
    }
    i32 downsample_log2 = (i32)floor(log2(downsample) + 0.5);
    i32 deepzoom_level = remote->max_deepzoom_level - downsample_log2;
    snprintf(buffer, buffer_size, "https://%s/i/%d/%s/i_files/%d/%d_%d.jpeg",
             remote->client.server_name, remote->image_id, remote->tile_server.url_part,
             deepzoom_level, tile_x, tile_y);
    return buffer;
}

char* slide_score_build_qupath_tile_path(char* buffer, size_t buffer_size, slide_score_remote_image_t* remote, i32 level, i32 tile_x, i32 tile_y, i32 tile_width, i32 tile_height, i64 level_width, i64 level_height) {
    double downsample = 1.0;
    if (level >= 0 && level < remote->metadata.downsample_count) {
        downsample = remote->metadata.downsamples[level];
    } else if (level > 0) {
        downsample = (double)(1 << level);
    }
    if (downsample < 1.0) downsample = 1.0;

    i64 source_x = (i64)floor((double)tile_x * (double)tile_width * downsample + 0.5);
    i64 source_y = (i64)floor((double)tile_y * (double)tile_height * downsample + 0.5);
    i64 level_x = (i64)tile_x * tile_width;
    i64 level_y = (i64)tile_y * tile_height;
    i32 request_width = (i32)CLAMP(level_width - level_x, 0, tile_width);
    i32 request_height = (i32)CLAMP(level_height - level_y, 0, tile_height);
    snprintf(buffer, buffer_size, "%sraw/%d/%lld_%lld/%d_%d.jpeg",
             remote->qupath_base_path, level, source_x, source_y, request_width, request_height);
    return buffer;
}

static bool slide_score_read_api_key_file(const char* filename, char* buffer, size_t buffer_size) {
    mem_t* key_file = platform_read_entire_file(filename);
    if (key_file) {
        size_t key_len = ATMOST(key_file->len, buffer_size - 1);
        memcpy(buffer, key_file->data, key_len);
        buffer[key_len] = 0;
        free(key_file);
        return true;
    }
    return false;
}

static void slide_score_read_api_key(char* buffer, size_t buffer_size) {
    static const char leaf_filename[] = "api_key.txt";
    buffer[0] = 0;
#ifndef SLIDESCAPE_TEST_FIXTURE_MANIFEST
    if (global_settings_dir) {
        char key_filename[1024];
        snprintf(key_filename, sizeof(key_filename), "%s" PATH_SEP "%s", global_settings_dir, leaf_filename);
        if (slide_score_read_api_key_file(key_filename, buffer, buffer_size)) {
            return;
        }
    }
#endif
    slide_score_read_api_key_file(leaf_filename, buffer, buffer_size);
}

bool slide_score_open_remote_image(app_state_t* app_state, const char* server_url_or_hostname, const char* api_token, i32 image_id) {
    // Read API lazily if not provided
    if (api_token == NULL) {
        char* api_key = (char*)alloca(4096);
        api_key[0] = '\0';
        slide_score_read_api_key(api_key, 4096);
        api_token = api_key;
    }

    slide_score_remote_image_t remote = {};
    slide_score_client_init(&remote.client, server_url_or_hostname, api_token);
    remote.image_id = image_id;
	if (!remote.client.api_key[0]) {
		slide_score_last_failure_kind = SLIDE_SCORE_FAILURE_CREDENTIALS_REQUIRED;
		slide_score_set_last_status("A Slide Score API token is required.");
		return false;
	}

    char image_id_str[32];
    snprintf(image_id_str, sizeof(image_id_str), "%d", image_id);
    const char* names[] = {"imageId"};
    const char* values[] = {image_id_str};

    slide_score_api_result_t metadata_result = {};
    if (!slide_score_request_api(&remote.client, SLIDE_SCORE_API_GET_IMAGE_METADATA, names, values, 1, &metadata_result)) {
        console_print_error("%s\n", slide_score_last_status);
        return false;
    }
    remote.metadata = metadata_result.get_image_metadata;
    remote.max_deepzoom_level = ceil_log2_i64(MAX(remote.metadata.level_0_width, remote.metadata.level_0_height));

    if (!slide_score_refresh_tile_server(&remote)) {
        console_print_error("%s\n", slide_score_last_status);
        return false;
    }

    image_t* image = (image_t*)calloc(1, sizeof(image_t));
    image->resource_id = global_next_resource_id++;
    bool is_valid = init_image_from_slide_score(image, &remote, false);
    if (image->is_valid && !image_initialize_runtime(image)) {
        image->is_valid = false;
        is_valid = false;
    }
    if (is_valid) {
        unload_all_images(app_state);
        add_image(app_state, image, true, false);
		slide_score_last_failure_kind = SLIDE_SCORE_FAILURE_NONE;
        slide_score_set_last_status("Opened Slide Score image %d.", image_id);
    } else {
        image_destroy(image);
        free(image);
    }
    return is_valid;
}

static bool slide_score_extract_qupath_base_path(const char* path, char* out_base_path, size_t out_base_path_size) {
    const char* suffix = strstr(path, "SlideScoreMetadata.json");
    if (!suffix) return false;
    size_t base_len = ATMOST((size_t)(suffix - path), out_base_path_size - 1);
    memcpy(out_base_path, path, base_len);
    out_base_path[base_len] = 0;
    return base_len > 0;
}

static bool slide_score_validate_qupath_link(slide_score_remote_image_t* remote) {
    char path[512];
    slide_score_build_qupath_tile_path(path, sizeof(path), remote, 0, 0, 0,
                                       remote->metadata.tile_width, remote->metadata.tile_height,
                                       remote->metadata.level_0_width, remote->metadata.level_0_height);

    char url[1024];
    snprintf(url, sizeof(url), "https://%s%s", remote->client.server_name, path);
	http_response_error_t transport_error = HTTP_RESPONSE_ERROR_NONE;
	remote_request_options_t options = {
		.max_response_body_size = MEGABYTES(2),
		.out_error = &transport_error,
	};
    http_response_t* response = open_remote_uri_with_extra_headers_and_options(
		url, NULL, NULL, &options);
    if (!response) {
		slide_score_last_failure_kind = SLIDE_SCORE_FAILURE_TRANSPORT;
        slide_score_set_last_status("QuPath link validation failed: %s.",
		                            http_response_error_string(transport_error));
        console_print_error("%s\n", slide_score_last_status);
        return false;
    }

    bool success = response->status_code >= 200 && response->status_code < 300 && response->content_length > 0;
    if (!success) {
		slide_score_last_failure_kind = slide_score_classify_http_failure(response->status_code);
        slide_score_set_last_status("QuPath link appears expired or invalid (HTTP %d while requesting first tile).", response->status_code);
        console_print_error("%s\n", slide_score_last_status);
    }
    http_response_destroy(response);
    return success;
}

bool slide_score_open_qupath_metadata_url(app_state_t* app_state, const char* uri) {
    char server[256] = "";
    const char* path = NULL;
    if (!slide_score_extract_host_from_https_uri(uri, server, sizeof(server), &path) || !slide_score_host_is_supported(server)) {
        return false;
    }

    slide_score_remote_image_t remote = {};
    slide_score_client_init(&remote.client, server, NULL);
    remote.use_qupath_tile_endpoint = true;
    if (!parse_i32_after_key(path, "/i/", &remote.image_id)) {
        console_print_error("Slide Score QuPath link recognized, but no image id could be found: %s\n", uri);
        return false;
    }
    if (!slide_score_extract_qupath_base_path(path, remote.qupath_base_path, sizeof(remote.qupath_base_path))) {
        console_print_error("Slide Score QuPath link recognized, but no metadata base path could be found: %s\n", uri);
        return false;
    }

	http_response_error_t transport_error = HTTP_RESPONSE_ERROR_NONE;
	remote_request_options_t options = {
		.max_response_body_size = MEGABYTES(2),
		.out_error = &transport_error,
	};
    http_response_t* response = open_remote_uri_with_extra_headers_and_options(
		uri, NULL, NULL, &options);
    if (!response) {
		slide_score_last_failure_kind = SLIDE_SCORE_FAILURE_TRANSPORT;
        slide_score_set_last_status("Slide Score failed to retrieve QuPath metadata: %s.",
		                            http_response_error_string(transport_error));
        console_print_error("%s\n", slide_score_last_status);
        return false;
    }

    bool success = false;
    if (response->status_code >= 200 && response->status_code < 300) {
        slide_score_api_result_t parsed = debug_slide_score_api_handle_response((const char*)response->buffer.data,
                                                                                response->content_length,
                                                                                SLIDE_SCORE_API_GET_IMAGE_METADATA);
        remote.metadata = parsed.get_image_metadata;
        remote.max_deepzoom_level = ceil_log2_i64(MAX(remote.metadata.level_0_width, remote.metadata.level_0_height));

        if (!slide_score_validate_qupath_link(&remote)) {
            http_response_destroy(response);
            return false;
        }

        image_t* image = (image_t*)calloc(1, sizeof(image_t));
        image->resource_id = global_next_resource_id++;
        bool is_valid = init_image_from_slide_score(image, &remote, false);
        if (image->is_valid && !image_initialize_runtime(image)) {
            image->is_valid = false;
            is_valid = false;
        }
        if (is_valid) {
            unload_all_images(app_state);
            add_image(app_state, image, true, false);
			slide_score_last_failure_kind = SLIDE_SCORE_FAILURE_NONE;
            slide_score_set_last_status("Opened Slide Score QuPath link for image %d.", remote.image_id);
            success = true;
        } else {
            image_destroy(image);
            free(image);
        }
    } else {
		slide_score_last_failure_kind = slide_score_classify_http_failure(response->status_code);
        slide_score_set_last_status("Slide Score QuPath metadata request failed: HTTP %d.", response->status_code);
        console_print_error("%s URL: %s\n", slide_score_last_status, uri);
    }

    http_response_destroy(response);
    return success;
}

static bool parse_i32_after_key(const char* text, const char* key, i32* out_value) {
    const char* pos = strstr(text, key);
    if (!pos) return false;
    pos += strlen(key);
    if (*pos == '=' || *pos == '/') ++pos;
    if (!isdigit(*pos)) return false;
    *out_value = atoi(pos);
    return true;
}

static bool slide_score_host_is_supported(const char* host) {
    if (!host) return false;
    size_t len = strlen(host);
    static const char suffix[] = ".slidescore.com";
    size_t suffix_len = strlen(suffix);
    return strcmp(host, "slidescore.com") == 0 || (len > suffix_len && strcmp(host + len - suffix_len, suffix) == 0);
}

static bool slide_score_extract_host_from_https_uri(const char* uri, char* out_host, size_t out_host_size, const char** out_path) {
    const char* host_start = NULL;
    if (strncmp(uri, "https://", 8) == 0 || strncmp(uri, "http://", 7) == 0) {
        host_start = strstr(uri, "://") + 3;
    } else {
        host_start = uri;
    }
    const char* host_end = strchr(host_start, '/');
    if (!host_end) {
        host_end = uri + strlen(uri);
    }
    size_t host_len = ATMOST((size_t)(host_end - host_start), out_host_size - 1);
    memcpy(out_host, host_start, host_len);
    out_host[host_len] = 0;
    if (out_path) {
        *out_path = (*host_end == '/') ? host_end : "/";
    }
    return true;
}

typedef struct slide_score_uri_parts_t {
    char server[256];
    const char* path;
    char effective_uri[1024];
} slide_score_uri_parts_t;

static bool slide_score_parse_image_id_from_path(const char* path, i32* out_image_id) {
    if (strstr(path, "/Image/Details") && parse_i32_after_key(path, "imageId", out_image_id)) {
        return true;
    }
    return parse_i32_after_key(path, "/image/", out_image_id) ||
           parse_i32_after_key(path, "/Image/", out_image_id) ||
           parse_i32_after_key(path, "/i/", out_image_id);
}

static bool slide_score_get_uri_parts(const char* uri, slide_score_uri_parts_t* parts) {
    memset(parts, 0, sizeof(*parts));
    if (strncmp(uri, "slidescore://", 13) == 0) {
        const char* host_start = uri + 13;
        const char* path_start = strchr(host_start, '/');
        const char* host_end = path_start ? path_start : uri + strlen(uri);
        size_t host_len = ATMOST((size_t)(host_end - host_start), sizeof(parts->server) - 1);
        memcpy(parts->server, host_start, host_len);
        parts->server[host_len] = 0;
        if (!slide_score_host_is_supported(parts->server)) return false;
        parts->path = path_start ? path_start : "/";
        snprintf(parts->effective_uri, sizeof(parts->effective_uri), "https://%s%s", parts->server, parts->path);
        return true;
    }

    if (!slide_score_extract_host_from_https_uri(uri, parts->server, sizeof(parts->server), &parts->path) ||
        !slide_score_host_is_supported(parts->server)) {
        return false;
    }

    if (strncmp(uri, "https://", 8) == 0 || strncmp(uri, "http://", 7) == 0) {
        snprintf(parts->effective_uri, sizeof(parts->effective_uri), "%s", uri);
    } else {
        snprintf(parts->effective_uri, sizeof(parts->effective_uri), "https://%s", uri);
    }
    return true;
}

bool slide_score_uri_is_supported(const char* uri) {
    slide_score_uri_parts_t parts = {};
    return slide_score_get_uri_parts(uri, &parts);
}

bool slide_score_try_open_uri(app_state_t* app_state, const char* uri, const char* api_token) {
    slide_score_open_succeeded = false;
	slide_score_last_failure_kind = SLIDE_SCORE_FAILURE_NONE;
	slide_score_last_status[0] = '\0';
    i32 image_id = 0;
    slide_score_uri_parts_t parts = {};
    if (!slide_score_get_uri_parts(uri, &parts)) return false;

    if (strstr(parts.path, "SlideScoreMetadata.json")) {
        slide_score_open_succeeded =
            slide_score_open_qupath_metadata_url(app_state, parts.effective_uri);
        return true;
    }

    if (!slide_score_parse_image_id_from_path(parts.path, &image_id)) {
        slide_score_set_last_status("Slide Score URI recognized, but no image id could be found.");
        console_print_error("%s URL: %s\n", slide_score_last_status, uri);
        return true;
    }

    slide_score_open_succeeded =
        slide_score_open_remote_image(app_state, parts.server, api_token, image_id);
    return true;
}

void slide_score_get_image_metadata(slide_score_client_t* ss, i32 image_id) {
    char image_id_str[16];
    snprintf(image_id_str, sizeof(image_id_str), "%d", image_id);
    const char* names[] = {"imageId"};
    const char* values[] = {image_id_str};
    web_api_call_t call = slide_score_build_api_call(ss, SLIDE_SCORE_API_GET_IMAGE_METADATA, names, values, 1);
    if (call.is_valid) {
//        do_http_request()
    }

    char url[512];
//    snprintf("https://")
}
