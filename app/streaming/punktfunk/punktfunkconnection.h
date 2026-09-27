#pragma once

// Built only with CONFIG+=punktfunk (6.4.0). See docs/punktfunk.md for what is wired and what
// is not yet.

#include "punktfunkdecodeunit.h"

#include <QByteArray>
#include <QString>

#include <memory>

struct PunktfunkConnection;

/**
 * One punktfunk/1 session with a host, over punktfunk-core's C ABI.
 *
 * Owns the connection handle and closes it on destruction. Threading follows the library:
 * video, audio and input are independent planes, each pulled or pushed from at most one thread
 * at a time, and they may run concurrently. Every call that blocks is bounded by a timeout.
 *
 * The client identity (a self-signed certificate the host recognises this device by) is created
 * once and kept in QSettings beside the Moonlight one, which is stored the same way.
 */
class PunktfunkSession
{
public:
    struct Params
    {
        QString host;
        quint16 port = 0;
        // The host certificate's SHA-256 as returned by pair(). Empty is trust-on-first-use,
        // which this client never asks for: see connect().
        QByteArray pinSha256;
        int width = 0;
        int height = 0;
        int refreshHz = 0;
        int bitrateKbps = 0;
        // VIDEO_FORMAT_* mask of what this machine's decoder can take. PyroWave is never
        // offered: nothing in StreamLight decodes it.
        int supportedVideoFormats = 0;
        int preferredVideoFormat = 0;
        int audioChannels = 2;
        QString deviceName;
        quint32 timeoutMs = 10000;
    };

    // False when the loaded punktfunk_core library was built for a different ABI. Must be
    // checked before anything else in this class is used.
    static bool libraryCompatible();

    static QString statusText(int status);

    // The persisted identity, created on first use. False only if generation failed.
    static bool clientIdentity(QByteArray& certPem, QByteArray& keyPem);

    // PIN pairing: the host shows a PIN, the user types it here. On success `hostSha256` is the
    // verified host fingerprint, the pin every later connect must present.
    static int pair(const QString& host, quint16 port, const QString& pin,
                    const QString& deviceName, QByteArray& hostSha256);

    // Is something at host:port answering, and is it the host we pinned? Blocks up to
    // `timeoutMs`; never on the UI thread.
    static bool probe(const QString& host, quint16 port, const QByteArray& pinSha256,
                      quint32 timeoutMs);

    PunktfunkSession() = default;
    ~PunktfunkSession();

    PunktfunkSession(const PunktfunkSession&) = delete;
    PunktfunkSession& operator=(const PunktfunkSession&) = delete;

    // Returns PUNKTFUNK_STATUS_OK (0) or the failure. Refuses an empty pin: an unpinned
    // connect trusts whoever answers on that address.
    int connect(const Params& params);

    // The format the host settled on, as VIDEO_FORMAT_*. Valid after connect().
    int videoFormat() const { return m_VideoFormat; }

    // The next access unit as a DECODE_UNIT, or null on timeout / end. The unit borrows the
    // library's buffer until the next call. `closed` is set once the session has ended.
    PDECODE_UNIT nextDecodeUnit(quint32 timeoutMs, bool& closed);

    // The next Opus packet (the stream's default audio), or false on timeout / end. The data
    // borrows until the next call.
    bool nextOpusPacket(quint32 timeoutMs, const uint8_t*& data, size_t& length, bool& closed);

    void requestKeyframe();

    // Ends the session as a user stop: the host drops it at once instead of holding it open
    // for a reconnect.
    void quit();

    // Ends the session as a drop: the host keeps it open briefly for a reconnect.
    void close();

private:
    PunktfunkConnection* m_Connection = nullptr;
    int m_VideoFormat = 0;
    std::unique_ptr<PunktfunkDecodeUnitBuilder> m_Builder;
};
