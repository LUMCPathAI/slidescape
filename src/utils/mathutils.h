/*
  BSD 2-Clause License

  Copyright (c) 2019-2024, Pieter Valkema

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
     list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define FLOAT_TO_BYTE(x) ((u8)(255.0f * CLAMP((x), 0.0f, 1.0f)))
#define BYTE_TO_FLOAT(x) CLAMP(((float)((x & 0x0000ff))) /255.0f, 0.0f, 1.0f)
#define MAKE_UINT32(u0, u1, u2, u3) ((u32)(u0) | ((u32)(u1) << 8) | ((u32)(u2) << 16) | ((u32)(u3) << 24))
#define MAKE_BGRA(r,g,b,a) (((u32)(a)) << 24u | ((u32)(r)) << 16u | ((u32)(g)) << 8u | ((u32)(b)) << 0u)
#define MAKE_RGBA(r,g,b,a) (((u32)(a)) << 24u | ((u32)(r)) << 0u | ((u32)(g)) << 8u | ((u32)(b)) << 16u)
#define BGRA_SET_ALPHA(p, a) (((p) & 0x00FFFFFF) | (((u32)(a)) << 24u))

#define PI_F 3.14159265358979323846f

#pragma pack(push, 1)
typedef struct rect2i {
	i32 x, y, w, h;
} rect2i;
FORCE_INLINE rect2i RECT2I(i32 x, i32 y, i32 w, i32 h) {rect2i r = {x, y, w, h}; return r;}


typedef struct rect2f {
	float x, y, w, h;
} rect2f;
FORCE_INLINE rect2f RECT2F(float x, float y, float w, float h) {rect2f r = {x, y, w, h}; return r;}

typedef struct v2i {
	i32 x, y;
} v2i;
FORCE_INLINE v2i V2I(i32 x, i32 y) {v2i v = {x, y}; return v; }

typedef struct rgba_t {
	union {
		struct { u8 r, g, b, a; };
		u8 values[4];
	};
} rgba_t;
FORCE_INLINE rgba_t RGBA(u8 r, u8 g, u8 b, u8 a) {rgba_t rgba = {{{r, g, b, a}}}; return rgba; }

#ifndef V2F_DEFINED
#define V2F_DEFINED
typedef struct v2f {
	float x, y;
} v2f;
#endif
FORCE_INLINE v2f V2F(float x, float y) {v2f v = {x, y}; return v; }

typedef struct v3f {
	union {
		struct {float r, g, b; };
		struct {float x, y, z; };
		float values[3];
	};
} v3f;
FORCE_INLINE v3f V3F(float x, float y, float z) {v3f v = {{{x, y, z}}}; return v; }

#ifndef V4F_DEFINED
#define V4F_DEFINED
typedef struct v4f {
	union {
		struct {float r, g, b, a; };
		struct {float x, y, z, w; };
		float values[4];
	};
} v4f;
#endif
FORCE_INLINE v4f V4F(float x, float y, float z, float w) {v4f v = {{{x, y, z, w}}}; return v; }

typedef struct bounds2i {
	union {
		struct { i32 left, top, right, bottom; };
		struct { v2i min, max; };
	};
} bounds2i;
FORCE_INLINE bounds2i BOUNDS2I(i32 left, i32 top, i32 right, i32 bottom) {bounds2i b = {{{left, top, right, bottom}}}; return b;}


typedef struct bounds2f {
	union {
		struct { float left, top, right, bottom; };
		struct { v2f min, max; };
	};
} bounds2f;
FORCE_INLINE bounds2f BOUNDS2F(float left, float top, float right, float bottom) {bounds2f b = {{{left, top, right, bottom}}}; return b;}

typedef struct polygon4v2f {
	union {
		struct { v2f topleft, topright, bottomleft, bottomright; };
		v2f values[4];
	};
} polygon4v2f;

typedef enum corner_enum {
	CORNER_TOP_LEFT = 0,
	CORNER_TOP_RIGHT = 1,
	CORNER_BOTTOM_LEFT = 2,
	CORNER_BOTTOM_RIGHT = 3,
} corner_enum;



#pragma pack(pop)

FORCE_INLINE float v2i_length(v2i v)                       { return sqrtf((float)SQUARE(v.x) + (float)SQUARE(v.y)); }
FORCE_INLINE float v2f_length(v2f v)                       { return sqrtf(SQUARE(v.x) + SQUARE(v.y)); }
FORCE_INLINE float v2f_length_squared(v2f v)               { return SQUARE(v.x) + SQUARE(v.y); }

FORCE_INLINE v2f v2f_add(v2f a, v2f b)                     { v2f result = {a.x + b.x, a.y + b.y }; return result; }
FORCE_INLINE v2i v2i_add(v2i a, v2i b)                     { v2i result = {a.x + b.x, a.y + b.y }; return result; }
FORCE_INLINE v2f v2f_subtract(v2f a, v2f b)                { v2f result = {a.x - b.x, a.y - b.y }; return result; }
FORCE_INLINE v2i v2i_subtract(v2i a, v2i b)                { v2i result = {a.x - b.x, a.y - b.y }; return result; }
FORCE_INLINE float v2f_dot(v2f a, v2f b)                   { return a.x * b.x + a.y * b.y; }
FORCE_INLINE v2f v2f_scale(float scalar, v2f v)            { v2f result = {v.x * scalar, v.y * scalar }; return result; }
FORCE_INLINE v2f v2f_rotate(v2f v, float radians) {
	float c = cosf(radians);
	float s = sinf(radians);
	v2f result = {v.x * c - v.y * s, v.x * s + v.y * c};
	return result;
}
FORCE_INLINE v2f v2f_lerp(v2f a, v2f b_minus_a, float t)   { v2f result = v2f_add(a, v2f_scale(t, b_minus_a)); return result; }

// prototypes
rect2i clip_rect(rect2i* first, rect2i* second);
bounds2i clip_bounds2i(bounds2i a, bounds2i b);
bounds2f clip_bounds2f(bounds2f a, bounds2f b);
bool is_point_inside_rect2i(rect2i rect, v2i point);
bool is_point_inside_bounds2i(bounds2i bounds, v2i point);
v2i rect2i_center_point(rect2i rect);
v2f rect2f_center_point(rect2f rect);
rect2f rect2f_recanonicalize(rect2f* rect);
bounds2f rect2f_to_bounds(rect2f rect);
rect2f bounds2f_to_rect(bounds2f bounds);
bounds2f bounds2f_encompassing(bounds2f a, bounds2f b);
bool are_bounds2f_overlapping(bounds2f a, bounds2f b);
v2i world_pos_to_pixel_pos(v2f world_pos, float um_per_pixel, i32 level);
i32 tile_pos_from_world_pos(float world_pos, float tile_side);
bounds2i world_bounds_to_tile_bounds(bounds2f* world_bounds, float tile_width, float tile_height, v2f image_pos);
bounds2f tile_bounds_to_world_bounds(bounds2i tile_bounds, float tile_width, float tile_height, v2f image_pos);
bounds2f bounds_from_center_point(v2f center, float r_minus_l, float t_minus_b);
bounds2f bounds_from_pivot_point(v2f pivot, v2f pivot_relative_pos, float r_minus_l, float t_minus_b);
bounds2f bounds_from_points(v2f* points, i32 point_count);
polygon4v2f rotated_rectangle(float width, float height, float rotation);
bounds2i world_bounds_to_pixel_bounds(bounds2f* world_bounds, float mpp_x, float mpp_y);
bounds2f pixel_bounds_to_world_bounds(bounds2i pixel_bounds, float mpp_x, float mpp_y);
rect2f pixel_rect_to_world_rect(rect2i pixel_rect, float mpp_x, float mpp_y);
v2f project_point_on_line_segment(v2f point, v2f line_start, v2f line_end, float* t_ptr);
bool v2f_within_bounds(bounds2f bounds, v2f point);
bool v2f_between_points(v2f v, v2f p0, v2f p1);
v2f v2f_average(v2f a, v2f b);
corner_enum get_closest_corner(v2f center_point, v2f p);
v2f get_corner_pos(rect2f rect, corner_enum corner);

// globals
#if defined(MATHUTILS_IMPL)
#define INIT(...) __VA_ARGS__
#define extern
#else
#define INIT(...)
#undef extern
#endif

//...

#undef INIT
#undef extern


#ifdef __cplusplus
}
#endif

