#define SHEREDOM_JSON_IMPLEMENTATION
#include "json.h"

#include "dicomweb.h"

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
	if (argc < 3) {
		fprintf(stderr, "Usage: %s metadata FILE | frame FILE CONTENT_TYPE\n", argv[0]);
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
	} else if (strcmp(argv[1], "frame") == 0 && argc == 4) {
		dicomweb_frame_t frame = {0};
		if (dicomweb_parse_multipart_frame(data, size, argv[3], &frame)) {
			printf("frame: %zu bytes, %s, transfer syntax %s\n", frame.size,
			       frame.media_type, frame.transfer_syntax_uid);
			result = 0;
		}
	}
	if (result != 0) fprintf(stderr, "DICOMweb response could not be parsed\n");
	free(data);
	return result;
}
