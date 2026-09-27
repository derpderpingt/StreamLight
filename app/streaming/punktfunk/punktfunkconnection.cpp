#include "punktfunkconnection.h"

#include <punktfunk_core.h>

#include <QElapsedTimer>
#include <QSettings>

#include <cstring>

#define SER_PFCERT "punktfunk/clientcert"
#define SER_PFKEY  "punktfunk/clientkey"

namespace {

uint8_t codecMaskFor(int videoFormats)
{
    uint8_t mask = 0;
    if (videoFormats & VIDEO_FORMAT_MASK_H264) mask |= PUNKTFUNK_CODEC_H264;
    if (videoFormats & VIDEO_FORMAT_MASK_H265) mask |= PUNKTFUNK_CODEC_HEVC;
    if (videoFormats & VIDEO_FORMAT_MASK_AV1)  mask |= PUNKTFUNK_CODEC_AV1;
    return mask;
}

uint8_t videoCapsFor(int videoFormats)
{
    return (videoFormats & VIDEO_FORMAT_MASK_10BIT) ? PUNKTFUNK_VIDEO_CAP_10BIT : 0;
}

// Local monotonic microseconds, the clock the decoder keeps its statistics in.
uint64_t nowUs()
{
    static QElapsedTimer clock = [] { QElapsedTimer t; t.start(); return t; }();
    return (uint64_t)clock.nsecsElapsed() / 1000;
}

} // namespace

bool PunktfunkSession::libraryCompatible()
{
    return punktfunk_abi_version() == PUNKTFUNK_ABI_VERSION;
}

QString PunktfunkSession::statusText(int status)
{
    switch (status) {
    case PUNKTFUNK_STATUS_OK: return QStringLiteral("OK");
    case PUNKTFUNK_STATUS_CRYPTO: return QStringLiteral("Wrong PIN or host certificate mismatch");
    case PUNKTFUNK_STATUS_TIMEOUT: return QStringLiteral("The host did not answer in time");
    case PUNKTFUNK_STATUS_CLOSED: return QStringLiteral("The session has ended");
    case PUNKTFUNK_STATUS_UNSUPPORTED: return QStringLiteral("Not supported by this host");
    case PUNKTFUNK_STATUS_REJECTED_NOT_ARMED: return QStringLiteral("The host is not accepting new devices");
    case PUNKTFUNK_STATUS_REJECTED_IDENTITY_REQUIRED: return QStringLiteral("This device has to be paired first");
    case PUNKTFUNK_STATUS_REJECTED_DENIED: return QStringLiteral("The host refused this device");
    case PUNKTFUNK_STATUS_REJECTED_APPROVAL_TIMEOUT: return QStringLiteral("Nobody approved the connection on the host");
    case PUNKTFUNK_STATUS_REJECTED_WIRE_VERSION: return QStringLiteral("The host runs a different punktfunk/1 revision");
    case PUNKTFUNK_STATUS_REJECTED_BUSY: return QStringLiteral("The host is busy with another session");
    case PUNKTFUNK_STATUS_REJECTED_SETUP_FAILED: return QStringLiteral("The host could not start the stream");
    case PUNKTFUNK_STATUS_REJECTED_ACCESS_EXPIRED: return QStringLiteral("This device's access has expired");
    case PUNKTFUNK_STATUS_REJECTED_LAUNCH_NOT_PERMITTED: return QStringLiteral("This device may not launch apps on the host");
    default: return QStringLiteral("Punktfunk error %1").arg(status);
    }
}

bool PunktfunkSession::clientIdentity(QByteArray& certPem, QByteArray& keyPem)
{
    QSettings settings;
    certPem = settings.value(SER_PFCERT).toByteArray();
    keyPem = settings.value(SER_PFKEY).toByteArray();
    if (!certPem.isEmpty() && !keyPem.isEmpty()) {
        return true;
    }

    // 4096 bytes each is what the library documents as ample.
    char cert[4096] = {};
    char key[4096] = {};
    if (punktfunk_generate_identity(cert, sizeof(cert), key, sizeof(key)) != PUNKTFUNK_STATUS_OK) {
        return false;
    }
    certPem = QByteArray(cert);
    keyPem = QByteArray(key);
    settings.setValue(SER_PFCERT, certPem);
    settings.setValue(SER_PFKEY, keyPem);
    return true;
}

int PunktfunkSession::pair(const QString& host, quint16 port, const QString& pin,
                           const QString& deviceName, QByteArray& hostSha256)
{
    QByteArray cert, key;
    if (!clientIdentity(cert, key)) {
        return PUNKTFUNK_STATUS_CRYPTO;
    }

    uint8_t fp[32] = {};
    int status = punktfunk_pair(host.toUtf8().constData(), port,
                                cert.constData(), key.constData(),
                                pin.toUtf8().constData(), deviceName.toUtf8().constData(),
                                fp, 30000);
    if (status == PUNKTFUNK_STATUS_OK) {
        hostSha256 = QByteArray((const char*)fp, sizeof(fp));
    }
    return status;
}

bool PunktfunkSession::probe(const QString& host, quint16 port, const QByteArray& pinSha256,
                             quint32 timeoutMs)
{
    uint8_t observed[32] = {};
    if (punktfunk_probe(host.toUtf8().constData(), port, timeoutMs, observed) != PUNKTFUNK_STATUS_OK) {
        return false;
    }
    // Something answered. Whether it is OUR host is the pin's to say: a stranger on the same
    // lease answers too.
    return pinSha256.size() == 32 && memcmp(observed, pinSha256.constData(), 32) == 0;
}

PunktfunkSession::~PunktfunkSession()
{
    close();
}

int PunktfunkSession::connect(const Params& params)
{
    close();

    if (params.pinSha256.size() != 32) {
        return PUNKTFUNK_STATUS_INVALID_ARG;
    }

    QByteArray cert, key;
    if (!clientIdentity(cert, key)) {
        return PUNKTFUNK_STATUS_CRYPTO;
    }

    const QByteArray host = params.host.toUtf8();
    const QByteArray deviceName = params.deviceName.toUtf8();

    PunktfunkConnectOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.struct_size = sizeof(opts);
    opts.host = host.constData();
    opts.pin_sha256 = (const uint8_t*)params.pinSha256.constData();
    opts.client_cert_pem = cert.constData();
    opts.client_key_pem = key.constData();
    opts.device_name = deviceName.isEmpty() ? nullptr : deviceName.constData();
    opts.width = (uint32_t)params.width;
    opts.height = (uint32_t)params.height;
    opts.refresh_hz = (uint32_t)params.refreshHz;
    opts.bitrate_kbps = (uint32_t)params.bitrateKbps;
    opts.timeout_ms = params.timeoutMs;
    opts.port = params.port;
    opts.video_caps = videoCapsFor(params.supportedVideoFormats);
    opts.audio_channels = (uint8_t)params.audioChannels;
    opts.video_codecs = codecMaskFor(params.supportedVideoFormats);
    opts.preferred_codec = codecMaskFor(params.preferredVideoFormat);

    int32_t status = PUNKTFUNK_STATUS_IO;
    m_Connection = punktfunk_connect_opts(&opts, nullptr, &status);
    if (m_Connection == nullptr) {
        return status != PUNKTFUNK_STATUS_OK ? status : PUNKTFUNK_STATUS_IO;
    }

    uint8_t codec = 0;
    punktfunk_connection_codec(m_Connection, &codec);
    // 10-bit only when it was offered: the host never picks a depth the client did not ask for.
    bool tenBit = (params.supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) != 0 &&
                  (params.preferredVideoFormat & VIDEO_FORMAT_MASK_10BIT) != 0;
    m_VideoFormat = PunktfunkDecodeUnitBuilder::videoFormatForCodec(codec, tenBit);
    if (m_VideoFormat == 0) {
        close();
        return PUNKTFUNK_STATUS_UNSUPPORTED;
    }
    m_Builder.reset(new PunktfunkDecodeUnitBuilder(m_VideoFormat));
    return PUNKTFUNK_STATUS_OK;
}

PDECODE_UNIT PunktfunkSession::nextDecodeUnit(quint32 timeoutMs, bool& closed)
{
    closed = false;
    if (m_Connection == nullptr) {
        closed = true;
        return nullptr;
    }

    for (;;) {
        PunktfunkFrame frame;
        memset(&frame, 0, sizeof(frame));
        int status = punktfunk_connection_next_au(m_Connection, &frame, timeoutMs);
        if (status == PUNKTFUNK_STATUS_CLOSED) {
            closed = true;
            return nullptr;
        }
        if (status != PUNKTFUNK_STATUS_OK) {
            return nullptr;
        }
        // Bandwidth-probe filler is not video.
        if (frame.flags & PUNKTFUNK_FLAG_PROBE) {
            continue;
        }
        return m_Builder->build(frame.data, frame.len, frame.frame_index, frame.pts_ns, nowUs());
    }
}

bool PunktfunkSession::nextOpusPacket(quint32 timeoutMs, const uint8_t*& data, size_t& length,
                                      bool& closed)
{
    closed = false;
    if (m_Connection == nullptr) {
        closed = true;
        return false;
    }

    PunktfunkAudioPacket packet;
    memset(&packet, 0, sizeof(packet));
    int status = punktfunk_connection_next_audio(m_Connection, &packet, timeoutMs);
    if (status == PUNKTFUNK_STATUS_CLOSED) {
        closed = true;
        return false;
    }
    if (status != PUNKTFUNK_STATUS_OK) {
        return false;
    }
    data = packet.data;
    length = packet.len;
    return true;
}

void PunktfunkSession::requestKeyframe()
{
    if (m_Connection != nullptr) {
        punktfunk_connection_request_keyframe(m_Connection);
    }
}

void PunktfunkSession::quit()
{
    // Marks the close as deliberate; it does not free the handle, close() does.
    punktfunk_connection_disconnect_quit(m_Connection);
    close();
}

void PunktfunkSession::close()
{
    if (m_Connection != nullptr) {
        punktfunk_connection_close(m_Connection);
        m_Connection = nullptr;
    }
    m_Builder.reset();
}
