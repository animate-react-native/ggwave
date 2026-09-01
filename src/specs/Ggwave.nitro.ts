import { type HybridObject } from 'react-native-nitro-modules'

/**
 * One of ggwave's protocols, as ggwave itself describes it.
 *
 * Read from `GGWave::Protocols::tx()` rather than written out here, so the ids,
 * the names and the timings cannot drift from the C they come from.
 */
export interface ProtocolInfo {
  id: number
  /** ggwave's own name, such as "[U] Fast". */
  name: string
  /**
   * Lowest frequency of the carrier, in Hz.
   *
   * ggwave stores this as an FFT bin index; it is converted here because a bin
   * index is meaningless without also knowing the sample rate and frame size.
   */
  freqStartHz: number
  /** How wide the carrier is, in Hz. Ninety six bins above `freqStartHz`. */
  bandwidthHz: number
  framesPerTx: number
  bytesPerTx: number
  /** How many simultaneous tones a chunk uses. */
  tones: number
  /** How long one chunk takes on the air, in milliseconds. */
  chunkMs: number
}

/**
 * Data over sound.
 *
 * Two layers over one C++ implementation. The modem owns the microphone and the
 * speaker, so nothing crosses the JS boundary per audio frame, only completed
 * messages. The codec is exposed as well, so the tests can run with no audio
 * hardware in the loop at all.
 *
 * See GGWAVE-NITRO-MODULE.md for why this is C++ on both platforms, and for the
 * ArrayBuffer ownership rules that make every method here synchronous.
 */
export interface Ggwave
  extends HybridObject<{ ios: 'c++'; android: 'c++' }> {
  // ── The modem.

  /**
   * Opens the audio streams, then creates the ggwave instance, in that order:
   * `sampleRateInp` and `sampleRateOut` are not known until the streams report
   * what they actually opened with.
   *
   * @param protocolId one of the `Protocol` values
   * @param payloadLength in bytes. The exact size of every message when
   * `variableLength` is false, or the largest one you will send when it is true.
   * The transmit buffers are sized from it either way.
   * @param variableLength false transmits a fixed size, which is what you want:
   * ggwave can then skip the sound markers it needs to find a boundary, and a
   * three byte move takes 256 ms rather than 1067 ms. True allows shorter
   * messages and a larger ceiling, at that cost.
   * @param soundMarkerThreshold how loud a marker must be to count, 0 to 1.
   * Pass 0 for ggwave's default. Worth lowering on an unprocessed input path,
   * which both platforms warn runs quiet.
   */
  start(
    protocolId: number,
    payloadLength: number,
    variableLength: boolean,
    soundMarkerThreshold: number,
  ): void

  /** Closes the streams and frees the ggwave instance. Safe to call twice. */
  stop(): void

  /**
   * Queues a payload for transmission.
   *
   * The buffer arrives non owning, and the audio thread reads it later, so it
   * is copied into the module before this returns. It is never retained.
   */
  send(payload: ArrayBuffer, volume: number): void

  /**
   * Called on the JS thread when a payload completes.
   *
   * Never called from the audio thread: a decoded payload goes onto a lock free
   * queue and is dispatched from there.
   */
  onMessage: ((payload: ArrayBuffer) => void) | undefined

  readonly isListening: boolean

  /**
   * Lets the modem decode its own transmission. False by default, because
   * hearing your own chirp is how a phone plays its own move twice.
   *
   * Exists for step 3 of the ladder, one phone with its speaker pointed at its
   * own microphone, which cannot be tested at all while the gate is closed. Not
   * something an app should turn on.
   */
  allowSelfReception: boolean

  /**
   * What the audio layer actually opened: the API, the real sample rates, and on
   * Android whether the unprocessed input preset was granted or quietly refused.
   * Reads "not started" until `start`.
   */
  readonly route: string

  // ── The codec. Same C++ underneath, no audio involved.

  /** Returns an owning buffer of waveform bytes, so JS may hold it. */
  encode(payload: ArrayBuffer, protocolId: number, volume: number): ArrayBuffer

  /**
   * Decodes one frame. Synchronous on purpose: `samples` arrives non owning and
   * is only valid until this returns. Never make this a promise.
   */
  decode(samples: ArrayBuffer): ArrayBuffer | undefined

  readonly samplesPerFrame: number

  // ── What ggwave can do, asked of ggwave rather than written down twice.

  /** Every protocol, with its band and its timing. */
  readonly protocols: ProtocolInfo[]

  /** The most a single message may carry, in bytes, in each of the two modes. */
  readonly maxPayloadFixed: number
  readonly maxPayloadVariable: number

  /**
   * How many ggwave instances may exist at once, process wide.
   *
   * A hard limit in the C, not a guideline: the modem holds two and the codec
   * one, so a second Ggwave object would fail to start.
   */
  readonly maxInstances: number

  /**
   * Exactly how long transmitting `payloadBytes` would take, in milliseconds.
   *
   * Measured by asking the encoder to size the waveform, not estimated from the
   * chunk count, because Reed Solomon adds chunks that no formula here would
   * predict. Has no effect on a running modem: it builds and frees its own
   * instance, so it neither disturbs reception nor changes which protocol the
   * codec is listening for.
   */
  durationMs(protocolId: number, payloadBytes: number, variableLength: boolean): number

  /**
   * Moves a protocol's carrier, for both transmitting and receiving.
   *
   * The escape hatch for a device whose speaker cannot reach the ultrasound
   * band: shifting down trades inaudibility for something that actually carries.
   * Applies to instances created afterwards, so call it before `start`.
   */
  setFreqStart(protocolId: number, freqStartHz: number): void
}
