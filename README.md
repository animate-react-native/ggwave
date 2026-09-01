![@animatereactnative/ggwave - data over sound for React Native.](https://raw.githubusercontent.com/animate-react-native/ggwave/main/docs/banner.png)

Data over sound for React Native. A [Nitro](https://nitro.margelo.com) module
wrapping [ggwave](https://github.com/ggerganov/ggwave), so two phones can pass a
few bytes to each other through the air with no network, no pairing and no
account.

Implemented in C++ once for both platforms. ggwave is a codec rather than a
modem, which is the whole reason this is a small module: it has no audio I/O and
no external dependencies, so it vendors as a single translation unit.

> **Status: runs on iOS and Android. Not yet measured across a room.**
>
> The codec passes 12 checks on a device. The modem has carried real chirps: one
> phone speaker to its own microphone, and an iOS simulator to a physical iPhone,
> both decoded. Off device, 48 checks cover the text codec, the vendored codec
> and the modem through a loopback backend.
>
> What is still unknown is range. Nobody has measured success rate at 30 cm and
> at 1 m, in a quiet room and a noisy one, phones flat and phones propped. Until
> that exists, treat the ultrasound protocols in particular as unproven: 15 kHz
> is exactly where phone speakers roll off hardest, which is why `AUDIBLE_FAST`
> is the default.

## Install

Not published yet, so it is consumed from this repo.

```sh
bun add @animatereactnative/ggwave react-native-nitro-modules
```

Requires a dev client or a bare build. Nothing here can run in Expo Go.

## Use

```typescript
import { ggwave, Protocol } from '@animatereactnative/ggwave'

// A Chalk move: three bytes.
const move = new Uint8Array([0x2a, 0x01, 0xc3])

// Waveform bytes, F32 samples, ready to hand to an audio API.
const waveform = ggwave.encode(move.buffer, Protocol.ULTRASOUND_FAST, 25)

// Feed captured audio back a frame at a time.
const payload = ggwave.decode(frame)
if (payload != null) {
  console.log(new Uint8Array(payload))
}
```

`volume` is 0 to 100. `Protocol` mirrors ggwave's enum, and the ids are asserted
against the header at compile time, so they cannot drift.

Three things about the codec that are easy to get wrong:

- **One transmission decodes several times**, because the payload stays inside
  ggwave's analysis window as it slides. Dedupe on a sequence number rather than
  treating every decode as a new message.
- **`encode` narrows reception to the protocol you pass it.** With all twelve of
  ggwave's protocols listening, a 48 byte payload does not decode at all. So a
  `decode` picks up where the last `encode` left off, and switching protocol
  rebuilds the instance underneath.
- **A payload is at most 140 bytes.** `encode` throws above that. ggwave's
  `kMaxDataSize` of 256 is the decode buffer, not a transmit limit.

### Text, if bytes are not what you have

```typescript
import { encodeText, decodeText } from '@animatereactnative/ggwave'

const buffer = encodeText('e4', 6) // zero padded to exactly 6 bytes
decodeText(buffer) // 'e4', padding trimmed
```

UTF-8, emoji included, hand rolled rather than through `TextEncoder`, which
React Native ships no types for. Text longer than the payload is **refused, not
truncated**: a silently shortened message is worse than an error.

## The modem

```typescript
import { ggwave, startModem, Protocol } from '@animatereactnative/ggwave'

ggwave.onMessage = (payload: ArrayBuffer) => {
  console.log('heard', new Uint8Array(payload))
}

startModem({ protocol: Protocol.AUDIBLE_FAST, payloadBytes: 3 })
console.log(ggwave.route) // what the audio layer actually opened

ggwave.send(move, 50) // volume is 0 to 100

ggwave.stop() // always, or the microphone stays open
```

Every option, all of them optional:

|                        |                                                                                                                                                                              |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `protocol`             | Defaults to `AUDIBLE_FAST`                                                                                                                                                   |
| `payloadBytes`         | Exact size of every message, or the largest one when variable. Defaults to 6                                                                                                 |
| `variableLength`       | See below. Defaults to false, which is the fast one                                                                                                                          |
| `soundMarkerThreshold` | How loud a marker must be to count, 0 to 1. Left to ggwave by default. Worth lowering when reception is poor, because an unprocessed input path runs quiet on both platforms |

`startModem` is a wrapper over `ggwave.start`, which takes the same four values
positionally. Prefer the named form: a fifth knob should not change what the
fourth argument means.

A phone does not hear its own chirp: `capture` is gated while a transmission is
going out. To test with one phone, pointing its speaker at its own microphone,
set `ggwave.allowSelfReception = true` before `start`. Nothing else should.

**Permissions.** The library declares `RECORD_AUDIO` in its own manifest, but
Android still needs the runtime request (`PermissionsAndroid`) before `start`.
On iOS the app needs `NSMicrophoneUsageDescription` in its `Info.plist`, or
`start` throws when the session refuses to activate.

**Interruptions are not handled yet.** A call arriving, headphones going in or
Bluetooth taking the route will stop the stream, and `route` will say so on
Android, but the modem does not currently recover on its own. Call `stop` then
`start` again.

## Protocols

Do not write the table down. Ask for it:

```typescript
import { ggwave, protocolInfo, Protocol } from '@animatereactnative/ggwave'

ggwave.protocols
// [{ id: 0, name: 'Normal', freqStartHz: 1875, bandwidthHz: 4500,
//    framesPerTx: 9, bytesPerTx: 3, tones: 6, chunkMs: 192 }, ...]

protocolInfo(Protocol.ULTRASOUND_FAST)?.freqStartHz // 15000

// Exactly how long a message takes, measured by sizing the real waveform
// rather than estimated, because Reed Solomon adds chunks no formula predicts.
ggwave.durationMs(Protocol.AUDIBLE_FAST, 3, false) // 256
ggwave.durationMs(Protocol.AUDIBLE_FAST, 48, false) // 3627
```

Everything above comes from ggwave's own table, so it cannot drift from the C.
Only the ids are declared in TypeScript, and the C asserts them at compile time.

The audible and ultrasound protocols carry three bytes a chunk over 4500 Hz; the
DT and MT ones carry one byte over 1500 Hz and are correspondingly slower. The
ultrasound band sits at 15.0 to 19.5 kHz, which is not silent: children hear it,
and so do plenty of adults.

If a device's speaker cannot reach that band, move it:

```typescript
ggwave.setFreqStart(Protocol.ULTRASOUND_FAST, 12000) // before start()
```

## How much can one message carry

```typescript
ggwave.maxPayloadFixed // 64
ggwave.maxPayloadVariable // 140
ggwave.maxInstances // 4, process wide, and the modem holds two
```

Airtime is charged per transmission, not per byte: a modem started for 64 bytes
costs 64 bytes of chirping to send one. So keep `payloadBytes` at what you
actually send.

Fixed length is the default and much the faster: ggwave can then skip the sound
markers it otherwise needs to find where a message begins, which is **256 ms
against 1067 ms** for three bytes. Variable length buys a higher ceiling and
messages that need not all be the same size:

```typescript
startModem({
  protocol: Protocol.AUDIBLE_FAST,
  payloadBytes: 120,
  variableLength: true,
})
```

Above 140 bytes there is no mechanism here, and that is deliberate. Splitting a
message into chunks needs ordering, loss detection, retransmission and
reassembly, which is a transport protocol; it belongs where the application
knows what a missing piece means. Sound is also a broadcast with no sender
identity, so two phones chirping in one room interleave and reassembly has
nothing to sort by.

## Development

```sh
bun install
bun run check        # everything below except the example app
bun run specs        # nitrogen codegen, after editing src/specs/*.nitro.ts
bun run typecheck
bun run lint
bun run test:text    # the text codec, pure TypeScript
bun run test:host    # the vendored codec, compiled and round tripped
bun run test:modem   # the modem through a loopback backend
bun run example:ios  # the example app, which needs pod install first
```

## Releasing

```sh
bun release          # patch
bun release minor
bun release 1.0.0    # or an exact version
```

That bumps `package.json`, commits it as `chore: release …`, tags it
`ggwave-v<version>`, publishes to npm, and then snapshots the source to
[animate-react-native/ggwave](https://github.com/animate-react-native/ggwave)
with a matching tag. Run it plain and release-it offers the increments with the
resulting numbers to pick from.

It runs from this checkout and refuses to run from a clone of the library repo,
which is the opposite of what a published package usually wants. The library
repo is a **snapshot**, not a subtree: every push replaces its tree with this
one, so a version bump or a CHANGELOG written over there lasts until the next
push and the version that actually ships never moves. The version lives here.

The example app in `example/` resolves this library through `metro.config.js`,
`react-native.config.js` and `tsconfig.json` rather than a `package.json`
dependency, because bun copies relative file dependencies instead of linking
them. `bunx react-native config` inside `example/` is the quickest check that
autolinking still sees the library.

`nitrogen/generated/` is regenerated wholesale by `bun run specs`. Never edit it.

ggwave itself lives in `cpp/vendor/ggwave`, copied rather than forked, with the
upstream commit recorded in `cpp/vendor/ggwave/UPSTREAM.md`.

## License

[MIT](./LICENSE). Vendored ggwave is MIT (`cpp/vendor/ggwave/LICENSE`), and the
Reed Solomon implementation inside it carries its own
(`cpp/vendor/ggwave/src/reed-solomon/LICENSE`).

---

<p align="center">
  <a href="https://www.animatereactnative.com">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://www.animatereactnative.com/animatereactnative_dark.svg">
      <img alt="AnimateReactNative.com - Premium and Custom React Native animations." src="https://www.animatereactnative.com/animatereactnative_logo.svg" height="34" align="middle">
    </picture>
  </a>
  &nbsp;&nbsp;&nbsp;<b>&times;</b>&nbsp;&nbsp;&nbsp;
  <a href="https://keyframer.dev">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://keyframer.dev/logo-dark.png">
      <img alt="Keyframer.dev - design and ship React Native animations." src="https://keyframer.dev/logo-light.png" height="24" align="middle">
    </picture>
  </a>
</p>
