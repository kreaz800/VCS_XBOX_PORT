#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>
#include <nxdk/xbe.h>
#include <xboxkrnl/xboxkrnl.h>

#include "psprecomp/runtime.hpp"

namespace psprecomp {
void register_generated_functions(Runtime &runtime);
}

static bool cycle_bank(PXBE_SECTION_HEADER section, int pass)
{
    debugPrint("---- .b16 PASS %d ----\n", pass);

    NTSTATUS status = XeLoadSection(section);

    debugPrint("XeLoadSection: 0x%08X\n",
               (unsigned int)status);

    if (status < 0)
        return false;

    debugPrint(".b16 resident.\n");

    Sleep(1000);

    status = XeUnloadSection(section);

    debugPrint("XeUnloadSection: 0x%08X\n",
               (unsigned int)status);

    return status >= 0;
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    debugPrint("Real VCS bank stress test\n");

    debugPrint("Creating 32 MB PSP Runtime...\n");

    psprecomp::Runtime runtime(32u * 1024u * 1024u);

    debugPrint("Runtime created.\n");

    debugPrint("Registering all VCS generated units...\n");

    psprecomp::register_generated_functions(runtime);

    debugPrint("Registration returned.\n");

    PXBE_SECTION_HEADER bank =
        nxXbeGetSectionByName(".b16");

    if (bank == nullptr) {
        debugPrint("ERROR: .b16 not found.\n");

        while (true)
            Sleep(1000);
    }

    debugPrint(".b16 found.\n");

    bool pass1 = cycle_bank(bank, 1);
    bool pass2 = false;

    if (pass1) {
        Sleep(1000);
        pass2 = cycle_bank(bank, 2);
    }

    if (pass1 && pass2)
        debugPrint("SUCCESS: largest real VCS bank reload works!\n");
    else
        debugPrint("FAILED: real VCS bank reload test.\n");

    while (true)
        Sleep(1000);

    return 0;
}
