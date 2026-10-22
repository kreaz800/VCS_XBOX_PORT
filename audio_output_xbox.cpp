#include "audio_output.hpp"
#include "vcs_config.hpp"

#include <hal/audio.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace vcs {
namespace {

constexpr std::uint32_t kSampleRate = 48000u;
constexpr std::uint32_t kOutputChannels = 2u;

constexpr std::size_t kBlockFrames = 512u;
constexpr std::size_t kBlockSamples = kBlockFrames * kOutputChannels;
constexpr std::size_t kBlockBytes =
    kBlockSamples * sizeof(std::int16_t);

/* About two seconds, matching the desktop mixer's reserve. */
constexpr std::size_t kRingFrames = kSampleRate * 2u;

constexpr std::size_t kGuestChannels = 9u;
constexpr std::uint64_t kChannelDiscontinuityFrames = 64u;

/* Software queue between normal guest execution and the XAudio DPC. */
constexpr std::size_t kReadyBlocks = 24u;

/* Six 512-frame AC97 buffers ~= 64 ms at 48 kHz. */
constexpr std::size_t kHardwareBuffers = 6u;

constexpr std::uint64_t kMixSafetyFrames = 1024u;

constexpr std::uint32_t kMaxXboxRamAddress = 0x03FFAFFFu;


/*
 * Xbox version of the existing streaming linear resampler.
 *
 * The desktop implementation targets 44100 Hz. Xbox AC97 playback is fixed
 * at 48000 Hz, so this otherwise-identical implementation targets 48000.
 */
class XboxStreamingLinearResampler {
public:
    void reset(std::uint32_t source_rate = kSampleRate,
               bool stereo = true) noexcept {
        source_rate_ = source_rate == 0u ? kSampleRate : source_rate;
        stereo_ = stereo;
        have_previous_ = false;
        previous_left_ = 0;
        previous_right_ = 0;
        source_index_ = 0u;
        next_output_position_ = 0u;
    }

    template <typename Emit>
    std::uint64_t process(std::span<const std::int16_t> pcm,
                          std::uint32_t frames,
                          bool stereo,
                          std::uint32_t source_rate,
                          Emit &&emit) {
        if (frames == 0u)
            return 0u;

        if (source_rate == 0u)
            source_rate = kSampleRate;

        const std::size_t channels = stereo ? 2u : 1u;

        if (pcm.size() <
            static_cast<std::size_t>(frames) * channels)
            return 0u;

        if (source_rate_ != source_rate || stereo_ != stereo)
            reset(source_rate, stereo);

        std::uint64_t emitted = 0u;

        for (std::uint32_t frame = 0u; frame < frames; ++frame) {
            const std::size_t offset =
                static_cast<std::size_t>(frame) * channels;

            const std::int32_t current_left = pcm[offset];
            const std::int32_t current_right =
                stereo ? pcm[offset + 1u] : current_left;

            if (!have_previous_) {
                previous_left_ = current_left;
                previous_right_ = current_right;
                have_previous_ = true;
                source_index_ = 0u;
                next_output_position_ = 0u;

                emit(static_cast<std::int16_t>(previous_left_),
                     static_cast<std::int16_t>(previous_right_));

                ++emitted;
                next_output_position_ += source_rate_;
                continue;
            }

            const std::uint64_t previous_position =
                source_index_ * kSampleRate;

            const std::uint64_t current_position =
                (source_index_ + 1u) * kSampleRate;

            while (next_output_position_ <= current_position) {
                const std::uint64_t numerator =
                    next_output_position_ - previous_position;

                const std::int64_t left =
                    previous_left_ +
                    ((static_cast<std::int64_t>(current_left) -
                      previous_left_) *
                     static_cast<std::int64_t>(numerator)) /
                        static_cast<std::int64_t>(kSampleRate);

                const std::int64_t right =
                    previous_right_ +
                    ((static_cast<std::int64_t>(current_right) -
                      previous_right_) *
                     static_cast<std::int64_t>(numerator)) /
                        static_cast<std::int64_t>(kSampleRate);

                emit(
                    static_cast<std::int16_t>(
                        std::clamp<std::int64_t>(
                            left, -32768, 32767)),
                    static_cast<std::int16_t>(
                        std::clamp<std::int64_t>(
                            right, -32768, 32767)));

                ++emitted;
                next_output_position_ += source_rate_;
            }

            previous_left_ = current_left;
            previous_right_ = current_right;
            ++source_index_;
        }

        return emitted;
    }

private:
    std::uint32_t source_rate_{kSampleRate};
    bool stereo_{true};
    bool have_previous_{};
    std::int32_t previous_left_{};
    std::int32_t previous_right_{};
    std::uint64_t source_index_{};
    std::uint64_t next_output_position_{};
};


struct ChannelStream {
    XboxStreamingLinearResampler resampler;

    std::uint64_t cursor{};
    std::uint64_t last_guest_time_us{};

    std::uint32_t source_rate{kSampleRate};

    bool stereo{true};
    bool active{};
};


struct AudioState {
    std::vector<std::int32_t> ring;

    std::array<ChannelStream, kGuestChannels> channels{};

    std::array<
        std::array<std::int16_t, kBlockSamples>,
        kReadyBlocks> ready{};

    std::uint32_t ready_read{};
    std::uint32_t ready_write{};

    std::array<unsigned char *, kHardwareBuffers>
        hardware_buffers{};

    std::size_t callback_hardware_buffer{};

    std::uint32_t callback_active{};
    std::uint32_t need_rebuffer{};

    std::uint64_t output_frame{};
    std::uint64_t guest_anchor_us{};

    bool timeline_anchored{};
    bool opened{};
    bool playback_started{};
    bool failed{};
    bool shutdown{};
};


AudioState &audio_state() {
    static AudioState state;
    return state;
}


std::uint32_t atomic_load_u32(
    const std::uint32_t *value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}


void atomic_store_u32(
    std::uint32_t *value,
    std::uint32_t new_value) {
    __atomic_store_n(value, new_value, __ATOMIC_RELEASE);
}


std::size_t ready_block_count(const AudioState &state) {
    const std::uint32_t read =
        atomic_load_u32(&state.ready_read);

    const std::uint32_t write =
        atomic_load_u32(&state.ready_write);

    return static_cast<std::size_t>(write - read);
}


bool pop_ready_block(AudioState &state,
                     unsigned char *destination) {
    const std::uint32_t read =
        atomic_load_u32(&state.ready_read);

    const std::uint32_t write =
        atomic_load_u32(&state.ready_write);

    if (read == write)
        return false;

    const auto &source =
        state.ready[read % kReadyBlocks];

    std::memcpy(
        destination,
        source.data(),
        kBlockBytes);

    atomic_store_u32(
        &state.ready_read,
        read + 1u);

    return true;
}


/*
 * Called by nxdk from a DPC.
 *
 * Do not allocate, mix, lock a C++ mutex, access files, or use floating point
 * here. It only copies one already-prepared block and queues it to AC97.
 */
void xaudio_callback(void *, void *data) {
    auto *state =
        static_cast<AudioState *>(data);

    if (state == nullptr ||
        atomic_load_u32(&state->callback_active) == 0u)
        return;

    unsigned char *buffer =
        state->hardware_buffers[
            state->callback_hardware_buffer];

    if (!pop_ready_block(*state, buffer)) {
        std::memset(buffer, 0, kBlockBytes);

        atomic_store_u32(
            &state->need_rebuffer,
            1u);
    }

    XAudioProvideSamples(
        buffer,
        static_cast<unsigned short>(kBlockBytes),
        0);

    state->callback_hardware_buffer =
        (state->callback_hardware_buffer + 1u) %
        kHardwareBuffers;
}


bool ensure_device(AudioState &state) {
    if (state.opened)
        return true;

    if (state.failed || state.shutdown)
        return false;

    for (std::size_t index = 0;
         index < kHardwareBuffers;
         ++index) {

        state.hardware_buffers[index] =
            static_cast<unsigned char *>(
                MmAllocateContiguousMemoryEx(
                    kBlockBytes,
                    0,
                    kMaxXboxRamAddress,
                    0,
                    PAGE_READWRITE |
                        PAGE_WRITECOMBINE));

        if (state.hardware_buffers[index] == nullptr) {
            state.failed = true;
            return false;
        }

        std::memset(
            state.hardware_buffers[index],
            0,
            kBlockBytes);
    }

    state.ring.assign(
        kRingFrames * kOutputChannels,
        0);

    atomic_store_u32(&state.ready_read, 0u);
    atomic_store_u32(&state.ready_write, 0u);
    atomic_store_u32(&state.callback_active, 0u);
    atomic_store_u32(&state.need_rebuffer, 0u);

    state.callback_hardware_buffer = 0u;
    state.output_frame = 0u;
    state.timeline_anchored = false;
    state.playback_started = false;

    XAudioInit(
        16,
        2,
        &xaudio_callback,
        &state);

    state.opened = true;
    return true;
}


void reset_channel(AudioState &state,
                   std::uint32_t channel) {
    if (channel >= state.channels.size())
        return;

    state.channels[channel] = ChannelStream{};
}


void reset_after_underrun(AudioState &state,
                          std::uint64_t guest_time_us) {
    if (atomic_load_u32(&state.need_rebuffer) == 0u)
        return;

    /*
     * Do the expensive recovery here on the normal execution thread,
     * never inside the XAudio DPC.
     */
    atomic_store_u32(&state.callback_active, 0u);
    XAudioPause();

    atomic_store_u32(&state.ready_read, 0u);
    atomic_store_u32(&state.ready_write, 0u);
    atomic_store_u32(&state.need_rebuffer, 0u);

    std::fill(
        state.ring.begin(),
        state.ring.end(),
        0);

    for (std::uint32_t channel = 0u;
         channel < kGuestChannels;
         ++channel)
        reset_channel(state, channel);

    state.output_frame = 0u;
    state.guest_anchor_us = guest_time_us;
    state.timeline_anchored = true;
    state.playback_started = false;
    state.callback_hardware_buffer = 0u;
}


std::uint64_t guest_frame_for(
    const AudioState &state,
    std::uint64_t guest_time_us) {

    if (!state.timeline_anchored ||
        guest_time_us <= state.guest_anchor_us)
        return 0u;

    const std::uint64_t delta =
        guest_time_us - state.guest_anchor_us;

    return
        (delta * kSampleRate + 500000u) /
        1000000u;
}


void start_playback_if_ready(AudioState &state) {
    if (state.playback_started)
        return;

    if (ready_block_count(state) < kHardwareBuffers)
        return;

    /*
     * XAudio is still paused here, so no callback can race us while
     * the initial AC97 descriptor set is queued.
     */
    for (std::size_t index = 0;
         index < kHardwareBuffers;
         ++index) {

        unsigned char *buffer =
            state.hardware_buffers[index];

        if (!pop_ready_block(state, buffer))
            std::memset(buffer, 0, kBlockBytes);

        XAudioProvideSamples(
            buffer,
            static_cast<unsigned short>(kBlockBytes),
            0);
    }

    state.callback_hardware_buffer = 0u;

    atomic_store_u32(
        &state.callback_active,
        1u);

    state.playback_started = true;

    XAudioPlay();
}


bool queue_one_block(AudioState &state) {
    const std::uint32_t read =
        atomic_load_u32(&state.ready_read);

    const std::uint32_t write =
        atomic_load_u32(&state.ready_write);

    if (write - read >= kReadyBlocks)
        return false;

    auto &block =
        state.ready[write % kReadyBlocks];

    for (std::size_t frame = 0u;
         frame < kBlockFrames;
         ++frame) {

        const std::size_t slot =
            static_cast<std::size_t>(
                (state.output_frame + frame) %
                kRingFrames) *
            kOutputChannels;

        for (std::size_t channel = 0u;
             channel < kOutputChannels;
             ++channel) {

            block[
                frame * kOutputChannels +
                channel] =
                static_cast<std::int16_t>(
                    std::clamp(
                        state.ring[slot + channel],
                        -32768,
                        32767));

            state.ring[slot + channel] = 0;
        }
    }

    atomic_store_u32(
        &state.ready_write,
        write + 1u);

    state.output_frame += kBlockFrames;

    start_playback_if_ready(state);

    return true;
}


void advance_audio(AudioState &state,
                   std::uint64_t guest_time_us) {
    if (!state.timeline_anchored ||
        !state.opened)
        return;

    reset_after_underrun(
        state,
        guest_time_us);

    const std::uint64_t guest_frame =
        guest_frame_for(state, guest_time_us);

    const std::size_t outstanding =
        ready_block_count(state);

    const std::uint64_t safety_frames =
        state.playback_started &&
                outstanding <= 2u
            ? 0u
            : kMixSafetyFrames;

    const std::uint64_t sealed_frame =
        guest_frame > safety_frames
            ? guest_frame - safety_frames
            : 0u;

    while (sealed_frame >=
           state.output_frame + kBlockFrames) {
        if (!queue_one_block(state))
            break;
    }
}


} // namespace


bool audio_output_enabled() {
    const VcsConfiguration &configuration =
        vcs_configuration();

    return
        !configuration.initialized ||
        configuration.audio.enabled;
}


void audio_output_submit(
    std::span<const std::int16_t> pcm,
    std::uint32_t frames,
    bool stereo,
    std::uint32_t left,
    std::uint32_t right,
    std::uint32_t source_rate,
    std::uint32_t channel,
    std::uint64_t guest_time_us) {

    if (!audio_output_enabled() ||
        frames == 0u ||
        channel >= kGuestChannels)
        return;

    if (source_rate == 0u)
        source_rate = kSampleRate;

    const std::size_t required =
        static_cast<std::size_t>(frames) *
        (stereo ? 2u : 1u);

    if (pcm.size() < required)
        return;

    AudioState &state = audio_state();

    if (!ensure_device(state))
        return;

    reset_after_underrun(
        state,
        guest_time_us);

    if (!state.timeline_anchored) {
        state.guest_anchor_us =
            guest_time_us;

        state.timeline_anchored = true;
        state.output_frame = 0u;
    }

    /*
     * Seal old timeline regions before mixing this submission.
     */
    advance_audio(
        state,
        guest_time_us);

    ChannelStream &stream =
        state.channels[channel];

    const std::uint64_t scheduled =
        guest_frame_for(
            state,
            guest_time_us);

    const auto distance =
        [](std::uint64_t a,
           std::uint64_t b) {
            return a > b ? a - b : b - a;
        };

    const bool format_changed =
        stream.active &&
        (stream.source_rate != source_rate ||
         stream.stereo != stereo);

    const bool discontinuity =
        stream.active &&
        distance(
            stream.cursor,
            scheduled) >
            kChannelDiscontinuityFrames;

    if (!stream.active ||
        format_changed ||
        discontinuity) {

        stream = ChannelStream{};
        stream.active = true;
        stream.source_rate = source_rate;
        stream.stereo = stereo;

        stream.resampler.reset(
            source_rate,
            stereo);

        stream.cursor =
            std::max(
                scheduled,
                state.output_frame);
    }

    if (stream.cursor < state.output_frame) {
        stream.cursor = state.output_frame;

        stream.resampler.reset(
            source_rate,
            stereo);
    }

    const std::uint32_t master =
        vcs_configuration().audio.volume;

    const std::int64_t left_gain =
        (static_cast<std::int64_t>(left) *
         master) /
        100;

    const std::int64_t right_gain =
        (static_cast<std::int64_t>(right) *
         master) /
        100;

    const std::uint64_t ring_limit =
        state.output_frame +
        kRingFrames -
        kBlockFrames;

    stream.resampler.process(
        pcm,
        frames,
        stereo,
        source_rate,
        [&](std::int16_t source_left,
            std::int16_t source_right) {

            if (stream.cursor >= ring_limit) {
                ++stream.cursor;
                return;
            }

            const std::size_t slot =
                static_cast<std::size_t>(
                    stream.cursor %
                    kRingFrames) *
                kOutputChannels;

            const std::int64_t mixed_left =
                (static_cast<std::int64_t>(
                     source_left) *
                 left_gain) >>
                15;

            const std::int64_t mixed_right =
                (static_cast<std::int64_t>(
                     source_right) *
                 right_gain) >>
                15;

            state.ring[slot] +=
                static_cast<std::int32_t>(
                    std::clamp<std::int64_t>(
                        mixed_left,
                        std::numeric_limits<
                            std::int32_t>::min(),
                        std::numeric_limits<
                            std::int32_t>::max()));

            state.ring[slot + 1u] +=
                static_cast<std::int32_t>(
                    std::clamp<std::int64_t>(
                        mixed_right,
                        std::numeric_limits<
                            std::int32_t>::min(),
                        std::numeric_limits<
                            std::int32_t>::max()));

            ++stream.cursor;
        });

    stream.last_guest_time_us =
        guest_time_us;

    advance_audio(
        state,
        guest_time_us);
}


void audio_output_advance(
    std::uint64_t guest_time_us) {

    if (!audio_output_enabled())
        return;

    AudioState &state = audio_state();

    if (!state.opened)
        return;

    advance_audio(
        state,
        guest_time_us);
}


void audio_output_reset_channel(
    std::uint32_t channel) {

    AudioState &state = audio_state();

    reset_channel(
        state,
        channel);
}


void audio_output_shutdown() {
    AudioState &state = audio_state();

    if (!state.opened)
        return;

    /*
     * nxdk XAudio currently has no disconnect/shutdown API.
     * Pause AC97 and leave its tiny contiguous buffers alive until
     * process termination instead of freeing memory still referenced
     * by descriptors.
     */
    atomic_store_u32(
        &state.callback_active,
        0u);

    XAudioPause();

    state.playback_started = false;
    state.opened = false;
    state.shutdown = true;
}


} // namespace vcs
