#define SHEREDOM_JSON_IMPLEMENTATION
#include "json.h"

#include "dicomweb.h"
#include "timerutils.h"

static u8* read_entire_file(const char* filename, size_t* out_size) {
	FILE* file = fopen(filename, "rb");
	if (!file) return NULL;
	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		return NULL;
	}
	long length = ftell(file);
	if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	u8* data = (u8*)malloc((size_t)length + 1);
	if (!data) {
		fclose(file);
		return NULL;
	}
	size_t bytes_read = fread(data, 1, (size_t)length, file);
	fclose(file);
	if (bytes_read != (size_t)length) {
		free(data);
		return NULL;
	}
	data[bytes_read] = '\0';
	*out_size = bytes_read;
	return data;
}

int main(int argc, char** argv) {
	init_timer();
	if (argc == 4 && strcmp(argv[1], "compare") == 0) {
		size_t size_a = 0, size_b = 0;
		u8* a = read_entire_file(argv[2], &size_a);
		u8* b = read_entire_file(argv[3], &size_b);
		dicomweb_instance_metadata_t standard = {0}, compact = {0};
		i64 start = get_clock();
		bool success = dicomweb_parse_instance_metadata((char*)a, size_a, &standard);
		double standard_ms = get_seconds_elapsed(start, get_clock()) * 1000.0;
		start = get_clock();
		success = success && dicomweb_parse_compact_metadata((char*)b, size_b, &compact);
		double compact_ms = get_seconds_elapsed(start, get_clock()) * 1000.0;
		success = success && standard.frame_map_count == compact.frame_map_count &&
			standard.number_of_frames == compact.number_of_frames &&
			standard.rows == compact.rows && standard.columns == compact.columns &&
			standard.total_pixel_matrix_columns == compact.total_pixel_matrix_columns &&
			standard.total_pixel_matrix_rows == compact.total_pixel_matrix_rows &&
			standard.is_tiled_full == compact.is_tiled_full &&
			standard.samples_per_pixel == compact.samples_per_pixel &&
			standard.is_mpp_known == compact.is_mpp_known &&
			(!standard.is_mpp_known ||
			 (fabsf(standard.mpp_x - compact.mpp_x) <= MAX(1e-6f, standard.mpp_x * 1e-5f) &&
			  fabsf(standard.mpp_y - compact.mpp_y) <= MAX(1e-6f, standard.mpp_y * 1e-5f))) &&
			strcmp(standard.study_instance_uid, compact.study_instance_uid) == 0 &&
			strcmp(standard.series_instance_uid, compact.series_instance_uid) == 0 &&
			strcmp(standard.image_flavor, compact.image_flavor) == 0 &&
			strcmp(standard.photometric_interpretation, compact.photometric_interpretation) == 0 &&
			strcmp(standard.sop_instance_uid, compact.sop_instance_uid) == 0 &&
			memcmp(standard.frame_numbers, compact.frame_numbers, standard.frame_map_count * sizeof(u32)) == 0;
		printf("compare: standard=%zu bytes %.3f ms, compact=%zu bytes %.3f ms, identical_map=%d\n",
			size_a, standard_ms, size_b, compact_ms, success);
		dicomweb_instance_metadata_destroy(&standard);
		dicomweb_instance_metadata_destroy(&compact);
		free(a);
		free(b);
		return success ? 0 : 1;
	}
	/* Offline inputs keep transport and decoder measurements independent. */
	if (argc == 6 && strcmp(argv[1], "profile") == 0) {
		size_t metadata_size = 0, frame_size = 0;
		u8* metadata_data = read_entire_file(argv[2], &metadata_size);
		u8* frame_data = read_entire_file(argv[3], &frame_size);
		i32 iterations = atoi(argv[5]);
		if (!metadata_data || !frame_data || iterations < 1 || iterations > 10000) {
			free(metadata_data);
			free(frame_data);
			return 2;
		}
		dicomweb_instance_metadata_t metadata = {0};
		i64 start = get_clock();
		bool success = dicomweb_parse_instance_metadata((char*)metadata_data, metadata_size, &metadata);
		float metadata_ms = get_seconds_elapsed(start, get_clock()) * 1000.0f;
		double multipart_ms = 0, decode_ms = 0;
		for (i32 i = 0; success && i < iterations; ++i) {
			dicomweb_frame_t frame = {0};
			start = get_clock();
			success = dicomweb_parse_multipart_frame(frame_data, frame_size, argv[4], &frame);
			multipart_ms += get_seconds_elapsed(start, get_clock()) * 1000.0;
			if (!success) break;
			start = get_clock();
			u8* pixels = dicomweb_decode_frame_to_bgra(&metadata, &frame);
			decode_ms += get_seconds_elapsed(start, get_clock()) * 1000.0;
			success = pixels != NULL;
			free(pixels);
		}
		printf("profile: metadata=%.3f ms, multipart=%.4f ms/frame, decode=%.3f ms/frame, iterations=%d, success=%d\n",
		       metadata_ms, multipart_ms / iterations, decode_ms / iterations, iterations, success);
		dicomweb_instance_metadata_destroy(&metadata);
		free(metadata_data);
		free(frame_data);
		return success ? 0 : 1;
	}
	if (argc < 3) {
		fprintf(stderr, "Usage: %s metadata FILE | frame FILE CONTENT_TYPE | frames FILE CONTENT_TYPE | profile METADATA FRAME CONTENT_TYPE ITERATIONS | compare DICOM_JSON COMPACT_JSON\n", argv[0]);
		return 2;
	}
	size_t size = 0;
	u8* data = read_entire_file(argv[2], &size);
	if (!data) {
		fprintf(stderr, "Could not read '%s'\n", argv[2]);
		return 2;
	}
	int result = 1;
	if (strcmp(argv[1], "metadata") == 0) {
		dicomweb_instance_metadata_t metadata = {0};
		if (dicomweb_parse_instance_metadata((const char*)data, size, &metadata)) {
			printf("metadata: %ux%u, tile %ux%u, %lld frames, mpp %.6g x %.6g\n",
			       metadata.total_pixel_matrix_columns, metadata.total_pixel_matrix_rows,
			       metadata.columns, metadata.rows, (long long)metadata.number_of_frames,
			       metadata.mpp_x, metadata.mpp_y);
			result = 0;
		}
		dicomweb_instance_metadata_destroy(&metadata);
	} else if (strcmp(argv[1], "frame") == 0 && argc == 4) {
		dicomweb_frame_t frame = {0};
		if (dicomweb_parse_multipart_frame(data, size, argv[3], &frame)) {
			printf("frame: %zu bytes, %s, transfer syntax %s\n", frame.size,
			       frame.media_type, frame.transfer_syntax_uid);
			result = 0;
		}
	} else if (strcmp(argv[1], "frames") == 0 && argc == 4) {
		dicomweb_frame_t frames[64] = {0};
		i32 count = dicomweb_parse_multipart_frames(data, size, argv[3],
		                                            frames, COUNT(frames));
		if (count > 0) {
			printf("frames: %d part%s", count, count == 1 ? "" : "s");
			for (i32 i = 0; i < count; ++i) {
				printf("%s%u", i ? "," : " (", frames[i].frame_number);
			}
			printf("), %s, transfer syntax %s\n", frames[0].media_type,
			       frames[0].transfer_syntax_uid);
			result = 0;
		}
	}
	if (result != 0) fprintf(stderr, "DICOMweb response could not be parsed\n");
	free(data);
	return result;
}
