#include "doctest.h"
#include "replay.h"
#include <vector>

static mem_t* make_replay(const replay_command_t* commands, size_t count) {
	FILE* file = tmpfile();
	REQUIRE(file);
	REQUIRE(replay_write_header(file));
	for (size_t i = 0; i < count; ++i) {
		REQUIRE(replay_write_command(file, commands + i));
	}
	long size = ftell(file);
	REQUIRE(size > 0);
	mem_t* buffer = (mem_t*)calloc(1, sizeof(mem_t) + size);
	REQUIRE(buffer);
	buffer->len = buffer->capacity = size;
	rewind(file);
	REQUIRE(fread(buffer->data, 1, size, file) == (size_t)size);
	fclose(file);
	return buffer;
}

struct replay_test_context_t {
	bool ready = false;
	std::vector<replay_command_t> executed;
};

static replay_result_t execute_test_command(void* userdata, const replay_command_t* command) {
	replay_test_context_t* context = (replay_test_context_t*)userdata;
	if (command->kind == REPLAY_WAIT && !context->ready) {
		return REPLAY_PENDING;
	}
	context->executed.push_back(*command);
	return REPLAY_DONE;
}

TEST_CASE("replay timestamps restart only after synchronization completes") {
	replay_command_t commands[6] = {};
	commands[0].kind = REPLAY_OPEN;
	commands[0].path = (char*)"slide.mrxs";
	commands[1].kind = REPLAY_WAIT;
	commands[1].timestamp = 2.0f;
	commands[2].kind = REPLAY_CAMERA;
	commands[2].timestamp = 1.0f;
	commands[2].camera[0] = 200;
	commands[2].camera[1] = 100;
	commands[2].camera[2] = 2;
	commands[2].has_zoom = true;
	commands[3] = commands[2];
	commands[3].timestamp = 1.5f;
	commands[3].has_zoom = false;
	commands[4].kind = REPLAY_CLOSE;
	commands[4].timestamp = 2.0f;
	commands[5].kind = REPLAY_EXIT;
	commands[5].timestamp = 3.0f;
	replay_t replay = {};
	REQUIRE(replay_init(&replay, make_replay(commands, COUNT(commands))));
	replay_test_context_t context;
	replay_tick(&replay, 100.0, execute_test_command, &context);
	REQUIRE(context.executed.size() == 1);
	CHECK(strcmp(context.executed[0].path, "slide.mrxs") == 0);
	replay_tick(&replay, 101.0, execute_test_command, &context);
	replay_tick(&replay, 105.0, execute_test_command, &context);
	CHECK(context.executed.size() == 1);
	context.ready = true;
	replay_tick(&replay, 110.0, execute_test_command, &context);
	CHECK(replay.origin == 110.0);
	CHECK(replay.segment == 1);
	replay_tick(&replay, 110.9, execute_test_command, &context);
	CHECK(context.executed.size() == 2);
	// A late frame catches up both camera commands; optional zoom remains distinguishable.
	replay_tick(&replay, 111.6, execute_test_command, &context);
	REQUIRE(context.executed.size() == 4);
	CHECK(context.executed[2].has_zoom);
	CHECK(context.executed[2].camera[2] == 2.0f);
	CHECK_FALSE(context.executed[3].has_zoom);
	replay_tick(&replay, 112.0, execute_test_command, &context);
	replay_tick(&replay, 113.0, execute_test_command, &context);
	CHECK(context.executed.size() == 6);
	CHECK(replay.finished);
	CHECK_FALSE(replay.error);
	replay_destroy(&replay);
}

TEST_CASE("replay rejects truncated payloads before dispatch") {
	replay_command_t command = {};
	command.kind = REPLAY_CAMERA;
	command.has_zoom = true;
	command.camera[2] = 2;
	mem_t* complete = make_replay(&command, 1);
	for (size_t size = 0; size < complete->len; ++size) {
		// Header-only is a valid empty replay.
		if (size == 12) {
			continue;
		}
		mem_t* truncated = (mem_t*)calloc(1, sizeof(mem_t) + size);
		truncated->len = truncated->capacity = size;
		memcpy(truncated->data, complete->data, size);
		replay_t replay = {};
		replay_test_context_t context;
		if (replay_init(&replay, truncated)) {
			replay_tick(&replay, 0, execute_test_command, &context);
		}
		CHECK(replay.error);
		CHECK(context.executed.empty());
		replay_destroy(&replay);
	}
	free(complete);
}

TEST_CASE("replay validates command schemas and numeric arguments") {
	for (i32 mutation = 0; mutation < 7; ++mutation) {
		replay_command_t command = {};
		command.kind = REPLAY_CAMERA;
		command.has_zoom = true;
		if (mutation == 0) {
			command.timestamp = -1;
		} else if (mutation == 1) {
			command.timestamp = NAN;
		} else if (mutation == 2) {
			command.camera[0] = INFINITY;
		} else if (mutation == 3) {
			command.camera[2] = 50;
		}
		mem_t* buffer = make_replay(&command, 1);
		if (mutation == 4) {
			buffer->data[16] = 99; // Unknown command ID.
		} else if (mutation == 5) {
			buffer->data[20] = 4; // Wrong argument count.
		} else if (mutation == 6) {
			memset(buffer->data + 24, 255, 4); // Overflow-sized payload.
		}
		replay_t replay = {};
		REQUIRE(replay_init(&replay, buffer));
		replay_test_context_t context;
		replay_tick(&replay, 0, execute_test_command, &context);
		CHECK(replay.error);
		CHECK(context.executed.empty());
		replay_destroy(&replay);
	}
}

TEST_CASE("replay reterminates strings and times out stalled synchronization") {
	replay_command_t commands[2] = {};
	commands[0].kind = REPLAY_OPEN;
	commands[0].path = (char*)"slide.tiff";
	commands[1].kind = REPLAY_WAIT;
	mem_t* buffer = make_replay(commands, 2);
	buffer->data[28 + strlen(commands[0].path)] = 'X';
	replay_t replay = {};
	REQUIRE(replay_init(&replay, buffer));
	replay_test_context_t context;
	replay_tick(&replay, 0, execute_test_command, &context);
	REQUIRE(context.executed.size() == 1);
	CHECK(strcmp(context.executed[0].path, "slide.tiff") == 0);
	replay_tick(&replay, 1, execute_test_command, &context);
	replay_tick(&replay, 122, execute_test_command, &context);
	CHECK(replay.error);
	CHECK_FALSE(replay.finished);
	replay_destroy(&replay);
}
