#pragma once

#include <Limelight.h>

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * One punktfunk/1 access unit, reshaped into the DECODE_UNIT moonlight-common-c would have
 * delivered for the same frame (6.4.0, native Punktfunk path).
 *
 * The decoders only know DECODE_UNIT, and what they need from it is specific:
 *
 *  - H.264 / HEVC parameter sets as their OWN entries, typed BUFFER_TYPE_SPS / PPS / VPS, each
 *    holding exactly one NAL with its Annex B start code. FFmpegVideoDecoder::writeBuffer()
 *    rewrites the SPS in place and asserts that the entry ends where the NAL does.
 *  - FRAME_TYPE_IDR on a keyframe. The decoder drops everything before the first one and sizes
 *    its buffer differently for it.
 *
 * Punktfunk hands over the whole access unit as one Annex B (or AV1 OBU) buffer with the
 * parameter sets in band, so this splits it. Every entry points into the caller's buffer:
 * nothing is copied, and the result is valid only as long as that buffer is — for
 * punktfunk_connection_next_au(), until the next call on the same connection.
 *
 * No dependency on punktfunk_core.h, deliberately: this is the part of the native path that
 * can be built and tested on any machine.
 */
class PunktfunkDecodeUnitBuilder
{
public:
    // `videoFormat` is a moonlight-common-c VIDEO_FORMAT_* value.
    explicit PunktfunkDecodeUnitBuilder(int videoFormat);

    // PUNKTFUNK_CODEC_* (1 H.264, 2 HEVC, 4 AV1) to VIDEO_FORMAT_*. 0 for anything else,
    // PyroWave included: no decoder in StreamLight can take it.
    static int videoFormatForCodec(uint8_t punktfunkCodec, bool tenBit);

    // Null for an empty unit. `ptsNs` is the host's capture time; `nowUs` is the local
    // monotonic clock the decoder's statistics are kept in, not Punktfunk's CLOCK_REALTIME
    // `received_ns`, which would make every reassembly time meaningless.
    //
    // The returned unit and its buffer list belong to this builder and are overwritten by the
    // next call.
    PDECODE_UNIT build(const uint8_t* data, size_t length,
                       uint32_t frameIndex, uint64_t ptsNs, uint64_t nowUs);

private:
    void buildAnnexB(const uint8_t* data, size_t length, bool& keyframe);
    static bool av1HasSequenceHeader(const uint8_t* data, size_t length);

    int m_VideoFormat;
    bool m_HaveFirstPts = false;
    uint64_t m_FirstPtsNs = 0;
    std::vector<LENTRY> m_Entries;
    DECODE_UNIT m_Unit;
};
