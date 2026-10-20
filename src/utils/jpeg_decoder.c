#include "common.h"
#include "profiler.h"
#include "tile_buffer_pool.h"

#ifdef TARGET_EMSCRIPTEN
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif
#include "jpeglib.h"

#include "setjmp.h" // we need to use setjmp()/longjmp() for JPEG error handling

static void on_error(j_common_ptr cinfo) {
    char buffer[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message) (cinfo, buffer);
    console_print_error("JPEG decoding error: %s\n", buffer);
	longjmp(cinfo->client_data, 10001);
}

static void empty_impl(j_decompress_ptr cinfo) {

}

static void skip_input_data(j_decompress_ptr cinfo, long num_bytes) {
	struct jpeg_source_mgr *src = cinfo->src;

	if (num_bytes > 0) {
		while (num_bytes > (long) src->bytes_in_buffer) {
			num_bytes -= (long) src->bytes_in_buffer;
			(void) (*src->fill_input_buffer)(cinfo);
		}
		src->next_input_byte += (size_t) num_bytes;
		src->bytes_in_buffer -= (size_t) num_bytes;
	}
}

static boolean fill_mem_input_buffer(j_decompress_ptr cinfo) {
	static const JOCTET mybuffer[4] = {
			(JOCTET) 0xFF, (JOCTET) JPEG_EOI, 0, 0
	};

	/* Insert a fake EOI marker */

	cinfo->src->next_input_byte = mybuffer;
	cinfo->src->bytes_in_buffer = 2;

	return TRUE;
}

void setup_jpeg_source(j_decompress_ptr cinfo, uint8_t *input_ptr, uint32_t input_length) {
	struct jpeg_source_mgr *src;

	if (cinfo->src == NULL) {
		cinfo->src = (struct jpeg_source_mgr *)
				(*cinfo->mem->alloc_small)((j_common_ptr) cinfo, JPOOL_PERMANENT,
				                           sizeof(struct jpeg_source_mgr));
	}

	src = cinfo->src;
	src->init_source = empty_impl;
	src->fill_input_buffer = fill_mem_input_buffer;
	src->skip_input_data = skip_input_data;
	src->resync_to_restart = jpeg_resync_to_restart;
	src->term_source = empty_impl;
	src->bytes_in_buffer = input_length;
	src->next_input_byte = input_ptr;
}


// https://www.ridgesolutions.ie/index.php/2019/12/10/libjpeg-example-encode-jpeg-to-memory-buffer-instead-of-file/



EMSCRIPTEN_KEEPALIVE
bool jpeg_decode_tile(uint8_t *table_ptr, uint32_t table_length, uint8_t *input_ptr, uint32_t input_length, uint8_t *output_ptr, bool is_YCbCr) {
	struct jpeg_decompress_struct cinfo = {};
	struct jpeg_error_mgr jerr = {};

	// Setup error handling
	cinfo.err = jpeg_std_error(&jerr);
	jerr.error_exit = on_error;

	jmp_buf on_err_jmp_buffer = {};
	cinfo.client_data = (void*)on_err_jmp_buffer;
	int r = setjmp(on_err_jmp_buffer);
	if (r != 0) {
		// We encountered an error during JPEG decoding -> handle the failure gracefully
		jpeg_destroy_decompress(&cinfo);
		return false;
	}

	jpeg_create_decompress(&cinfo);

	// Load Jpeg table
	if (table_ptr && table_length > 0) {
		setup_jpeg_source(&cinfo, table_ptr, table_length);
		if (jpeg_read_header(&cinfo, FALSE) != JPEG_HEADER_TABLES_ONLY) {
			printf("Failed to load table\n");
			jpeg_destroy_decompress(&cinfo);
			return false;
		}
	}

	// Read tile data
	setup_jpeg_source(&cinfo, input_ptr, input_length);
	if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
		printf("Failed to read header\n");
		jpeg_destroy_decompress(&cinfo);
		return false;
	}

	cinfo.jpeg_color_space = is_YCbCr ? JCS_YCbCr : JCS_RGB;
	cinfo.out_color_space = JCS_EXT_BGRA;

	jpeg_start_decompress(&cinfo);

	int row_width = cinfo.output_width;
	int target_row_stride = row_width * 4;
	int source_row_stride = row_width * cinfo.output_components;
	JSAMPARRAY buffer = (*cinfo.mem->alloc_sarray)
			((j_common_ptr) &cinfo, JPOOL_IMAGE, source_row_stride, 1);

	while (cinfo.output_scanline < cinfo.output_height) {
		(void) jpeg_read_scanlines(&cinfo, buffer, 1);
		memcpy(output_ptr, buffer[0], target_row_stride);
		output_ptr += target_row_stride;
	}

	(void) jpeg_finish_decompress(&cinfo);

	jpeg_destroy_decompress(&cinfo);

	return true;
}

/* Heap-owned state survives libjpeg's longjmp, including any output allocation. */
typedef struct jpeg_image_decode_state_t {
	struct jpeg_decompress_struct decoder;
	struct jpeg_error_mgr error;
	jmp_buf jump;
	u8* output;
	size_t output_size;
} jpeg_image_decode_state_t;

bool jpeg_validate_baseline_ycbcr_header(u8* input, u32 input_size, u32 width, u32 height) {
	if (!input || input_size < 4 || input[0] != 0xff || input[1] != 0xd8) return false;
	/* libjpeg v6 API does not expose the SOF coding process. Inspect marker
	 * framing through SOS so truncated prefixes cannot use its synthetic EOI. */
	bool baseline = false, scan = false;
	size_t offset = 2;
	while (offset < input_size) {
		if (input[offset++] != 0xff) return false;
		while (offset < input_size && input[offset] == 0xff) ++offset;
		if (offset >= input_size) return false;
		u8 marker = input[offset++];
		if (marker == 0 || marker == 0xd8 || marker == 0xd9 ||
		    marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7) ||
		    input_size - offset < 2) return false;
		u32 length = ((u32)input[offset] << 8) | input[offset + 1];
		if (length < 2 || length > input_size - offset) return false;
		if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
			if (marker != 0xc0 || baseline) return false;
			baseline = true;
		}
		offset += length;
		if (marker == 0xda) { scan = true; break; }
	}
	if (!baseline || !scan) return false;
	jpeg_image_decode_state_t* state = calloc(1, sizeof(*state));
	if (!state) return false;
	j_decompress_ptr decoder = &state->decoder;
	decoder->err = jpeg_std_error(&state->error);
	state->error.error_exit = on_error;
	decoder->client_data = state->jump;
	if (setjmp(state->jump)) {
		jpeg_destroy_decompress(decoder);
		free(state);
		return false;
	}
	jpeg_create_decompress(decoder);
	setup_jpeg_source(decoder, input, input_size);
	bool valid = jpeg_read_header(decoder, TRUE) == JPEG_HEADER_OK &&
		decoder->image_width == width && decoder->image_height == height &&
		decoder->data_precision == 8 && decoder->num_components == 3 &&
		decoder->jpeg_color_space == JCS_YCbCr && !decoder->progressive_mode && !decoder->arith_code;
	for (i32 i = 0; i < decoder->num_components && valid; ++i) {
		const jpeg_component_info* component = decoder->comp_info + i;
		valid = component->h_samp_factor >= 1 && component->h_samp_factor <= (i == 0 ? 2 : 1) &&
			component->v_samp_factor >= 1 && component->v_samp_factor <= (i == 0 ? 2 : 1) &&
			component->quant_tbl_no < NUM_QUANT_TBLS && decoder->quant_tbl_ptrs[component->quant_tbl_no] &&
			component->dc_tbl_no < NUM_HUFF_TBLS && decoder->dc_huff_tbl_ptrs[component->dc_tbl_no] &&
			component->ac_tbl_no < NUM_HUFF_TBLS && decoder->ac_huff_tbl_ptrs[component->ac_tbl_no];
	}
	jpeg_destroy_decompress(decoder);
	free(state);
	return valid;
}

static u8* jpeg_decode_image_internal(u8* input, u32 input_size,
		i32* width, i32* height, i32* channels, J_COLOR_SPACE input_color_space,
		i32 expected_width, i32 expected_height, size_t maximum_output_size) {
	if (width) *width = 0;
	if (height) *height = 0;
	if (channels) *channels = 0;
	if (!input || !input_size) return NULL;
	jpeg_image_decode_state_t* state = calloc(1, sizeof(*state));
	if (!state) return NULL;
	j_decompress_ptr decoder = &state->decoder;
	decoder->err = jpeg_std_error(&state->error);
	state->error.error_exit = on_error;
	decoder->client_data = state->jump;
	if (setjmp(state->jump)) goto failed;
	jpeg_create_decompress(decoder);
	setup_jpeg_source(decoder, input, input_size);
	if (jpeg_read_header(decoder, TRUE) != JPEG_HEADER_OK) goto failed;
	/* Check dimensions before start_decompress can allocate image-sized buffers. */
	if (!decoder->image_width || !decoder->image_height ||
	    decoder->image_width > INT32_MAX || decoder->image_height > INT32_MAX ||
	    (expected_width > 0 && decoder->image_width != (u32)expected_width) ||
	    (expected_height > 0 && decoder->image_height != (u32)expected_height) ||
	    (u64)decoder->image_width * decoder->image_height > maximum_output_size / 4)
		goto failed;
	if (input_color_space != JCS_UNKNOWN) decoder->jpeg_color_space = input_color_space;
	decoder->out_color_space = JCS_EXT_BGRA;
	if (!jpeg_start_decompress(decoder) || decoder->output_components != 4)
		goto failed;
	size_t stride = (size_t)decoder->output_width * 4;
	state->output_size = stride * decoder->output_height;
	state->output = tile_buffer_acquire(state->output_size);
	if (!state->output) goto failed;
	while (decoder->output_scanline < decoder->output_height) {
		u8* row = state->output + (size_t)decoder->output_scanline * stride;
		if (jpeg_read_scanlines(decoder, &row, 1) != 1) goto failed;
	}
	if (!jpeg_finish_decompress(decoder)) goto failed;
	if (width) *width = (i32)decoder->output_width;
	if (height) *height = (i32)decoder->output_height;
	if (channels) *channels = 4;
	u8* result = state->output;
	jpeg_destroy_decompress(decoder);
	free(state);
	return result;


failed:
	jpeg_destroy_decompress(decoder);
	if (state->output) {
		tile_buffer_release(state->output, state->output_size);
	}
	free(state);
	return NULL;
}

u8* jpeg_decode_image_bounded(u8* input, u32 input_size,
		i32 expected_width, i32 expected_height, size_t maximum_output_size,
		i32* width, i32* height, i32* channels) {
	if (expected_width <= 0 || expected_height <= 0) return NULL;
	i64 profile_start = profiler_worker_decode_begin();
	u8* result = jpeg_decode_image_internal(input, input_size, width, height, channels,
		JCS_UNKNOWN, expected_width, expected_height, maximum_output_size);
	profiler_worker_decode_end(profile_start);
	return result;
}

u8* jpeg_decode_image(u8* input, u32 input_size, i32* width, i32* height, i32* channels) {
	i64 profile_start = profiler_worker_decode_begin();
	u8* result = jpeg_decode_image_internal(input, input_size, width, height, channels,
		JCS_UNKNOWN, 0, 0, SIZE_MAX);
	profiler_worker_decode_end(profile_start);
	return result;
}

u8* jpeg_decode_rgb_image(u8* input, u32 input_size, i32* width, i32* height, i32* channels) {
	i64 profile_start = profiler_worker_decode_begin();
	u8* result = jpeg_decode_image_internal(input, input_size, width, height, channels,
		JCS_RGB, 0, 0, SIZE_MAX);
	profiler_worker_decode_end(profile_start);
	return result;
}

u8* jpeg_decode_ndpi_image(u8* input_ptr, u32 input_length, i32 width, i32 height, i32 *channels_in_file) {
    struct jpeg_decompress_struct cinfo = {};
    struct jpeg_error_mgr jerr = {};

    // Setup error handling
    cinfo.err = jpeg_std_error(&jerr);
    jerr.error_exit = on_error;

	jmp_buf on_err_jmp_buffer = {};
	cinfo.client_data = (void*)on_err_jmp_buffer;
	int r = setjmp(on_err_jmp_buffer);
	if (r != 0) {
		// We arrived via longjmp and encountered an error -> handle the failure gracefully
		jpeg_destroy_decompress(&cinfo);
		return false;
	}

    jpeg_create_decompress(&cinfo);

    // We need to edit the SOF for image width and height so that libjpeg does not throw an error


    // Read tile data
    setup_jpeg_source(&cinfo, input_ptr, input_length);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        printf("Failed to read header\n");
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }
    cinfo.image_width = width;
    cinfo.image_height = height;

    cinfo.out_color_space = JCS_EXT_BGRA;

    jpeg_start_decompress(&cinfo);

    int row_width = cinfo.output_width;
    int target_row_stride = row_width * cinfo.output_components;
    size_t output_size = target_row_stride * cinfo.output_height;
    u8* output_buffer = malloc(output_size);

    while (cinfo.output_scanline < cinfo.output_height) {
        u8* output_pos = output_buffer + (cinfo.output_scanline) * target_row_stride;
        u8* buffer_array[1] = { output_pos };
        i32 ret = jpeg_read_scanlines(&cinfo, buffer_array, 1);
    }

//    if (width) *width = cinfo.output_width;
//    if (height) *height = cinfo.output_height;
    if (channels_in_file) *channels_in_file = cinfo.output_components;

    (void) jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    return output_buffer;
}

EMSCRIPTEN_KEEPALIVE
uint8_t *create_buffer(int size) {
	return malloc(size * sizeof(uint8_t));
}

EMSCRIPTEN_KEEPALIVE
void destroy_buffer(uint8_t *p) {
	free(p);
}

void jpeg_encode_tile(u8* pixels, i32 width, i32 height, i32 quality, u8** tables_buffer, u64* tables_size_ptr,
                      u8** jpeg_buffer, u64* jpeg_size_ptr, bool use_rgb) {
	struct jpeg_compress_struct cinfo = {};
	struct jpeg_error_mgr jerr = {};

	// Setup error handling
	cinfo.err = jpeg_std_error(&jerr);
	jerr.error_exit = on_error;

	jmp_buf on_err_jmp_buffer = {};
	cinfo.client_data = (void*)on_err_jmp_buffer;
	int r = setjmp(on_err_jmp_buffer);
	if (r != 0) {
		// We arrived via longjmp and encountered an error -> handle the failure gracefully
		jpeg_destroy_compress(&cinfo);
		return;
	}

	jpeg_create_compress(&cinfo);
	cinfo.image_width = width;
	cinfo.image_height = height;

	cinfo.input_components = 4;
	cinfo.in_color_space = JCS_EXT_BGRA;

	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);
	if (use_rgb) {
		jpeg_set_colorspace(&cinfo, JCS_RGB);
	}

	if (tables_buffer) {
		unsigned long tables_size = 0;
		jpeg_mem_dest(&cinfo, tables_buffer, &tables_size); // libjpeg-turbo will allocate the buffer
		jpeg_write_tables(&cinfo);
		if (tables_size_ptr) {
			*tables_size_ptr = tables_size;
		}
	} else {
		jpeg_suppress_tables(&cinfo, TRUE);
	}

	if (jpeg_buffer) {
		unsigned long jpeg_size = 0;
		jpeg_mem_dest(&cinfo, jpeg_buffer, &jpeg_size); // libjpeg-turbo will allocate the buffer
		jpeg_start_compress(&cinfo, FALSE);

		i32 row_stride = width * cinfo.input_components;
		JSAMPROW row_pointer[1];
		while (cinfo.next_scanline < cinfo.image_height) {
			row_pointer[0] = pixels + (cinfo.next_scanline * row_stride);
			jpeg_write_scanlines(&cinfo, row_pointer, 1);
		}

		jpeg_finish_compress(&cinfo);
		if (jpeg_size_ptr) {
			*jpeg_size_ptr = jpeg_size;
		}
	}

	jpeg_destroy_compress(&cinfo);

}

void jpeg_encode_image(u8* pixels, i32 width, i32 height, i32 quality, u8** jpeg_buffer, u64* jpeg_size_ptr) {
	struct jpeg_compress_struct cinfo = {};
	struct jpeg_error_mgr jerr = {};

	// Setup error handling
	cinfo.err = jpeg_std_error(&jerr);
	jerr.error_exit = on_error;

	jmp_buf on_err_jmp_buffer = {};
	cinfo.client_data = (void*)on_err_jmp_buffer;
	int r = setjmp(on_err_jmp_buffer);
	if (r != 0) {
		// We arrived via longjmp and encountered an error -> handle the failure gracefully
		jpeg_destroy_compress(&cinfo);
		return;
	}

	jpeg_create_compress(&cinfo);
	cinfo.image_width = width;
	cinfo.image_height = height;

	cinfo.input_components = 4;
	cinfo.in_color_space = JCS_EXT_BGRA;

	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);

	if (jpeg_buffer) {
		unsigned long jpeg_size = 0;
		jpeg_mem_dest(&cinfo, jpeg_buffer, &jpeg_size); // libjpeg-turbo will allocate the buffer
		jpeg_start_compress(&cinfo, TRUE);

		i32 row_stride = width * cinfo.input_components;
		JSAMPROW row_pointer[1];
		while (cinfo.next_scanline < cinfo.image_height) {
			row_pointer[0] = pixels + (cinfo.next_scanline * row_stride);
			jpeg_write_scanlines(&cinfo, row_pointer, 1);
		}

		jpeg_finish_compress(&cinfo);
		if (jpeg_size_ptr) {
			*jpeg_size_ptr = jpeg_size;
		}
	}

	jpeg_destroy_compress(&cinfo);
}
