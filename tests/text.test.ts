// The text codec, off device. No native module, no platform globals.
// Run: bun run test:text
import { decodeText, encodeText } from '../src/text.ts';

let failures = 0;

function check(ok: boolean, what: string) {
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${what}`);
  if (!ok) failures++;
}

function roundTrips(text: string, payloadLength: number) {
  const decoded = decodeText(encodeText(text, payloadLength));
  check(decoded === text, `"${text}" round trips in ${payloadLength} bytes (got "${decoded}")`);
}

console.log('text over a fixed length byte channel\n');

roundTrips('', 3);
roundTrips('a', 3);
roundTrips('abc', 3);
roundTrips('e4', 8);
roundTrips('hello world', 16);
// Two, three and four bytes per code point.
roundTrips('é', 8);
roundTrips('€', 8);
roundTrips('🎲', 8);
roundTrips('a🎲z', 16);

console.log('\nThe payload is always exactly the length the modem was started for:');
check(encodeText('a', 3).byteLength === 3, 'one character still fills 3 bytes');
check(encodeText('', 64).byteLength === 64, 'an empty string still fills 64 bytes');

console.log('\nOversized text is refused, never truncated:');
const tooBig: [string, number][] = [
  ['abcd', 3],
  ['🎲', 3],
  ['ab', 1],
];
for (const [text, length] of tooBig) {
  try {
    encodeText(text, length);
    check(false, `"${text}" in ${length} bytes throws`);
  } catch {
    check(true, `"${text}" in ${length} bytes throws`);
  }
}

console.log('\nPadding is trimmed rather than delivered:');
const padded = encodeText('ab', 8);
check(decodeText(padded) === 'ab', 'padding is trimmed');
check(decodeText(padded).length === 2, 'no stray NUL survives the trip');

console.log(`\n${failures === 0 ? 'all passed' : 'FAILURES'}`);
process.exit(failures === 0 ? 0 : 1);
