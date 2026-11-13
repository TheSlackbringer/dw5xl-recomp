// DW5XL-TEST issue 049: the Vulkan renderer's unit tests as one program. Each stays a CTest of its own, with its
// own name, checks and exit code (77: no Vulkan device, skipped); the program runs the one named by its argument.
// One program means one shader cache file of the graphics driver instead of five (test_one_program.h).
#include <cstdio>
#include <cstring>

int dw5xl_test_main_gs_picture_reads();
int dw5xl_test_main_gs_window_support();
int dw5xl_test_main_gs_second_memory();
int dw5xl_test_main_gs_turns();
int dw5xl_test_main_gs_picture_sync();

int main(int argc, char **argv)
{
    static const struct
    {
        const char *name;
        int (*run)();
    } tests[] = {
        {"gs_picture_reads", dw5xl_test_main_gs_picture_reads},
        {"gs_window_support", dw5xl_test_main_gs_window_support},
        {"gs_second_memory", dw5xl_test_main_gs_second_memory},
        {"gs_turns", dw5xl_test_main_gs_turns},
        {"gs_picture_sync", dw5xl_test_main_gs_picture_sync},
    };
    if (argc == 2)
        for (const auto &test : tests)
            if (std::strcmp(argv[1], test.name) == 0)
                return test.run();
    std::fprintf(stderr, "usage: %s <test>\ntests:", argc > 0 ? argv[0] : "dw5xl_gs_renderer_tests");
    for (const auto &test : tests)
        std::fprintf(stderr, " %s", test.name);
    std::fprintf(stderr, "\n");
    return 2;
}
