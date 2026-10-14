// DSP-ADPCM as MLGC stores it (tools/dspadpcm.py makes it): a clip is blocks of up to 512
// frames a channel, each channel's block a 64-byte header (its 8 coefficient pairs and the two
// samples before it) and then its frames, 8 bytes and 14 samples each. A block decodes on its
// own from its header, exactly as the encoder decoded it (tools/dspadpcm.py's decode() is the
// same arithmetic).
#pragma once

#include <cstdint>

namespace dsp {

constexpr uint32_t kFrameSamples = 14;
constexpr uint32_t kFrameBytes = 8;
constexpr uint32_t kBlockFrames = 512;
constexpr uint32_t kBlockSamples = kBlockFrames * kFrameSamples; // 7168
constexpr uint32_t kHeaderBytes = 64;
constexpr uint32_t kChannelBlockBytes = kHeaderBytes + kBlockFrames * kFrameBytes; // 4160

// A channel's block header (big-endian on the disc: the GameCube's own order).
struct BlockHeader {
    int16_t coefs[16]; // 8 pairs: c1 (times the last sample), c2 (times the one before)
    int16_t yn1, yn2;  // the samples before the block
    uint16_t frames;   // in this block
    uint8_t pad[26];
};
static_assert(sizeof(BlockHeader) == kHeaderBytes, "the block header is 64 bytes");

// A frame's 14 samples; yn1 and yn2 carry the last two from frame to frame.
inline void DecodeFrame(const uint8_t* frame, const int16_t* coefs, int32_t& yn1, int32_t& yn2, int16_t* out)
{
    const int predictor = frame[0] >> 4, shift = frame[0] & 15;
    const int32_t c1 = coefs[predictor * 2], c2 = coefs[predictor * 2 + 1];
    const int32_t scale = (1 << shift) * 2048;
    for (uint32_t i = 0; i < kFrameSamples; i++) {
        const uint8_t byte = frame[1 + i / 2];
        int32_t residual = (i & 1) ? (byte & 15) : (byte >> 4);
        if (residual >= 8) residual -= 16;
        int32_t s = (residual * scale + 1024 + c1 * yn1 + c2 * yn2) >> 11;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out[i] = static_cast<int16_t>(s);
        yn2 = yn1;
        yn1 = s;
    }
}

} // namespace dsp
