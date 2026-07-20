#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"

u8* jpeg2000_decode_image_to_bgra(const u8* input, size_t input_size,
		i32* width, i32* height);

#ifdef __cplusplus
}
#endif
