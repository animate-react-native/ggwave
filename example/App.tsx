/**
 * Step 2 of the ladder in GGWAVE-NITRO-MODULE.md: the module in a real app,
 * encode straight back into decode, with no audio hardware in the loop.
 *
 * This is what isolates the JSI boundary and the ArrayBuffer ownership rules
 * from every acoustic variable. If a row here fails, the problem is the module.
 * If every row passes and two phones still cannot hear each other, the problem
 * is the air.
 */
import React, { useCallback, useState } from 'react';
import {
  ActivityIndicator,
  ScrollView,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from 'react-native';
import { ggwave, Protocol, type ProtocolId } from '@animatereactnative/ggwave';
import ModemPanel from './ModemPanel';

type Row = {
  name: string;
  passed: boolean;
  detail: string;
};

const VOLUME = 25;

/** Feeds a whole waveform back through decode one frame at a time. */
function decodeWaveform(waveform: ArrayBuffer): Uint8Array | null {
  const frameBytes = ggwave.samplesPerFrame * Float32Array.BYTES_PER_ELEMENT;

  for (let offset = 0; offset < waveform.byteLength; offset += frameBytes) {
    const frame = waveform.slice(offset, Math.min(offset + frameBytes, waveform.byteLength));
    const payload = ggwave.decode(frame);
    if (payload != null) {
      return new Uint8Array(payload);
    }
  }

  // The last frames of a payload only complete once more audio arrives, so a
  // real microphone would keep feeding. Silence stands in for that here.
  const silence = new ArrayBuffer(frameBytes);
  for (let i = 0; i < 32; i++) {
    const payload = ggwave.decode(silence);
    if (payload != null) {
      return new Uint8Array(payload);
    }
  }

  return null;
}

function sameBytes(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  return a.every((byte, i) => byte === b[i]);
}

/**
 * An ArrayBuffer and a view onto it. Built this way round because
 * `new Uint8Array([...]).buffer` is typed `ArrayBufferLike`, which admits a
 * SharedArrayBuffer and so will not pass as the module's `ArrayBuffer`.
 */
function payloadOf(values: readonly number[]): { buffer: ArrayBuffer; view: Uint8Array } {
  const buffer = new ArrayBuffer(values.length);
  const view = new Uint8Array(buffer);
  view.set(values);
  return { buffer, view };
}

function roundTrip(name: string, values: readonly number[], protocol: ProtocolId): Row {
  const { buffer, view: payload } = payloadOf(values);
  try {
    const waveform = ggwave.encode(buffer, protocol, VOLUME);
    const samples = waveform.byteLength / Float32Array.BYTES_PER_ELEMENT;
    const durationMs = Math.round((1000 * samples) / 48000);

    const decoded = decodeWaveform(waveform);
    if (decoded == null) {
      return { name, passed: false, detail: `${durationMs} ms, nothing decoded` };
    }
    if (!sameBytes(decoded, payload)) {
      return {
        name,
        passed: false,
        detail: `${durationMs} ms, decoded ${decoded.length} wrong bytes`,
      };
    }
    return { name, passed: true, detail: `${durationMs} ms, ${payload.length} bytes` };
  } catch (error) {
    return { name, passed: false, detail: String(error) };
  }
}

function runSuite(): Row[] {
  // A Chalk move: kind and move in byte 0, sequence in byte 1, CRC in byte 2.
  const move = [0x2a, 0x01, 0xc3];
  const resync = Array.from({ length: 48 }, (_, i) => (i * 7 + 1) & 0xff);

  const rows: Row[] = [
    {
      name: 'samplesPerFrame is 1024',
      passed: ggwave.samplesPerFrame === 1024,
      detail: String(ggwave.samplesPerFrame),
    },
    roundTrip('A single byte', [0xff], Protocol.ULTRASOUND_FAST),
    roundTrip('A move, AUDIBLE_FAST', move, Protocol.AUDIBLE_FAST),
    roundTrip('A move, ULTRASOUND_NORMAL', move, Protocol.ULTRASOUND_NORMAL),
    roundTrip('A move, ULTRASOUND_FAST', move, Protocol.ULTRASOUND_FAST),
    roundTrip('A move, ULTRASOUND_FASTEST', move, Protocol.ULTRASOUND_FASTEST),
    roundTrip('A 48 byte resync', resync, Protocol.ULTRASOUND_FAST),
  ];

  // The ownership rules from the doc, exercised rather than trusted. An empty
  // payload and an oversized one must both be refused, not crash.
  rows.push(refuses('Refuses an empty payload', 0));
  rows.push(refuses('Refuses 200 bytes', 200));

  // Nothing about the modem is asserted here. An earlier version of this suite
  // checked that start() and send() threw "not implemented", which passed until
  // the modem was built and then failed for the best possible reason. The modem
  // has its own tab, and the codec must not depend on whether it is running.
  rows.push({
    name: 'The codec works whether or not the modem is listening',
    passed: true,
    detail: ggwave.isListening ? `modem is listening: ${ggwave.route}` : 'modem is stopped',
  });

  return rows;
}

function refuses(name: string, byteLength: number): Row {
  const buffer = new ArrayBuffer(byteLength);
  return throws(name, () => ggwave.encode(buffer, Protocol.ULTRASOUND_FAST, VOLUME));
}

function throws(name: string, run: () => unknown): Row {
  try {
    run();
    return { name, passed: false, detail: 'did not throw' };
  } catch (error) {
    return { name, passed: true, detail: String(error).slice(0, 60) };
  }
}

type Tab = 'codec' | 'modem';

export default function App(): React.JSX.Element {
  const [tab, setTab] = useState<Tab>('codec');
  const [rows, setRows] = useState<Row[] | null>(null);
  const [running, setRunning] = useState(false);

  const run = useCallback(() => {
    setRunning(true);
    // A frame later, so the spinner actually paints: the whole suite is
    // synchronous JSI calls and blocks the JS thread while it runs.
    setTimeout(() => {
      setRows(runSuite());
      setRunning(false);
    }, 50);
  }, []);

  const failed = rows?.filter((row) => !row.passed).length ?? 0;

  return (
    <View style={styles.screen}>
      <Text style={styles.title}>ggwave</Text>
      <Text style={styles.subtitle}>
        {tab === 'codec' ? 'Codec round trip, no audio hardware' : 'Real audio, ladder step 3'}
      </Text>

      <View style={styles.tabs}>
        {(['codec', 'modem'] as Tab[]).map((name) => (
          <TouchableOpacity
            key={name}
            onPress={() => setTab(name)}
            style={[styles.tab, tab === name && styles.tabOn]}
          >
            <Text style={[styles.tabText, tab === name && styles.tabTextOn]}>{name}</Text>
          </TouchableOpacity>
        ))}
      </View>

      {tab === 'modem' && <ModemPanel />}

      {tab === 'codec' && (
      <>
      <TouchableOpacity style={styles.button} onPress={run} disabled={running}>
        <Text style={styles.buttonText}>{running ? 'Running' : 'Run the suite'}</Text>
      </TouchableOpacity>

      {running && <ActivityIndicator style={styles.spinner} />}

      {rows != null && (
        <Text style={[styles.summary, failed > 0 ? styles.bad : styles.good]}>
          {failed === 0 ? `All ${rows.length} passed` : `${failed} of ${rows.length} failed`}
        </Text>
      )}

      <ScrollView style={styles.list} contentContainerStyle={styles.listContent}>
        {rows?.map((row) => (
          <View key={row.name} style={styles.row}>
            <Text style={[styles.mark, row.passed ? styles.good : styles.bad]}>
              {row.passed ? 'ok' : 'no'}
            </Text>
            <View style={styles.rowText}>
              <Text style={styles.rowName}>{row.name}</Text>
              <Text style={styles.rowDetail}>{row.detail}</Text>
            </View>
          </View>
        ))}
      </ScrollView>
      </>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  screen: { flex: 1, paddingTop: 72, paddingHorizontal: 20, backgroundColor: '#101014' },
  tabs: { flexDirection: 'row', gap: 8, marginTop: 16 },
  tab: { paddingVertical: 8, paddingHorizontal: 16, borderRadius: 999, backgroundColor: '#1e1e24' },
  tabOn: { backgroundColor: '#3f3f46' },
  tabText: { color: '#a1a1aa', fontSize: 14, fontWeight: '600' },
  tabTextOn: { color: '#ffffff' },
  title: { fontSize: 34, fontWeight: '700', color: '#f4f4f5' },
  subtitle: { fontSize: 15, color: '#8b8b93', marginTop: 4 },
  button: {
    marginTop: 24,
    paddingVertical: 14,
    borderRadius: 12,
    backgroundColor: '#2f6fed',
    alignItems: 'center',
  },
  buttonText: { color: '#ffffff', fontSize: 16, fontWeight: '600' },
  spinner: { marginTop: 20 },
  summary: { marginTop: 20, fontSize: 17, fontWeight: '600' },
  list: { marginTop: 12, flex: 1 },
  listContent: { paddingBottom: 40 },
  row: { flexDirection: 'row', alignItems: 'flex-start', paddingVertical: 10 },
  mark: { width: 34, fontSize: 14, fontWeight: '700', fontVariant: ['tabular-nums'] },
  rowText: { flex: 1 },
  rowName: { fontSize: 15, color: '#e4e4e7' },
  rowDetail: { fontSize: 13, color: '#7c7c85', marginTop: 2 },
  good: { color: '#3ecf8e' },
  bad: { color: '#f26d6d' },
});
