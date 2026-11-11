/**
 * Text on a byte channel, with no native code and no platform globals, so it can
 * be tested off device.
 */

/**
 * Text over the modem, for callers who would rather not think in bytes.
 *
 * Thin on purpose. The byte API is the real one: a modem is started for a fixed
 * payload length, so every message is exactly that many bytes, and text has to
 * be padded to fit. Anything longer is refused rather than truncated, because a
 * silently shortened move is worse than an error.
 *
 * `payloadLength` is what was passed to `ggwave.start`, and it caps at 64. So
 * this is for short strings: a move, a seat, a round number. Not a game state.
 */
export function encodeText(text: string, payloadLength: number): ArrayBuffer {
  const utf8 = utf8Encode(text)
  if (utf8.byteLength > payloadLength) {
    throw new Error(
      `"${text}" is ${utf8.byteLength} bytes as UTF-8, which does not fit a ${payloadLength} byte payload`,
    )
  }
  // Zero padded, so the length on the wire is always the length the modem was
  // started for. Zero is not valid UTF-8 continuation data, which is what makes
  // decodeText able to find the end again.
  const buffer = new ArrayBuffer(payloadLength)
  new Uint8Array(buffer).set(utf8)
  return buffer
}

/** The other half of `encodeText`: trims the zero padding and decodes UTF-8. */
export function decodeText(payload: ArrayBuffer): string {
  const bytes = new Uint8Array(payload)
  let end = bytes.length
  while (end > 0 && bytes[end - 1] === 0) end--
  return utf8Decode(bytes.subarray(0, end))
}

/*
 * UTF-8 by hand, rather than through TextEncoder.
 *
 * React Native ships no types for TextEncoder and no polyfill that could be
 * found in the installed packages, and a payload here is at most 64 bytes, so
 * depending on a global that may or may not exist buys nothing. `for..of` walks
 * code points rather than UTF-16 units, so an emoji encodes as one four byte
 * sequence instead of two broken halves.
 */
function utf8Encode(text: string): Uint8Array {
  const bytes: number[] = []
  for (const character of text) {
    const code = character.codePointAt(0) ?? 0
    if (code < 0x80) {
      bytes.push(code)
    } else if (code < 0x800) {
      bytes.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f))
    } else if (code < 0x10000) {
      bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f))
    } else {
      bytes.push(
        0xf0 | (code >> 18),
        0x80 | ((code >> 12) & 0x3f),
        0x80 | ((code >> 6) & 0x3f),
        0x80 | (code & 0x3f),
      )
    }
  }
  return new Uint8Array(bytes)
}

function utf8Decode(bytes: Uint8Array): string {
  let text = ''
  for (let i = 0; i < bytes.length; ) {
    const first = bytes[i] ?? 0
    let code: number
    let width: number
    if (first < 0x80) {
      code = first
      width = 1
    } else if (first < 0xe0) {
      code = first & 0x1f
      width = 2
    } else if (first < 0xf0) {
      code = first & 0x0f
      width = 3
    } else {
      code = first & 0x07
      width = 4
    }
    // A sequence cut off by the padding trim is dropped rather than guessed at.
    if (i + width > bytes.length) break
    for (let k = 1; k < width; k++) {
      code = (code << 6) | ((bytes[i + k] ?? 0) & 0x3f)
    }
    text += String.fromCodePoint(code)
    i += width
  }
  return text
}
