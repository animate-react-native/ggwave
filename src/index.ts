import { NitroModules } from 'react-native-nitro-modules'
import type { Ggwave, ProtocolInfo } from './specs/Ggwave.nitro'

/**
 * The protocol ids.
 *
 * Only the ids are written here, and the C asserts them at compile time. Every
 * other fact about a protocol, including its name, its band and its timing,
 * comes from `ggwave.protocols`, which reads ggwave's own table. Two copies of
 * that would drift the first time upstream changed one.
 */
export const Protocol = {
  AUDIBLE_NORMAL: 0,
  AUDIBLE_FAST: 1,
  AUDIBLE_FASTEST: 2,
  ULTRASOUND_NORMAL: 3,
  ULTRASOUND_FAST: 4,
  ULTRASOUND_FASTEST: 5,
  DT_NORMAL: 6,
  DT_FAST: 7,
  DT_FASTEST: 8,
  MT_NORMAL: 9,
  MT_FAST: 10,
  MT_FASTEST: 11,
} as const

export type ProtocolId = (typeof Protocol)[keyof typeof Protocol]

export const ggwave = NitroModules.createHybridObject<Ggwave>('Ggwave')
export type { Ggwave, ProtocolInfo }

/** Looks a protocol up in ggwave's own table. */
export function protocolInfo(id: ProtocolId): ProtocolInfo | undefined {
  return ggwave.protocols.find((p) => p.id === id)
}

export type ModemOptions = {
  /** Defaults to AUDIBLE_FAST: see the note on why audible is not a compromise. */
  protocol?: ProtocolId
  /**
   * Bytes per message, or the largest message when `variableLength` is set.
   * Every transmission costs the airtime of this many bytes, so keep it small.
   */
  payloadBytes?: number
  /**
   * Allow shorter messages, and a ceiling of `ggwave.maxPayloadVariable` rather
   * than `ggwave.maxPayloadFixed`. Roughly four times slower for short
   * payloads, because ggwave then has to mark where a message begins and ends.
   */
  variableLength?: boolean
  /**
   * How loud a marker must be to count, 0 to 1. Left to ggwave by default.
   * Worth lowering if reception is poor: both platforms warn that an
   * unprocessed input path runs quiet.
   */
  soundMarkerThreshold?: number
}

/**
 * Starts the modem, with names instead of four positional numbers.
 *
 * `ggwave.start` is the same call underneath; this exists so adding a knob does
 * not silently change what an existing argument means.
 */
export function startModem({
  protocol = Protocol.AUDIBLE_FAST,
  payloadBytes = 6,
  variableLength = false,
  soundMarkerThreshold = 0,
}: ModemOptions = {}): void {
  ggwave.start(protocol, payloadBytes, variableLength, soundMarkerThreshold)
}

export { decodeText, encodeText } from './text'
