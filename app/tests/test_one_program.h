// DW5XL-TEST: several tests in one program. Built alone, a test's entry point is main; built into a program with
// others (DW5XL_ONE_TEST_PROGRAM), it gets a name of its own and the program's main picks one by its argument.
// Why (issue 049, found 2026-11-13): the graphics driver keeps a shader cache file for every program that draws
// with the renderer; five renderer tests as five programs wrote 412 MB into a folder that holds 1 GB and pushed
// the game's own compiled shaders out right before timed runs.
#pragma once

#ifdef DW5XL_ONE_TEST_PROGRAM
#define DW5XL_TEST_MAIN(name) dw5xl_test_main_##name
#else
#define DW5XL_TEST_MAIN(name) main
#endif
