# Punktfunk in StreamLight

[Punktfunk](https://punktfunk.unom.io/) is a game-streaming host for Linux and Windows. It
speaks two protocols: GameStream (`serve --gamestream`, the one every Moonlight client uses)
and its own `punktfunk/1` (QUIC control, Leopard-RS FEC, AES-GCM data plane).

## What works today

**Streaming over GameStream.** A Punktfunk host with GameStream enabled is found over
`_nvstream._tcp`, paired and streamed like any Sunshine host. Nothing Punktfunk-specific runs
on the stream path.

**Recognition** (`ComputerManager::handlePunktfunkAdvert`). StreamLight also browses
`_punktfunk._udp`. The advert's TXT `id` is the same uniqueid Punktfunk's GameStream side
reports as serverinfo `uniqueid`, so the advert is matched to the saved host by uuid, never by
address. The serverinfo document is a Sunshine clone (same `appversion`, same state strings),
so it can't tell the two apart. On a match the host stores:

| Field | Source | Use |
|---|---|---|
| `punktfunkPort` | SRV port | native connect; non-zero marks the host as Punktfunk |
| `punktfunkFingerprint` | TXT `fp` | display only, see Security |
| `punktfunkPairing` | TXT `pair` | `required` or `optional` |

The host card shows a **Punktfunk** chip. Nothing in this path opens a connection, so a host
held asleep stays asleep. A Punktfunk host with no GameStream side is logged and not added.

## The native path: built, not wired

Opt-in at build time, off by default:

```
cargo build -p punktfunk-core --features quic --release
qmake CONFIG+=punktfunk PUNKTFUNK_DIR=C:\src\punktfunk
```

Ship `punktfunk_core.dll` beside `StreamLight.exe`. `build-release.ps1` does not do this yet.

| Piece | File | State |
|---|---|---|
| Access unit to `DECODE_UNIT` | `streaming/punktfunk/punktfunkdecodeunit.*` | Done, tested (`tests/punktfunk`) |
| Connection, pairing, probe, identity | `streaming/punktfunk/punktfunkconnection.*` | Done, compiles against ABI 39, not run against a host |
| Session integration | `streaming/session.cpp` | Not started |
| Input | `streaming/input/*` | Not started |
| Pairing UI | `gui/PairDialog.qml` | Not started |

`PunktfunkDecodeUnitBuilder` splits each access unit the way moonlight-common-c would have
delivered it: every H.264/HEVC parameter set in its own entry with its start code, because
`FFmpegVideoDecoder::writeBuffer()` rewrites the SPS in place and asserts the entry ends where
the NAL does. Keyframes are detected from the bitstream: parameter sets or IRAP slices for
H.264/HEVC, a sequence header OBU for AV1. PyroWave is never offered to the host, since no
decoder in StreamLight can take it.

## What is left, in order

1. **A transport seam in Session.** The stream is hard-wired to moonlight-common-c in three
   places, and each needs an indirection that the Punktfunk session can fill:
   - *Video.* On Windows the FFmpeg decoder is a pull renderer: its own thread calls
     `LiWaitForNextVideoFrame` / `LiCompleteVideoFrame` (`ffmpeg.cpp`). The Punktfunk path
     needs the same pull, backed by `PunktfunkSession::nextDecodeUnit()`, and `LiRequestIdrFrame`
     mapped to `requestKeyframe()`. Call `drSetup` with `videoFormat()` first, as
     `LiStartConnection` does.
   - *Audio.* `nextOpusPacket()` feeds `Session::arDecodeAndPlaySample` unchanged. Get the
     channel count from `punktfunk_connection_audio_channels` for `arInit`'s Opus config.
   - *Lifecycle.* `LiStartConnection` / `LiStopConnection` and the connection-listener
     callbacks (termination, connection status) become `connect()` / `quit()` plus a watcher
     on `closed`.
2. **Input.** About 40 `LiSend*` calls across `input/*.cpp` and `session.cpp`. Route them
   through one interface with a moonlight-common-c implementation and a Punktfunk one
   (`punktfunk_connection_send_input` / `send_rich_input`). Keyboard, mouse and gamepad first;
   motion, touch and pen later.
3. **Pairing and choice of path.** A PIN dialog calling `PunktfunkSession::pair()`, which
   stores the verified fingerprint on the host. Then a per-host "Use Punktfunk protocol"
   switch, shown only when `punktfunkPort != 0`, that falls back to GameStream on any connect
   failure.
4. **Release build.** Add the cargo step and the DLL copy to `build-release.ps1`, and pin the
   Punktfunk revision. `PunktfunkSession::libraryCompatible()` must pass before anything else
   runs, because the ABI changes often (v39 at the time of writing).

## Security

- The mDNS `fp` is **unauthenticated**: anyone on the LAN can advertise any value. It is kept
  for display only. The pin a connect presents comes from `punktfunk_pair()`, which verifies
  it against the PIN.
- `PunktfunkSession::connect()` refuses an empty pin. The library would treat that as
  trust-on-first-use.
- The client identity (certificate and key PEM) lives in QSettings under `punktfunk/`, the
  same way and the same place as the Moonlight identity.
- Punktfunk documents its GameStream mode as trusted-LAN only.
