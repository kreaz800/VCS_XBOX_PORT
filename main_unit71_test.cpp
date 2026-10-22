#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <nxdk/xbe.h>

static bool cycle(PXBE_SECTION_HEADER section, int pass)
{
    debugPrint("---- VCS UNIT 71 PASS %d ----\n", pass);

    NTSTATUS status = XeLoadSection(section);
    debugPrint("XeLoadSection: 0x%08X\n", (unsigned int)status);

    if (status < 0)
        return false;

    debugPrint("Real VCS .v071 is resident.\n");

    status = XeUnloadSection(section);
    debugPrint("XeUnloadSection: 0x%08X\n", (unsigned int)status);

    return status >= 0;
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    debugPrint("VCS generated-unit overlay test\n");

    PXBE_SECTION_HEADER section = nxXbeGetSectionByName(".v071");

    if (!section) {
        debugPrint("ERROR: .v071 not found.\n");
        while (true)
            Sleep(1000);
    }

    debugPrint(".v071 found.\n");

    bool a = cycle(section, 1);
    bool b = false;

    if (a) {
        Sleep(500);
        b = cycle(section, 2);
    }

    if (a && b)
        debugPrint("SUCCESS: real VCS unit reload works!\n");
    else
        debugPrint("FAILED: VCS unit overlay test.\n");

    while (true)
        Sleep(1000);
}
