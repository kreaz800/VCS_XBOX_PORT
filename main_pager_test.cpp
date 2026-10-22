#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>

#include "psprecomp/runtime.hpp"

static volatile unsigned g_step0 = 0;
static volatile unsigned g_step1 = 0;
static volatile unsigned g_chain_unwound = 0;

__attribute__((noinline, section(".b00")))
static void test_unit_0(psprecomp::Runtime &rt,
                        psprecomp::AllegrexContext &ctx)
{
    g_step0 = 1;

    // Unit 8 begins at:
    // 0x08804000 + (8 * 0x4000) = 0x08824000
    ctx.pc = 0x08824000u;

    // .b01 is not resident while .b00 is executing.
    // The runtime should refuse to chain directly and unwind.
    if (!rt.invoke_chained_call(ctx))
        g_chain_unwound = 1;
}

__attribute__((noinline, section(".b01")))
static void test_unit_8(psprecomp::Runtime &rt,
                        psprecomp::AllegrexContext &ctx)
{
    (void)rt;
    (void)ctx;

    g_step1 = 1;
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    debugPrint("Automatic Xbox bank pager test\n");

    psprecomp::Runtime runtime(32u * 1024u * 1024u);

    debugPrint("Runtime created.\n");

    runtime.register_generated_unit(
        0u,
        0x08804000u,
        16384u,
        &test_unit_0,
        nullptr);

    runtime.register_generated_unit(
        8u,
        0x08824000u,
        16384u,
        &test_unit_8,
        nullptr);

    debugPrint("Synthetic units registered.\n");
    debugPrint("Starting Runtime::run...\n");

    runtime.run(0x08804000u, 2u);

    debugPrint("Runtime::run returned.\n");
    debugPrint("unit0=%u unwind=%u unit8=%u\n",
               g_step0,
               g_chain_unwound,
               g_step1);

    if (g_step0 == 1 &&
        g_chain_unwound == 1 &&
        g_step1 == 1) {
        debugPrint("SUCCESS: automatic bank switch works!\n");
    } else {
        debugPrint("FAILED: automatic bank switch test.\n");
    }

    while (true)
        Sleep(1000);

    return 0;
}
