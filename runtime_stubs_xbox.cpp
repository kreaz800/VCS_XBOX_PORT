#include <cstdint>

namespace psprecomp {

std::int32_t runtime_thread_uid() noexcept {
    return -1;
}

const char* runtime_thread_name() noexcept {
    return "xbox";
}

std::uint32_t runtime_dispatch_pc() noexcept {
    return 0;
}

}
