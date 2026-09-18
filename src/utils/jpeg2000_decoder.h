#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"

u8* jpeg2000_decode_image_to_bgra(const u8* input, size_t input_size,
		i32* width, i32* height);

/* Header bounds apply before OpenJPEG allocates decoded component planes. */
u8* jpeg2000_decode_image_to_bgra_bounded(const u8* input, size_t input_size,
		i32 expected_width, i32 expected_height, size_t maximum_output_size,
		i32* width, i32* height);

#ifdef __cplusplus
}
#endif
