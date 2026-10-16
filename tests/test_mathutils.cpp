#include "common.h"
#include "doctest.h"

#include "mathutils.h"

TEST_CASE("rect2f_recanonicalize normalizes negative extents") {
	rect2f rect = RECT2F(10.0f, 20.0f, -3.0f, -7.0f);
	rect2f normalized = rect2f_recanonicalize(&rect);

	CHECK(normalized.x == doctest::Approx(7.0f));
	CHECK(normalized.y == doctest::Approx(13.0f));
	CHECK(normalized.w == doctest::Approx(3.0f));
	CHECK(normalized.h == doctest::Approx(7.0f));
}

TEST_CASE("world and pixel bounds round-trip for integer mpp") {
	bounds2f world = BOUNDS2F(20.0f, 40.0f, 60.0f, 100.0f);
	bounds2i pixels = world_bounds_to_pixel_bounds(&world, 2.0f, 4.0f);
	bounds2f roundtrip = pixel_bounds_to_world_bounds(pixels, 2.0f, 4.0f);

	CHECK(pixels.left == 10);
	CHECK(pixels.top == 10);
	CHECK(pixels.right == 30);
	CHECK(pixels.bottom == 25);
	CHECK(roundtrip.left == doctest::Approx(world.left));
	CHECK(roundtrip.top == doctest::Approx(world.top));
	CHECK(roundtrip.right == doctest::Approx(world.right));
	CHECK(roundtrip.bottom == doctest::Approx(world.bottom));
}

TEST_CASE("project_point_on_line_segment clamps to segment endpoints") {
	float t = -1.0f;
	v2f projected = project_point_on_line_segment(V2F(15.0f, 4.0f), V2F(0.0f, 0.0f), V2F(10.0f, 0.0f), &t);

	CHECK(t == doctest::Approx(1.0f));
	CHECK(projected.x == doctest::Approx(10.0f));
	CHECK(projected.y == doctest::Approx(0.0f));
}

TEST_CASE("screen-relative panning vector maps through inverse view rotation") {
	v2f right_in_world = v2f_rotate(V2F(1.0f, 0.0f), -0.5f * PI_F);
	v2f down_in_world = v2f_rotate(V2F(0.0f, 1.0f), -0.5f * PI_F);

	CHECK(right_in_world.x == doctest::Approx(0.0f).epsilon(0.0001));
	CHECK(right_in_world.y == doctest::Approx(-1.0f));
	CHECK(down_in_world.x == doctest::Approx(1.0f));
	CHECK(down_in_world.y == doctest::Approx(0.0f).epsilon(0.0001));
}

TEST_CASE("tile coordinate conversion includes touched boundary tile") {
	bounds2f world = BOUNDS2F(511.0f, 0.0f, 512.0f, 512.0f);
	bounds2i tiles = world_bounds_to_tile_bounds(&world, 512.0f, 512.0f, V2F(0.0f, 0.0f));

	CHECK(tiles.left == 0);
	CHECK(tiles.top == 0);
	CHECK(tiles.right == 2);
	CHECK(tiles.bottom == 2);
}
