#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include "vcs_profile.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace psprecomp {
void register_generated_functions(Runtime &runtime);
}

static std::vector<std::uint8_t> read_file(const char *path)
{
    FILE *f = std::fopen(path, "rb");

    if (f == nullptr)
        throw psprecomp::Error(std::string("Cannot open ") + path);

    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        throw psprecomp::Error("fseek failed");
    }

    const long size = std::ftell(f);

    if (size <= 0) {
        std::fclose(f);
        throw psprecomp::Error("VCS ELF is empty");
    }

    std::rewind(f);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));

    const std::size_t read =
        std::fread(bytes.data(), 1, bytes.size(), f);

    std::fclose(f);

    if (read != bytes.size())
        throw psprecomp::Error("Failed reading VCS ELF");

    return bytes;
}

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    debugPrint("VCS Xbox bootstrap\n");

    try {
        debugPrint("Loading VCS ELF...\n");

        auto bytes = read_file("D:\\vcs.elf");

        psprecomp::Elf32Image elf =
            psprecomp::Elf32Image::from_bytes(
                std::move(bytes),
                "D:\\vcs.elf");

        debugPrint("ELF parsed successfully.\n");

        psprecomp::Runtime runtime(32u * 1024u * 1024u);

        debugPrint("PSP Runtime created.\n");

        /*
         * D: is the directory containing default.xbe.
         * PSP_GAME and the other extracted game files live below it.
         */
        runtime.set_game_root("D:\\");

        debugPrint("Loading ELF into PSP RAM...\n");

        const auto relocations =
            elf.load_and_relocate(
                runtime.memory(),
                psprecomp::kDefaultPspUserLoadBase);

        (void)relocations;

        std::uint64_t image_end = 0u;

        for (std::size_t i = 0; i < elf.segments().size(); ++i) {
            const auto &segment = elf.segments()[i];

            if (segment.type != 1u)
                continue;

            const std::uint64_t start =
                elf.segment_runtime_address(
                    i,
                    psprecomp::kDefaultPspUserLoadBase);

            image_end =
                std::max(
                    image_end,
                    start + segment.memory_size);
        }

        if (image_end == 0u ||
            image_end > 0x0A000000ull) {
            throw psprecomp::Error(
                "Invalid PSP ELF load image extent");
        }

        const std::uint32_t user_arena_start =
            static_cast<std::uint32_t>(
                (image_end + 0xFFu) & ~0xFFull);

        debugPrint("Registering generated VCS code...\n");

        psprecomp::register_generated_functions(runtime);

        debugPrint("Installing VCS profile...\n");

        vcs::install_profile(runtime, user_arena_start);

        debugPrint("VCS profile installed.\n");

        const auto module =
            elf.find_module_info(
                runtime.memory(),
                psprecomp::kDefaultPspUserLoadBase);

        if (!module)
            throw psprecomp::Error(
                "PSP module info not found");

        runtime.cpu().set_gpr(28, module->gp);

        /*
         * install_profile() establishes the initial module/thread stack.
         */
        runtime.cpu().set_gpr(31, 0u);
        runtime.cpu().set_gpr(4, 0u);
        runtime.cpu().set_gpr(5, 0u);

        const std::uint32_t entry =
            elf.runtime_entry();

        debugPrint("Starting VCS at 0x%08X\n",
                   (unsigned int)entry);

        runtime.run(entry, 4'000'000'000ull);

        debugPrint("VCS runtime stopped.\n");
    }
    catch (const std::exception &e) {
        debugPrint("BOOT ERROR: %s\n", e.what());
    }
    catch (...) {
        debugPrint("BOOT ERROR: unknown exception\n");
    }

    while (true)
        Sleep(1000);

    return 0;
}
