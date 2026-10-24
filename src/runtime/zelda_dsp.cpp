// The Wind Waker's audio DSP microcode ("DAC", the Zelda microcode family that JAudio uploads from
// main.dol's jdsp: Dolphin's CRC 0x86840740), implemented natively. The game's JAudio sends it
// command batches through the mailbox (src/runtime/dsp.cpp hands every mail here once the
// microcode runs): 01 sets the voice parameter blocks and the mixing tables up, 02 renders audio
// frames - each voice's samples from ARAM (AFC ADPCM, PCM8/16) or synthesized, resampled,
// filtered and mixed into the output buffers in main memory, which the AI DMA then plays
// (src/runtime/audio.cpp) - and the others are acknowledged.
//
// Ported from Dolphin's DSP HLE (Source/Core/Core/HW/DSPHLE/UCodes/Zelda.cpp, Zelda.h):
//   Copyright 2008 Dolphin Emulator Project
//   SPDX-License-Identifier: GPL-2.0-or-later
// reduced to this game's microcode (no light protocol, no Wii ARAM-less variants, no GBA crypto)
// and moved onto this runtime's guest memory, ARAM and mailbox.
#include "runtime/runtime.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace rt {

void dspPushMail(u32 mail, bool interrupt);  // src/runtime/dsp.cpp
u8* aramPtr(u32 a);                           // src/runtime/hle_hw.cpp

namespace {

enum : u32 {
    DSP_INIT = 0xDCD10000,
    DSP_SYNC = 0xDCD10004,
    DSP_FRAME_END = 0xDCD10005,
    TASK_MAIL_MASK = 0xFFFF0000,
    TASK_MAIL_TO_DSP = 0xCDD10000,
    MAIL_RESUME = 0xCDD10000,
    MAIL_NEW_UCODE = 0xCDD10001,
    MAIL_RESET = 0xCDD10002,
    MAIL_CONTINUE = 0xCDD10003,
};

// ---- guest memory, as the DSP's DMA sees it (16-bit words, big-endian) ----
void readWords(u16* dst, u32 addr, size_t bytes) {
    for (size_t i = 0; i < bytes / 2; i++) {
        u16 v;
        std::memcpy(&v, hostp(addr + u32(i * 2)), 2);
        dst[i] = bswap16(v);
    }
}
void writeWords(u32 addr, const u16* src, size_t bytes) {
    for (size_t i = 0; i < bytes / 2; i++) {
        const u16 v = bswap16(src[i]);
        std::memcpy(hostp(addr + u32(i * 2)), &v, 2);
    }
}
template <size_t N>
void readArray(std::array<s16, N>& a, u32 addr) {
    readWords(reinterpret_cast<u16*>(a.data()), addr, N * 2);
}

using MixingBuffer = std::array<s16, 0x50>;

// Volume in 1.15 or 4.12 fixed point.
template <size_t N, size_t B>
void applyVolumeInPlace(std::array<s16, N>* buf, u16 vol) {
    for (size_t i = 0; i < N; ++i) {
        s32 tmp = s32(u32((*buf)[i]) * u32(vol));
        tmp >>= 16 - B;
        (*buf)[i] = s16(std::clamp(tmp, -0x8000, 0x7FFF));
    }
}
// Mixes src into dst with a volume ramping by `step` per sample (1.31); returns the end volume.
template <size_t N>
s32 addBuffersWithVolumeRamp(std::array<s16, N>* dst, const std::array<s16, N>& src, s32 vol, s32 step) {
    if (!vol && !step) return vol;
    for (size_t i = 0; i < N; ++i) {
        (*dst)[i] += s16(((vol >> 16) * src[i]) >> 16);
        vol += step;
    }
    return vol;
}
void addBuffersWithVolume(s16* dst, const s16* src, size_t count, u16 vol) {
    while (count--) {
        const s32 v = (s32(*src++) * s32(vol)) >> 15;
        *dst++ += s16(std::clamp(v, -0x8000, 0x7FFF));
    }
}

#define DEFINE_32BIT_ACCESSOR(field, name) \
    u32 get##name() const { return u32(field##_h) << 16 | field##_l; } \
    void set##name(u32 v) { field##_h = u16(v >> 16), field##_l = u16(v); }

// A voice parameter block: 0xC0 words in main memory, the first 0x80 written back after each frame.
#pragma pack(push, 1)
struct VPB {
    u16 enabled;           // 00: zero: skip the voice
    u16 done;              // 01: nonzero: skip the voice
    u16 resampling_ratio;  // 02: 4.12 raw samples per output sample
    u16 unk_03;
    u16 reset_vpb;            // 04
    u16 end_reached;          // 05
    u16 use_constant_sample;  // 06: inputs are constant_sample (fading a voice out)
    u16 samples_to_keep_count;
    struct Channel {
        u16 id;  // the DSP DRAM address of a mixing buffer
        s16 target_volume;
        s16 current_volume;
        u16 unk;
    } channels[6];  // 08-1F
    u16 unk_20_28[8];
    u16 dolby_voice_position;  // 28: 0XXXXXXX0YYYYYYY
    u8 dolbyX() const { return (dolby_voice_position >> 8) & 0x7F; }
    u8 dolbyY() const { return dolby_voice_position & 0x7F; }
    s16 dolby_reverb_factor;   // 29
    s16 dolby_volume_current;  // 2A
    s16 dolby_volume_target;   // 2B
    u16 use_dolby_volume;      // 2C
    u16 unk_2D, unk_2E, unk_2F;
    u16 current_pos_frac;  // 30: 0.12
    u16 unk_31;
    u16 afc_remaining_decoded_samples;  // 32
    s16 constant_sample;                // 33
    u16 current_position_h, current_position_l;
    DEFINE_32BIT_ACCESSOR(current_position, CurrentPosition)
    u16 samples_before_loop;  // 36
    u16 unk_37;
    u16 current_aram_addr_h, current_aram_addr_l;
    DEFINE_32BIT_ACCESSOR(current_aram_addr, CurrentARAMAddr)
    u16 remaining_length_h, remaining_length_l;
    DEFINE_32BIT_ACCESSOR(remaining_length, RemainingLength)
    s16 resample_buffer[4];  // 3C
    s16 variable_fir_history[20];
    s16 biquad_xn1, biquad_xn2, biquad_yn1, biquad_yn2;
    s16 afc_remaining_samples[0x10];  // 58
    s16* afcYn2() { return &afc_remaining_samples[0xE]; }
    s16* afcYn1() { return &afc_remaining_samples[0xF]; }
    s16 low_pass_yn1, low_pass_xn1;  // 68
    u16 unk_6A_80[0x80 - 0x6A];
    enum SamplesSourceType {
        SRC_SQUARE_WAVE = 0,
        SRC_SAW_WAVE = 1,
        SRC_SQUARE_WAVE_25PCT = 3,
        SRC_CONST_PATTERN_0 = 7,
        SRC_CONST_PATTERN_0_VARIABLE_STEP = 10,
        SRC_CONST_PATTERN_1 = 4,
        SRC_CONST_PATTERN_2 = 11,
        SRC_CONST_PATTERN_3 = 12,
        SRC_AFC_LQ_FROM_ARAM = 5,  // 16 samples in 5 bytes
        SRC_PCM8_FROM_ARAM = 8,
        SRC_AFC_HQ_FROM_ARAM = 9,  // 16 samples in 9 bytes
        SRC_PCM16_FROM_ARAM = 16,
        SRC_PCM16_FROM_MRAM = 33,
    };
    u16 samples_source_type;  // 80
    u16 is_looping;
    s16 loop_yn1, loop_yn2;
    u16 filter_flags;  // 84: bits 0-4 variable FIR size, bit 5 biquad enable
    u16 end_requested;
    u16 unk_86, unk_87;
    u16 loop_address_h, loop_address_l;
    DEFINE_32BIT_ACCESSOR(loop_address, LoopAddress)
    u16 loop_start_position_h, loop_start_position_l;
    DEFINE_32BIT_ACCESSOR(loop_start_position, LoopStartPosition)
    u16 base_address_h, base_address_l;
    DEFINE_32BIT_ACCESSOR(base_address, BaseAddress)
    u16 unk_8E, unk_8F;
    u16 variable_fir_coeffs[20];
    s16 biquad_bn1, biquad_bn2, biquad_an1, biquad_an2;
    u16 low_pass_coeff;  // A8
    u16 padding[0xC0];
};
struct ReverbPB {
    u16 enabled;
    u16 circular_buffer_size;  // in 0x50-sample blocks
    u16 circular_buffer_base_h, circular_buffer_base_l;
    DEFINE_32BIT_ACCESSOR(circular_buffer_base, CircularBufferBase)
    struct Destination {
        u16 buffer_id;
        u16 volume;  // 1.15
    } dest[2];
    s16 filter_coeffs[8];
};
#pragma pack(pop)
static_assert(offsetof(VPB, samples_source_type) == 0x80 * 2);
static_assert(offsetof(VPB, low_pass_coeff) == 0xA8 * 2);
static_assert(offsetof(VPB, afc_remaining_samples) == 0x58 * 2);

// ---- the renderer: one frame is 0x50 samples per buffer ----
class Renderer {
public:
    void prepareFrame();
    void addVoice(u16 voice_id);
    void finalizeFrame();

    std::array<s16, 0x80> sine_table{};
    std::array<s16, 0x100> const_patterns{};
    std::array<s16, 0x100> resampling_coeffs{};
    std::array<s16, 0x20> afc_coeffs{};
    u32 vpb_base_addr = 0, reverb_pb_base_addr = 0;
    u16 output_volume = 0;
    u32 output_lbuf_addr = 0, output_rbuf_addr = 0;

private:
    bool m_prepared = false;
    MixingBuffer m_front_left{}, m_front_right{}, m_back_left{}, m_back_right{};
    MixingBuffer m_front_left_reverb{}, m_front_right_reverb{}, m_back_left_reverb{}, m_back_right_reverb{};
    MixingBuffer m_unk0_reverb{}, m_unk1_reverb{}, m_unk0{}, m_unk1{}, m_unk2{};
    std::array<u16, 4> m_reverb_pb_frames_count{};
    std::array<s16, 8> m_unk0_reverb_last8{}, m_unk1_reverb_last8{}, m_front_left_reverb_last8{}, m_front_right_reverb_last8{};

    MixingBuffer* bufferForId(u16 id);
public:
    void readVpb(u16 voice_id, VPB* vpb) { fetchVpb(voice_id, vpb); }

private:
    void fetchVpb(u16 voice_id, VPB* vpb) { readWords(reinterpret_cast<u16*>(vpb), vpb_base_addr + voice_id * 0xC0 * 2, 0xC0 * 2); }
    void storeVpb(u16 voice_id, VPB* vpb) { writeWords(vpb_base_addr + voice_id * 0xC0 * 2, reinterpret_cast<u16*>(vpb), 0x80 * 2); }
    void loadInputSamples(MixingBuffer* buffer, VPB* vpb);
    static u16 neededRawSamplesCount(const VPB& vpb) { return u16((vpb.current_pos_frac + 0x50 * vpb.resampling_ratio) >> 12); }
    void resample(VPB* vpb, const s16* src, MixingBuffer* dst);
    template <typename T>
    void downloadPcmFromAram(s16* dst, VPB* vpb, u16 count);
    void downloadAfcFromAram(s16* dst, VPB* vpb, u16 count);
    void decodeAfc(VPB* vpb, s16* dst, size_t blocks);
    void downloadRawFromMram(s16* dst, VPB* vpb, u16 count);
    static void lowPass(MixingBuffer* buf, VPB* vpb);
    static void biquad(MixingBuffer* buf, VPB* vpb);
    void applyReverb(bool post);
};

MixingBuffer* Renderer::bufferForId(u16 id) {
    switch (id) {
    case 0x0D00: return &m_front_left;
    case 0x0D60: return &m_front_right;
    case 0x0F40: return &m_back_left;
    case 0x0CA0: return &m_back_right;
    case 0x0E80: return &m_front_left_reverb;
    case 0x0EE0: return &m_front_right_reverb;
    case 0x0C00: return &m_back_left_reverb;
    case 0x0C50: return &m_back_right_reverb;
    case 0x0DC0: return &m_unk0_reverb;
    case 0x0E20: return &m_unk1_reverb;
    case 0x09A0: return &m_unk0;
    case 0x0FA0: return &m_unk1;
    case 0x0B00: return &m_unk2;
    default: return nullptr;
    }
}

void Renderer::prepareFrame() {
    if (m_prepared) return;
    m_front_left.fill(0);
    m_front_right.fill(0);
    applyVolumeInPlace<0x50, 1>(&m_back_left, 0x6784);
    applyVolumeInPlace<0x50, 1>(&m_back_right, 0x6784);
    // reverb data from the previous frame
    applyReverb(false);
    addBuffersWithVolume(m_front_left_reverb.data(), m_back_left_reverb.data(), 0x50, 0x7FFF);
    addBuffersWithVolume(m_front_right_reverb.data(), m_back_left_reverb.data(), 0x50, 0xB820);
    addBuffersWithVolume(m_front_left_reverb.data(), m_back_right_reverb.data() + 0x28, 0x28, 0xB820);
    addBuffersWithVolume(m_front_right_reverb.data(), m_back_right_reverb.data() + 0x28, 0x28, 0x7FFF);
    m_back_left_reverb.fill(0);
    m_back_right_reverb.fill(0);
    // patterns 2 and 3 change every frame (0 and 1 are constant)
    s16* p2 = const_patterns.data() + 2 * 0x40;
    s32 yn2 = p2[0x40 - 2], yn1 = p2[0x40 - 1], v;
    for (int i = 0; i < 0x40; i += 2) {
        v = yn2 * yn1 - (p2[i] << 16);
        yn2 = yn1, yn1 = p2[i], p2[i] = s16(v >> 16);
        v = 2 * (yn2 * yn1 + (p2[i + 1] << 16));
        yn2 = yn1, yn1 = p2[i + 1], p2[i + 1] = s16(v >> 16);
    }
    s16* p3 = const_patterns.data() + 3 * 0x40;
    yn2 = p3[0x40 - 2], yn1 = p3[0x40 - 1];
    const s16 acc = s16(yn1);
    s16 step = s16(p3[0] + ((yn1 * yn2 + ((yn2 << 16) + yn1)) >> 16));
    step = s16((step & 0x1FF) | 0x2000);
    for (s32 i = 0; i < 0x40; ++i) p3[i] = s16(acc + (i + 1) * step);
    m_prepared = true;
}

void Renderer::applyReverb(bool post) {
    if (!reverb_pb_base_addr) return;
    MixingBuffer* reverb[4] = {&m_unk0_reverb, &m_unk1_reverb, &m_front_left_reverb, &m_front_right_reverb};
    std::array<s16, 8>* last8[4] = {&m_unk0_reverb_last8, &m_unk1_reverb_last8, &m_front_left_reverb_last8, &m_front_right_reverb_last8};
    for (u16 r = 0; r < 4; ++r) {
        ReverbPB rpb{};
        readWords(reinterpret_cast<u16*>(&rpb), reverb_pb_base_addr + r * u32(sizeof(ReverbPB)), sizeof(ReverbPB));
        if (!rpb.enabled) continue;
        u16 idx = m_reverb_pb_frames_count[r];
        const u32 mram = rpb.getCircularBufferBase() + idx * 0x50 * 2;
        if (!post) {
            std::array<s16, 0x58> buffer;  // 8 more: the filter's order (the previous frame's last 8)
            for (u16 i = 0; i < 8; ++i) buffer[i] = (*last8[r])[i];
            readWords(reinterpret_cast<u16*>(&buffer[8]), mram, 0x50 * 2);
            for (u16 i = 0; i < 8; ++i) (*last8[r])[i] = buffer[0x50 + i];
            auto filter = [&] {
                for (u16 i = 0; i < 0x50; ++i) {
                    s32 s = 0;
                    for (u16 j = 0; j < 8; ++j) s += s32(buffer[i + j]) * rpb.filter_coeffs[j];
                    buffer[i] = s16(std::clamp(s >> 15, -0x8000, 0x7FFF));
                }
            };
            if (rpb.enabled & 1) filter();  // pre-filtering
            for (const auto& d : rpb.dest) {
                if (!d.buffer_id) continue;
                if (MixingBuffer* dst = bufferForId(d.buffer_id)) addBuffersWithVolume(dst->data(), buffer.data(), 0x50, d.volume);
            }
            if (rpb.enabled & 2) filter();  // post-filtering
            for (u16 i = 0; i < 0x50; ++i) (*reverb[r])[i] = buffer[i];
        } else {
            writeWords(mram, reinterpret_cast<u16*>(reverb[r]->data()), 0x50 * 2);
            idx = rpb.circular_buffer_size ? u16((idx + 1) % rpb.circular_buffer_size) : 0;
            m_reverb_pb_frames_count[r] = idx;
        }
    }
}

void Renderer::lowPass(MixingBuffer* buf, VPB* vpb) {
    s32 yn1 = vpb->reset_vpb ? 0 : vpb->low_pass_yn1;
    s32 xn1 = vpb->reset_vpb ? 0 : vpb->low_pass_xn1;
    const s32 coeff = vpb->low_pass_coeff;  // 9.7
    for (int i = 0; i < 0x50; ++i) {
        const s32 xn0 = (*buf)[i];
        s64 tmp = s64(xn0 - xn1) * coeff;
        tmp >>= 7;
        tmp += yn1;
        const s16 yn0 = s16(std::clamp<s64>(tmp, -0x8000, 0x7FFF));
        (*buf)[i] = yn0;
        yn1 = yn0, xn1 = xn0;
    }
    vpb->low_pass_yn1 = s16(yn1);
    vpb->low_pass_xn1 = s16(xn1);
}

void Renderer::biquad(MixingBuffer* buf, VPB* vpb) {
    s32 xn1 = vpb->biquad_xn1, xn2 = vpb->biquad_xn2, yn1 = vpb->biquad_yn1, yn2 = vpb->biquad_yn2;
    for (int i = 0; i < 0x50; ++i) {
        const s32 xn0 = (*buf)[i];
        s64 tmp = s64(vpb->biquad_bn1) * xn1 + s64(vpb->biquad_bn2) * xn2 + s64(vpb->biquad_an1) * yn1 + s64(vpb->biquad_an2) * yn2;
        const s16 yn0 = s16(std::clamp<s64>(tmp >> 15, -0x8000, 0x7FFF));
        (*buf)[i] = yn0;
        xn2 = xn1, xn1 = xn0, yn2 = yn1, yn1 = yn0;
    }
    vpb->biquad_xn1 = s16(xn1), vpb->biquad_xn2 = s16(xn2), vpb->biquad_yn1 = s16(yn1), vpb->biquad_yn2 = s16(yn2);
}

void Renderer::addVoice(u16 voice_id) {
    VPB vpb;
    fetchVpb(voice_id, &vpb);
    if (!vpb.enabled || vpb.done) return;
    MixingBuffer input;
    loadInputSamples(&input, &vpb);
    if (vpb.low_pass_coeff) lowPass(&input, &vpb);
    if ((vpb.filter_flags & 0x20) && (vpb.biquad_an2 || vpb.biquad_an1 || vpb.biquad_bn2 || vpb.biquad_bn1 != 0x7FFF))
        biquad(&input, &vpb);
    if (vpb.use_dolby_volume) {
        if (vpb.end_requested) {
            vpb.dolby_volume_target = s16(vpb.dolby_volume_current / 2);
            if (vpb.dolby_volume_target == 0) vpb.done = 1;
        }
        const s16 right = sine_table[vpb.dolbyX()], back = sine_table[vpb.dolbyY()];
        const s16 left = sine_table[vpb.dolbyX() ^ 0x7F], front = sine_table[vpb.dolbyY() ^ 0x7F];
        const u16 shift = 16;  // (this microcode does not double the Dolby volumes)
        s16 quad[4] = {s16((left * front) >> shift), s16((left * back) >> shift), s16((right * front) >> shift),
                       s16((right * back) >> shift)};
        const s16 delta = s16(vpb.dolby_volume_target - vpb.dolby_volume_current);
        s16 deltas[4];
        for (size_t i = 0; i < 4; ++i) deltas[i] = s16((u16(quad[i]) * delta) >> shift);
        for (s16& q : quad) q = s16((q * vpb.dolby_volume_current) >> shift);
        s16 rv[4], rd[4];
        for (size_t i = 0; i < 4; ++i) {
            rv[i] = s16((quad[i] * vpb.dolby_reverb_factor) >> shift);
            rd[i] = s16((deltas[i] * vpb.dolby_reverb_factor) >> shift);
        }
        struct {
            MixingBuffer* buffer;
            s16 volume, delta;
        } outs[8] = {{&m_front_left, quad[0], deltas[0]},     {&m_back_left, quad[1], deltas[1]},
                     {&m_front_right, quad[2], deltas[2]},    {&m_back_right, quad[3], deltas[3]},
                     {&m_front_left_reverb, rv[0], rd[0]},    {&m_back_left_reverb, rv[1], rd[1]},
                     {&m_front_right_reverb, rv[2], rd[2]},   {&m_back_right_reverb, rv[3], rd[3]}};
        for (const auto& o : outs) addBuffersWithVolumeRamp(o.buffer, input, o.volume << 16, (o.delta << 16) / s32(o.buffer->size()));
        vpb.dolby_volume_current = vpb.dolby_volume_target;
    } else {
        constexpr int kChannels = 6;
        if (vpb.end_requested) {
            bool allMute = true;
            for (int i = 0; i < kChannels; ++i) {
                vpb.channels[i].target_volume = s16(vpb.channels[i].current_volume / 2);
                allMute &= vpb.channels[i].target_volume == 0;
            }
            if (allMute) vpb.done = 1;
        }
        for (int i = 0; i < kChannels; ++i) {
            if (!vpb.channels[i].id) continue;
            const s16 delta = s16(vpb.channels[i].target_volume - vpb.channels[i].current_volume);
            const s32 step = (delta << 16) / s32(input.size());  // 1.31
            if (!vpb.channels[i].current_volume && !step) continue;
            MixingBuffer* dst = bufferForId(vpb.channels[i].id);
            if (!dst) continue;
            const s32 nv = addBuffersWithVolumeRamp(dst, input, vpb.channels[i].current_volume << 16, step);
            vpb.channels[i].current_volume = s16(nv >> 16);
        }
    }
    if (!vpb.use_constant_sample) vpb.reset_vpb = 0;
    storeVpb(voice_id, &vpb);
}

void Renderer::finalizeFrame() {
    applyVolumeInPlace<0x50, 4>(&m_front_left, output_volume);
    applyVolumeInPlace<0x50, 4>(&m_front_right, output_volume);
    writeWords(output_lbuf_addr, reinterpret_cast<u16*>(m_front_left.data()), 0x50 * 2);
    writeWords(output_rbuf_addr, reinterpret_cast<u16*>(m_front_right.data()), 0x50 * 2);
    output_lbuf_addr += 0x50 * 2;
    output_rbuf_addr += 0x50 * 2;
    applyReverb(true);
    m_prepared = false;
}

void Renderer::loadInputSamples(MixingBuffer* buffer, VPB* vpb) {
    // raw samples before resampling: at most 0x500 (ratio 0xFFFF), plus 0x10 of rounding, plus the
    // previous frame's last 4 at the start
    std::array<s16, 4 + 0x500 + 0x10> raw;
    for (size_t i = 0; i < 4; ++i) raw[i] = vpb->resample_buffer[i];
    if (vpb->use_constant_sample) {
        buffer->fill(vpb->constant_sample);
        return;
    }
    switch (vpb->samples_source_type) {
    case VPB::SRC_SQUARE_WAVE:
    case VPB::SRC_SQUARE_WAVE_25PCT: {
        const u32 shift = vpb->samples_source_type == VPB::SRC_SQUARE_WAVE ? 1 : 2;
        const u32 mask = (1u << shift) - 1, ratio = u32(vpb->resampling_ratio) << (shift - 1);
        u32 pos = u32(vpb->current_pos_frac) << shift;
        for (s16& s : *buffer) {
            s = s16(((pos >> 16) & mask) ? 0xC000 : 0x4000);
            pos += ratio;
        }
        vpb->current_pos_frac = u16((pos >> shift) & 0xFFFF);
        break;
    }
    case VPB::SRC_SAW_WAVE: {
        u32 pos = vpb->current_pos_frac;
        for (s16& s : *buffer) {
            s = s16(pos & 0xFFFF);
            pos += vpb->resampling_ratio >> 1;
        }
        vpb->current_pos_frac = u16(pos);
        break;
    }
    case VPB::SRC_CONST_PATTERN_0:
    case VPB::SRC_CONST_PATTERN_0_VARIABLE_STEP:
    case VPB::SRC_CONST_PATTERN_1:
    case VPB::SRC_CONST_PATTERN_2:
    case VPB::SRC_CONST_PATTERN_3: {
        constexpr u16 kSize = 0x40;
        const u16 t = vpb->samples_source_type;
        const u16 idx = t == VPB::SRC_CONST_PATTERN_1 ? 1 : t == VPB::SRC_CONST_PATTERN_2 ? 2 : t == VPB::SRC_CONST_PATTERN_3 ? 3 : 0;
        const bool variable = t == VPB::SRC_CONST_PATTERN_0_VARIABLE_STEP;
        const s16* pattern = const_patterns.data() + idx * kSize;
        u32 pos = u32(vpb->current_pos_frac) << 6;
        const u32 step = u32(vpb->resampling_ratio) << 5;
        for (size_t i = 0; i < buffer->size(); ++i) {
            (*buffer)[i] = pattern[pos >> 16];
            pos = (pos + step) % (kSize << 16);
            if (variable) pos = u32((s64(pos) << 10) + m_back_right[i] * vpb->resampling_ratio) >> 10;
        }
        vpb->current_pos_frac = u16(pos >> 6);
        break;
    }
    case VPB::SRC_PCM8_FROM_ARAM:
        downloadPcmFromAram<s8>(raw.data() + 4, vpb, neededRawSamplesCount(*vpb));
        resample(vpb, raw.data(), buffer);
        break;
    case VPB::SRC_AFC_HQ_FROM_ARAM:
    case VPB::SRC_AFC_LQ_FROM_ARAM:
        downloadAfcFromAram(raw.data() + 4, vpb, neededRawSamplesCount(*vpb));
        resample(vpb, raw.data(), buffer);
        break;
    case VPB::SRC_PCM16_FROM_ARAM:
        downloadPcmFromAram<s16>(raw.data() + 4, vpb, neededRawSamplesCount(*vpb));
        resample(vpb, raw.data(), buffer);
        break;
    case VPB::SRC_PCM16_FROM_MRAM:
        downloadRawFromMram(raw.data() + 4, vpb, neededRawSamplesCount(*vpb));
        resample(vpb, raw.data(), buffer);
        break;
    default: {
        static bool s_warned = false;
        if (!s_warned) s_warned = true, log("[zelda] unknown sample source %04X\n", vpb->samples_source_type);
        buffer->fill(0);
        return;
    }
    }
}

void Renderer::resample(VPB* vpb, const s16* src, MixingBuffer* dst) {
    const u32 ratio = vpb->resampling_ratio;  // 20.12
    u32 pos = vpb->current_pos_frac;
    if ((ratio >> 12) >= 4) {  // (more than 4:1: no interpolation)
        for (s16& d : *dst) {
            pos += ratio;
            d = src[pos >> 12];
        }
    } else {
        for (s16& d : *dst) {
            const s16* coeffs = &resampling_coeffs[((pos & 0xFFF) >> 6) * 4];
            const s16* in = &src[pos >> 12];
            s64 v = 0;
            for (size_t i = 0; i < 4; ++i) v += s64(2) * coeffs[i] * in[i];
            d = s16(std::clamp<s64>(v >> 16, -0x8000, 0x7FFF));
            pos += ratio;
        }
    }
    for (u32 i = 0; i < 4; ++i) vpb->resample_buffer[i] = src[(pos >> 12) + i];
    vpb->constant_sample = (*dst)[dst->size() - 1];
    vpb->current_pos_frac = u16(pos & 0xFFF);
}

template <typename T>
void Renderer::downloadPcmFromAram(s16* dst, VPB* vpb, u16 count) {
    if (vpb->done) {
        std::fill(dst, dst + count, s16(0));
        return;
    }
    if (vpb->reset_vpb) {
        vpb->setRemainingLength(vpb->getLoopStartPosition() - vpb->getCurrentPosition());
        vpb->setCurrentARAMAddr(vpb->getBaseAddress() + vpb->getCurrentPosition() * u32(sizeof(T)));
    }
    vpb->end_reached = 0;
    while (count) {
        if (vpb->end_reached) {
            vpb->end_reached = 0;
            if (!vpb->is_looping) {
                std::fill(dst, dst + count, s16(0));
                vpb->done = 1;
                break;
            }
            vpb->setCurrentPosition(vpb->getLoopAddress());
            vpb->setRemainingLength(vpb->getLoopStartPosition() - vpb->getCurrentPosition());
            vpb->setCurrentARAMAddr(vpb->getBaseAddress() + vpb->getCurrentPosition() * u32(sizeof(T)));
        }
        const u16 n = u16(std::min(vpb->getRemainingLength(), u32(count)));
        u32 a = vpb->getCurrentARAMAddr();
        for (u16 i = 0; i < n; ++i, a += sizeof(T)) {
            if constexpr (sizeof(T) == 1) *dst++ = s16(s8(*aramPtr(a)) << 8);
            else *dst++ = s16(u16(*aramPtr(a)) << 8 | *aramPtr(a + 1));
        }
        vpb->setRemainingLength(vpb->getRemainingLength() - n);
        vpb->setCurrentARAMAddr(vpb->getCurrentARAMAddr() + n * u32(sizeof(T)));
        count = u16(count - n);
        if (!vpb->getRemainingLength()) vpb->end_reached = 1;
    }
}

void Renderer::downloadAfcFromAram(s16* dst, VPB* vpb, u16 count) {
    if (vpb->reset_vpb) {
        *vpb->afcYn1() = 0;
        *vpb->afcYn2() = 0;
        vpb->afc_remaining_decoded_samples = 0;
        vpb->setRemainingLength(vpb->getLoopStartPosition());
        vpb->setCurrentARAMAddr(vpb->getBaseAddress());
    }
    if (vpb->done) {
        std::fill(dst, dst + count, s16(0));
        return;
    }
    if (vpb->afc_remaining_decoded_samples > 0x10) vpb->afc_remaining_decoded_samples = 0x10;
    for (;;) {
        // what was decoded already first
        const u16 out = std::min(vpb->afc_remaining_decoded_samples, count);
        s16* base = &vpb->afc_remaining_samples[0x10 - vpb->afc_remaining_decoded_samples];
        for (size_t i = 0; i < out; ++i) *dst++ = base[i];
        vpb->afc_remaining_decoded_samples = u16(vpb->afc_remaining_decoded_samples - out);
        count = u16(count - out);
        if (count == 0) return;
        if (count <= vpb->getRemainingLength()) {
            const u16 blocks = u16((count + 0xF) >> 4);  // 16 samples a block
            const u16 decoded = u16(blocks << 4);
            if (decoded < vpb->getRemainingLength()) {
                vpb->afc_remaining_decoded_samples = u16(decoded - count);
                vpb->setRemainingLength(vpb->getRemainingLength() - decoded);
            } else {
                vpb->afc_remaining_decoded_samples = u16(vpb->getRemainingLength() - count);
                vpb->setRemainingLength(0);
            }
            decodeAfc(vpb, dst, blocks);
            if (vpb->afc_remaining_decoded_samples) {
                for (size_t i = 0; i < 0x10; ++i) vpb->afc_remaining_samples[i] = dst[decoded - 0x10 + i];
                if (!vpb->getRemainingLength() && vpb->getLoopStartPosition()) {  // the loop to come
                    base = vpb->afc_remaining_samples + ((vpb->getLoopStartPosition() + 0xF) & 0xF);
                    for (size_t i = 0; i < vpb->afc_remaining_decoded_samples; ++i) vpb->afc_remaining_samples[0x10 - i - 1] = *base--;
                }
            }
            return;
        }
        // more asked for than is left: finish, or loop
        if (vpb->getRemainingLength()) {
            count = u16(count - vpb->getRemainingLength());
            decodeAfc(vpb, dst, (vpb->getRemainingLength() + 0xF) >> 4);
            dst += vpb->getRemainingLength();
        }
        if (!vpb->is_looping) {
            vpb->done = 1;
            std::fill(dst, dst + count, s16(0));
            return;
        }
        // (the sample source number is also the number of bytes per 16 samples)
        const u32 loopOff = (vpb->getLoopAddress() >> 4) * vpb->samples_source_type;
        vpb->setCurrentARAMAddr(vpb->getBaseAddress() + loopOff);
        *vpb->afcYn2() = vpb->loop_yn2;
        *vpb->afcYn1() = vpb->loop_yn1;
        decodeAfc(vpb, vpb->afc_remaining_samples, 1);
        vpb->afc_remaining_decoded_samples = u16(0x10 - (vpb->getLoopAddress() & 0xF));
        u32 remaining = vpb->getLoopStartPosition();
        remaining -= vpb->afc_remaining_decoded_samples;
        remaining -= vpb->getLoopAddress();
        vpb->setRemainingLength(remaining);
    }
}

void Renderer::decodeAfc(VPB* vpb, s16* dst, size_t blocks) {
    u32 addr = vpb->getCurrentARAMAddr();
    vpb->setCurrentARAMAddr(addr + u32(blocks) * vpb->samples_source_type);
    for (size_t b = 0; b < blocks; ++b) {
        s16 nibbles[16];
        const u8 head = *aramPtr(addr++);
        const s16 delta = s16(1 << ((head >> 4) & 0xF));
        const s16 idx = s16(head & 0xF);
        if (vpb->samples_source_type == VPB::SRC_AFC_HQ_FROM_ARAM) {  // 4-bit samples
            for (size_t i = 0; i < 16; i += 2) {
                const u8 v = *aramPtr(addr++);
                nibbles[i] = v >> 4;
                nibbles[i + 1] = v & 0xF;
            }
            for (s16& n : nibbles) n = s16(s16(n << 12) >> 1);
        } else {  // 2-bit samples
            for (size_t i = 0; i < 16; i += 4) {
                const u8 v = *aramPtr(addr++);
                nibbles[i] = (v >> 6) & 3;
                nibbles[i + 1] = (v >> 4) & 3;
                nibbles[i + 2] = (v >> 2) & 3;
                nibbles[i + 3] = v & 3;
            }
            for (s16& n : nibbles) n = s16(s16(n << 14) >> 1);
        }
        s32 yn1 = *vpb->afcYn1(), yn2 = *vpb->afcYn2();
        for (s16 n : nibbles) {
            s32 s = delta * n + yn1 * afc_coeffs[idx * 2] + yn2 * afc_coeffs[idx * 2 + 1];
            s = std::clamp(s >> 11, -0x8000, 0x7FFF);
            *dst++ = s16(s);
            yn2 = yn1, yn1 = s;
        }
        *vpb->afcYn2() = s16(yn2);
        *vpb->afcYn1() = s16(yn1);
    }
}

void Renderer::downloadRawFromMram(s16* dst, VPB* vpb, u16 count) {
    const u32 addr = vpb->getBaseAddress() + vpb->current_position_h * 2u;
    const u32 remaining = vpb->getRemainingLength();
    if (count > remaining) {
        s16 last = 0;
        if (remaining) {
            readWords(reinterpret_cast<u16*>(dst), addr, remaining * 2);
            last = dst[remaining - 1];
        }
        for (u32 i = remaining; i < count; ++i) dst[i] = last;
        vpb->current_position_h = u16(vpb->current_position_h + remaining);
        vpb->setRemainingLength(0);
        vpb->done = 1;
    } else {
        vpb->setRemainingLength(remaining - count);
        vpb->samples_before_loop = u16(vpb->loop_start_position_h - vpb->current_position_h);
        if (count <= vpb->samples_before_loop) {
            readWords(reinterpret_cast<u16*>(dst), addr, count * 2u);
            vpb->current_position_h = u16(vpb->current_position_h + count);
        } else {
            readWords(reinterpret_cast<u16*>(dst), addr, vpb->samples_before_loop * 2u);
            vpb->setBaseAddress(vpb->getLoopAddress());
            vpb->current_position_h = u16(count - vpb->samples_before_loop);
            readWords(reinterpret_cast<u16*>(dst + vpb->samples_before_loop), vpb->getLoopAddress(), vpb->current_position_h * 2u);
        }
    }
}

// ---- the microcode: mail state machine, command buffer, rendering flow ----
enum class MailState { Waiting, Rendering, WritingCmd, Halted };
MailState s_state = MailState::Waiting;
u32 s_expectedCmdMails = 0;
u32 s_syncMaxVoiceId = 0;
std::array<u16, 256> s_syncVoiceSkipFlags{};
std::array<u32, 64> s_cmdBuffer{};
u32 s_readOffset = 0, s_writeOffset = 0, s_pendingCommands = 0;
bool s_cmdCanExecute = true;
u32 s_renderRequestedFrames = 0, s_renderCurFrame = 0, s_renderCurVoice = 0;
u16 s_voicesPerFrame = 0;
Renderer s_renderer;

bool renderingInProgress() { return s_renderCurFrame != s_renderRequestedFrames; }

u32 read32() {
    if (s_readOffset == s_writeOffset) return 0;
    const u32 v = s_cmdBuffer[s_readOffset];
    s_readOffset = (s_readOffset + 1) % s_cmdBuffer.size();
    return v;
}
void write32(u32 v) {
    s_cmdBuffer[s_writeOffset] = v;
    s_writeOffset = (s_writeOffset + 1) % s_cmdBuffer.size();
}

// SA2_ZELDA_LOG=<n>: the first n mails each way, logged (bring-up); the last 64 are kept too, for
// zeldaLogNext to show what led up to a problem.
int s_mailLog = std::getenv("SA2_ZELDA_LOG") ? std::atoi(std::getenv("SA2_ZELDA_LOG")) : 0;
struct MailRec {
    bool toCpu;
    u32 mail, frame, frames, voice, syncMax;
};
std::array<MailRec, 64> s_ring{};
u32 s_ringPos = 0;
void logMail(const char* dir, u32 mail) {
    s_ring[s_ringPos++ % s_ring.size()] = {dir[0] == '-', mail, s_renderCurFrame, s_renderRequestedFrames, s_renderCurVoice, s_syncMaxVoiceId};
    if (s_mailLog > 0) s_mailLog--, log("[zelda] %s %08X (frame %u/%u, voice %u, sync max %u)\n", dir, mail,
                                        s_renderCurFrame, s_renderRequestedFrames, s_renderCurVoice, s_syncMaxVoiceId);
}

enum class Ack { Standard, DoneRendering };
void sendAck(Ack type, u16 sync) {
    logMail("->cpu", type == Ack::Standard ? 0xF3550000u | sync : DSP_FRAME_END);
    dspPushMail(type == Ack::Standard ? DSP_SYNC : DSP_FRAME_END, true);
    if (type == Ack::Standard) dspPushMail(0xF3550000 | sync, false);
}

void renderAudio() {
    if (!renderingInProgress()) return;
    while (s_renderCurFrame < s_renderRequestedFrames) {
        if (s_renderCurVoice == 0) s_renderer.prepareFrame();
        const u16 voices = std::min<u16>(s_voicesPerFrame, u16(s_syncVoiceSkipFlags.size() << 4));
        while (s_renderCurVoice < voices) {
            if (s_renderCurVoice >= s_syncMaxVoiceId) return;  // not synced up to here yet: more mail first
            const u16 flags = s_syncVoiceSkipFlags[s_renderCurVoice >> 4];
            if (flags & (1u << (0xF - (s_renderCurVoice & 0xF)))) s_renderer.addVoice(u16(s_renderCurVoice));
            s_renderCurVoice++;
        }
        sendAck(Ack::Standard, u16(0xFF00 | s_renderCurFrame));
        s_renderer.finalizeFrame();
        s_renderCurVoice = 0;
        s_syncMaxVoiceId = 0;
        s_renderCurFrame++;
    }
    sendAck(Ack::DoneRendering, 0);
    s_cmdCanExecute = false;  // until the CPU acknowledges
}

void runPendingCommands() {
    if (renderingInProgress() || !s_cmdCanExecute) return;
    while (s_pendingCommands) {
        const u32 cmd = read32();
        if (!(cmd & 0x80000000)) continue;
        const u32 command = (cmd >> 24) & 0x7F;
        const u16 sync = u16(cmd >> 16);
        const u32 extra = cmd & 0xFFFF;
        s_pendingCommands--;
        switch (command) {
        case 0x00:
        case 0x0A:
        case 0x0B:
        case 0x0F:
        case 0x03: sendAck(Ack::Standard, sync); break;  // NOPs
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
        case 0x08:
        case 0x09:  // (these crash the real microcode)
            log("[zelda] crashing command %02X: halted\n", command);
            s_state = MailState::Halted;
            return;
        case 0x01: {  // setup: VPBs, resampling coefficients, patterns, sine table, AFC coefficients, reverb
            s_voicesPerFrame = u16(extra);
            s_renderer.vpb_base_addr = read32();
            const u32 tables = read32();
            readArray(s_renderer.resampling_coeffs, tables);
            readArray(s_renderer.const_patterns, tables + 0x200);
            readArray(s_renderer.sine_table, tables + 0x400);
            readArray(s_renderer.afc_coeffs, read32());
            s_renderer.reverb_pb_base_addr = read32();
            log("[zelda] setup: %u voices, VPBs at %08X, reverb at %08X\n", s_voicesPerFrame, s_renderer.vpb_base_addr,
                s_renderer.reverb_pb_base_addr);
            sendAck(Ack::Standard, sync);
            break;
        }
        case 0x02:  // render: takes over the mail flow until the frames are done
            s_renderRequestedFrames = (cmd >> 16) & 0xFF;
            s_renderer.output_volume = u16(cmd & 0xFFFF);
            s_renderer.output_lbuf_addr = read32();
            s_renderer.output_rbuf_addr = read32();
            s_renderCurFrame = 0;
            s_renderCurVoice = 0;
            renderAudio();
            return;
        case 0x0C: sendAck(Ack::Standard, sync); break;  // (a NOP in this microcode)
        case 0x0D:                                        // (unknown purpose; Wind Waker sends it)
            read32();
            sendAck(Ack::Standard, sync);
            break;
        case 0x0E:  // (Wii ARAM base: not on this microcode)
            read32();
            sendAck(Ack::Standard, sync);
            break;
        default:
            log("[zelda] unknown command %02X: halted\n", command);
            s_state = MailState::Halted;
            return;
        }
    }
}
}  // namespace

// Bring-up: log the next n mails (from OSReport when the game reports a problem, SA2_ZELDA_LOG).
void zeldaLogNext(int n) {
    log("[zelda] the last %zu mails:\n", s_ring.size());
    for (u32 i = 0; i < s_ring.size(); i++) {
        const MailRec& r = s_ring[(s_ringPos + i) % s_ring.size()];
        log("[zelda]   %s %08X (frame %u/%u, voice %u, sync max %u)\n", r.toCpu ? "->cpu" : "<-cpu", r.mail, r.frame, r.frames, r.voice, r.syncMax);
    }
    s_mailLog = n;
}

// The microcode has started (the boot ROM jumped to it): it greets the CPU.
void zeldaStart() {
    s_state = MailState::Waiting;
    s_expectedCmdMails = s_syncMaxVoiceId = 0;
    s_readOffset = s_writeOffset = s_pendingCommands = 0;
    s_cmdCanExecute = true;
    s_renderRequestedFrames = s_renderCurFrame = s_renderCurVoice = 0;
    dspPushMail(DSP_INIT, true);
    dspPushMail(0xF3551111, false);  // the handshake (DspHandShake waits for it)
}

// Returns true when the microcode is being replaced (the boot ROM takes the next mails).
bool zeldaMail(u32 mail) {
    logMail("<-cpu", mail);
    switch (s_state) {
    case MailState::Waiting:
        if (mail & 0x80000000) {
            if ((mail & TASK_MAIL_MASK) != TASK_MAIL_TO_DSP) mail = TASK_MAIL_TO_DSP | (mail & ~TASK_MAIL_MASK);  // (not checked)
            switch (mail) {
            case MAIL_NEW_UCODE:
                s_cmdCanExecute = true;
                runPendingCommands();
                log("[zelda] microcode being replaced\n");
                return true;
            case MAIL_RESET:
                log("[zelda] rebooted to the boot ROM\n");
                return true;
            case MAIL_CONTINUE:
                s_cmdCanExecute = true;
                runPendingCommands();
                break;
            default:  // MAIL_RESUME, or an unknown end-of-rendering action
                s_state = MailState::Halted;
                break;
            }
        } else if (!(mail & 0xFFFF)) {
            s_state = renderingInProgress() ? MailState::Rendering : MailState::Halted;
        } else {
            s_state = MailState::WritingCmd;
            s_expectedCmdMails = mail & 0xFFFF;
        }
        break;
    case MailState::Rendering:
        // (not this microcode's sync-per-frame variant: one mail syncs 16 voices)
        s_syncMaxVoiceId = (((mail >> 16) & 0xF) + 1) << 4;
        s_syncVoiceSkipFlags[(mail >> 16) & 0xFF] = u16(mail & 0xFFFF);
        renderAudio();
        s_state = MailState::Waiting;
        break;
    case MailState::WritingCmd:
        write32(mail);
        if (--s_expectedCmdMails == 0) {
            s_pendingCommands++;
            s_state = MailState::Waiting;
            runPendingCommands();
        }
        break;
    case MailState::Halted: break;
    }
    return false;
}

// Whether a voice that is not finished reads its samples from ARAM in [lo, hi) (bod_voice.cpp swaps a wave file there
// only when none does). Called on the game thread; the voice blocks are read as the DSP does.
bool zeldaAramInUse(u32 lo, u32 hi) {
    if (!s_renderer.vpb_base_addr) return false;
    VPB vpb;
    for (u16 v = 0; v < s_voicesPerFrame; ++v) {
        s_renderer.readVpb(v, &vpb);
        if (!vpb.enabled || vpb.done) continue;
        const u16 t = vpb.samples_source_type;
        if (t != VPB::SRC_AFC_LQ_FROM_ARAM && t != VPB::SRC_AFC_HQ_FROM_ARAM && t != VPB::SRC_PCM8_FROM_ARAM && t != VPB::SRC_PCM16_FROM_ARAM) continue;
        const u32 cur = vpb.getCurrentARAMAddr(), base = vpb.getBaseAddress();
        if ((cur >= lo && cur < hi) || (base >= lo && base < hi)) return true;
    }
    return false;
}

}  // namespace rt
