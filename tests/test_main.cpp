#define DOCTEST_CONFIG_IMPLEMENT
#include "common.h"
#include "doctest.h"
#include "timerutils.h"

int main(int argc, char** argv) {
	init_timer();
	doctest::Context context(argc, argv);
	return context.run();
}
