// Deterministic checks for PunktfunkDecodeUnitBuilder: the entry layout the FFmpeg decoder
// relies on (one NAL per parameter-set entry, start code included) and keyframe detection.

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <vector>

#include "../../app/streaming/punktfunk/punktfunkdecodeunit.h"

static int entryCount(PDECODE_UNIT du)
{
    int n = 0;
    for (PLENTRY e = du->bufferList; e != nullptr; e = e->next) n++;
    return n;
}

static PLENTRY entryAt(PDECODE_UNIT du, int index)
{
    PLENTRY e = du->bufferList;
    while (index-- > 0) e = e->next;
    return e;
}

static void h264Keyframe()
{
    // SPS, PPS (3-byte code), IDR slice, then a second slice NAL.
    std::vector<uint8_t> au = {
        0, 0, 0, 1, 0x67, 0xAA, 0xBB,
        0, 0, 1,    0x68, 0xCC,
        0, 0, 0, 1, 0x65, 0x01, 0x02, 0x03,
        0, 0, 0, 1, 0x65, 0x04,
    };
    PunktfunkDecodeUnitBuilder b(VIDEO_FORMAT_H264);
    PDECODE_UNIT du = b.build(au.data(), au.size(), 7, 1000000, 42);
    assert(du != nullptr);
    assert(du->frameType == FRAME_TYPE_IDR);
    assert(du->frameNumber == 7);
    assert(du->fullLength == (int)au.size());
    assert(du->receiveTimeUs == 42 && du->enqueueTimeUs == 42);
    assert(du->presentationTimeUs == 0);
    assert(entryCount(du) == 3);
    assert(entryAt(du, 0)->bufferType == BUFFER_TYPE_SPS);
    assert(entryAt(du, 0)->length == 7);
    assert(entryAt(du, 0)->data == (char*)au.data());
    assert(entryAt(du, 1)->bufferType == BUFFER_TYPE_PPS);
    assert(entryAt(du, 1)->length == 5);
    assert(entryAt(du, 2)->bufferType == BUFFER_TYPE_PICDATA);
    assert(entryAt(du, 2)->length == 14); // both slices merged

    int total = 0;
    for (PLENTRY e = du->bufferList; e; e = e->next) total += e->length;
    assert(total == du->fullLength);

    // A later P-frame: timing is relative to the first unit.
    std::vector<uint8_t> p = { 0, 0, 0, 1, 0x41, 0x09 };
    du = b.build(p.data(), p.size(), 8, 1000000 + 16667000, 50);
    assert(du->frameType == FRAME_TYPE_PFRAME);
    assert(entryCount(du) == 1);
    assert(du->presentationTimeUs == 16667);
    assert(du->rtpTimestamp == 1500);
}

static void hevcKeyframe()
{
    std::vector<uint8_t> au = {
        0, 0, 0, 1, 0x40, 0x01, 0x0C,   // VPS (32)
        0, 0, 0, 1, 0x42, 0x01, 0x01,   // SPS (33)
        0, 0, 0, 1, 0x44, 0x01, 0xC0,   // PPS (34)
        0, 0, 0, 1, 0x26, 0x01, 0xAF,   // IDR_W_RADL (19)
    };
    PunktfunkDecodeUnitBuilder b(VIDEO_FORMAT_H265_MAIN10);
    PDECODE_UNIT du = b.build(au.data(), au.size(), 1, 0, 0);
    assert(du->frameType == FRAME_TYPE_IDR);
    assert(entryCount(du) == 4);
    assert(entryAt(du, 0)->bufferType == BUFFER_TYPE_VPS);
    assert(entryAt(du, 1)->bufferType == BUFFER_TYPE_SPS);
    assert(entryAt(du, 2)->bufferType == BUFFER_TYPE_PPS);
    assert(entryAt(du, 3)->bufferType == BUFFER_TYPE_PICDATA);

    std::vector<uint8_t> p = { 0, 0, 1, 0x02, 0x01, 0xD0 }; // TRAIL_R (1)
    du = b.build(p.data(), p.size(), 2, 0, 0);
    assert(du->frameType == FRAME_TYPE_PFRAME);
}

static void av1()
{
    // Temporal delimiter (2, size 0), sequence header (1, size 2), frame (6, size 1).
    std::vector<uint8_t> key = { 0x12, 0x00, 0x0A, 0x02, 0xAA, 0xBB, 0x32, 0x01, 0xCC };
    PunktfunkDecodeUnitBuilder b(VIDEO_FORMAT_AV1_MAIN8);
    PDECODE_UNIT du = b.build(key.data(), key.size(), 1, 0, 0);
    assert(du->frameType == FRAME_TYPE_IDR);
    assert(entryCount(du) == 1);
    assert(entryAt(du, 0)->length == (int)key.size());

    std::vector<uint8_t> inter = { 0x12, 0x00, 0x32, 0x01, 0xCC };
    du = b.build(inter.data(), inter.size(), 2, 0, 0);
    assert(du->frameType == FRAME_TYPE_PFRAME);

    // A size that runs past the end must not be followed.
    std::vector<uint8_t> bad = { 0x12, 0x7F };
    du = b.build(bad.data(), bad.size(), 3, 0, 0);
    assert(du->frameType == FRAME_TYPE_PFRAME);
}

static void edges()
{
    PunktfunkDecodeUnitBuilder b(VIDEO_FORMAT_H264);
    assert(b.build(nullptr, 0, 0, 0, 0) == nullptr);

    // No start code: passed through whole.
    std::vector<uint8_t> raw = { 1, 2, 3, 4 };
    PDECODE_UNIT du = b.build(raw.data(), raw.size(), 0, 0, 0);
    assert(entryCount(du) == 1 && entryAt(du, 0)->length == 4);

    assert(PunktfunkDecodeUnitBuilder::videoFormatForCodec(1, false) == VIDEO_FORMAT_H264);
    assert(PunktfunkDecodeUnitBuilder::videoFormatForCodec(2, true) == VIDEO_FORMAT_H265_MAIN10);
    assert(PunktfunkDecodeUnitBuilder::videoFormatForCodec(4, false) == VIDEO_FORMAT_AV1_MAIN8);
    assert(PunktfunkDecodeUnitBuilder::videoFormatForCodec(8, false) == 0); // PyroWave
}

int main()
{
    h264Keyframe();
    hevcKeyframe();
    av1();
    edges();
    std::puts("tst_punktfunkdecodeunit: all passed");
    return 0;
}
