#include "punktfunkdecodeunit.h"

#include <cstring>

namespace {

// punktfunk_core.h's PUNKTFUNK_CODEC_* values, restated so this file needs no Punktfunk header.
constexpr uint8_t kCodecH264 = 1;
constexpr uint8_t kCodecHevc = 2;
constexpr uint8_t kCodecAv1  = 4;

// Offset of the next Annex B start code at or after `from`, and its length (3 or 4). Returns
// `length` when there is none.
size_t findStartCode(const uint8_t* data, size_t length, size_t from, size_t& codeLength)
{
    for (size_t i = from; i + 3 <= length; i++) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                // A zero before 00 00 01 belongs to a 4-byte code.
                if (i > from && data[i - 1] == 0) {
                    codeLength = 4;
                    return i - 1;
                }
                codeLength = 3;
                return i;
            }
        }
    }
    codeLength = 0;
    return length;
}

} // namespace

PunktfunkDecodeUnitBuilder::PunktfunkDecodeUnitBuilder(int videoFormat)
    : m_VideoFormat(videoFormat)
{
    memset(&m_Unit, 0, sizeof(m_Unit));
}

int PunktfunkDecodeUnitBuilder::videoFormatForCodec(uint8_t punktfunkCodec, bool tenBit)
{
    switch (punktfunkCodec) {
    case kCodecH264:
        return VIDEO_FORMAT_H264;
    case kCodecHevc:
        return tenBit ? VIDEO_FORMAT_H265_MAIN10 : VIDEO_FORMAT_H265;
    case kCodecAv1:
        return tenBit ? VIDEO_FORMAT_AV1_MAIN10 : VIDEO_FORMAT_AV1_MAIN8;
    default:
        return 0;
    }
}

void PunktfunkDecodeUnitBuilder::buildAnnexB(const uint8_t* data, size_t length, bool& keyframe)
{
    const bool hevc = (m_VideoFormat & VIDEO_FORMAT_MASK_H265) != 0;

    size_t codeLength;
    size_t nalStart = findStartCode(data, length, 0, codeLength);
    if (nalStart != 0) {
        // Not Annex B, or leading garbage: pass it through whole rather than guess.
        m_Entries.push_back({ nullptr, (char*)data, (int)length, BUFFER_TYPE_PICDATA });
        return;
    }

    while (nalStart < length) {
        size_t headerAt = nalStart + codeLength;
        size_t nextCodeLength;
        size_t nalEnd = findStartCode(data, length, headerAt, nextCodeLength);

        int bufferType = BUFFER_TYPE_PICDATA;
        if (headerAt < length) {
            if (hevc) {
                int type = (data[headerAt] >> 1) & 0x3F;
                if (type == 32) bufferType = BUFFER_TYPE_VPS;
                else if (type == 33) bufferType = BUFFER_TYPE_SPS;
                else if (type == 34) bufferType = BUFFER_TYPE_PPS;
                // IRAP pictures: BLA, IDR and CRA (16..21).
                if (type >= 16 && type <= 21) keyframe = true;
            }
            else {
                int type = data[headerAt] & 0x1F;
                if (type == 7) bufferType = BUFFER_TYPE_SPS;
                else if (type == 8) bufferType = BUFFER_TYPE_PPS;
                if (type == 5) keyframe = true;
            }
        }
        if (bufferType != BUFFER_TYPE_PICDATA) {
            keyframe = true;
        }

        // Consecutive picture NALs share one entry: the decoder concatenates them anyway, and
        // only parameter sets need to stand alone.
        if (bufferType == BUFFER_TYPE_PICDATA && !m_Entries.empty() &&
                m_Entries.back().bufferType == BUFFER_TYPE_PICDATA) {
            m_Entries.back().length += (int)(nalEnd - nalStart);
        }
        else {
            m_Entries.push_back({ nullptr, (char*)data + nalStart, (int)(nalEnd - nalStart), bufferType });
        }

        nalStart = nalEnd;
        codeLength = nextCodeLength;
    }
}

bool PunktfunkDecodeUnitBuilder::av1HasSequenceHeader(const uint8_t* data, size_t length)
{
    // Low-overhead bitstream format: each OBU header, an optional extension byte, then a
    // leb128 size. A keyframe carries the sequence header in front of it.
    size_t at = 0;
    while (at < length) {
        uint8_t header = data[at];
        int type = (header >> 3) & 0x0F;
        bool hasExtension = (header & 0x04) != 0;
        bool hasSize = (header & 0x02) != 0;
        if (type == 1) {
            return true;
        }
        at += 1 + (hasExtension ? 1 : 0);
        if (!hasSize) {
            // The rest of the unit is this OBU; nothing after it to read.
            return false;
        }
        uint64_t size = 0;
        for (int i = 0; i < 8; i++) {
            if (at >= length) return false;
            uint8_t b = data[at++];
            size |= uint64_t(b & 0x7F) << (7 * i);
            if ((b & 0x80) == 0) break;
        }
        if (size > length - at) {
            return false;
        }
        at += (size_t)size;
    }
    return false;
}

PDECODE_UNIT PunktfunkDecodeUnitBuilder::build(const uint8_t* data, size_t length,
                                               uint32_t frameIndex, uint64_t ptsNs, uint64_t nowUs)
{
    if (data == nullptr || length == 0) {
        return nullptr;
    }

    m_Entries.clear();
    bool keyframe = false;

    if (m_VideoFormat & VIDEO_FORMAT_MASK_AV1) {
        keyframe = av1HasSequenceHeader(data, length);
        m_Entries.push_back({ nullptr, (char*)data, (int)length, BUFFER_TYPE_PICDATA });
    }
    else {
        buildAnnexB(data, length, keyframe);
    }

    // Linked last: push_back may have moved the vector.
    for (size_t i = 0; i + 1 < m_Entries.size(); i++) {
        m_Entries[i].next = &m_Entries[i + 1];
    }

    if (!m_HaveFirstPts) {
        m_HaveFirstPts = true;
        m_FirstPtsNs = ptsNs;
    }
    uint64_t sinceFirstUs = ptsNs >= m_FirstPtsNs ? (ptsNs - m_FirstPtsNs) / 1000 : 0;

    memset(&m_Unit, 0, sizeof(m_Unit));
    m_Unit.frameNumber = (int)frameIndex;
    m_Unit.frameType = keyframe ? FRAME_TYPE_IDR : FRAME_TYPE_PFRAME;
    m_Unit.receiveTimeUs = nowUs;
    m_Unit.enqueueTimeUs = nowUs;
    m_Unit.presentationTimeUs = sinceFirstUs;
    m_Unit.rtpTimestamp = (uint32_t)(sinceFirstUs * 90 / 1000);
    m_Unit.fullLength = (int)length;
    m_Unit.bufferList = m_Entries.data();
    return &m_Unit;
}
