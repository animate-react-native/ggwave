/**
 * Step 3 of the ladder in GGWAVE-NITRO-MODULE.md: real audio.
 *
 * On one phone, turn on "hear myself" and send: the chirp leaves the speaker,
 * comes back through the microphone, and should arrive as a message. That is the
 * first point at which the block size, the sample rate negotiation and the
 * unprocessed input preset are all actually in play.
 *
 * On two phones, leave "hear myself" off and send from one. Whatever arrives
 * came through the air.
 */
import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  PermissionsAndroid,
  Platform,
  ScrollView,
  StyleSheet,
  Switch,
  Text,
  TouchableOpacity,
  View,
} from 'react-native';
import { ggwave, Protocol, startModem, type ProtocolId } from '@animatereactnative/ggwave';

const VOLUME = 50;
const PAYLOAD_LENGTH = 3;

type Entry = { at: string; text: string; kind: 'in' | 'out' | 'note' | 'bad' };

async function ensureMicrophone(): Promise<boolean> {
  if (Platform.OS !== 'android') return true; // iOS asks on first use
  const granted = await PermissionsAndroid.request(
    PermissionsAndroid.PERMISSIONS.RECORD_AUDIO,
    {
      title: 'Microphone',
      message: 'Needed to hear the other phone.',
      buttonPositive: 'OK',
    },
  );
  return granted === PermissionsAndroid.RESULTS.GRANTED;
}

export default function ModemPanel(): React.JSX.Element {
  // Read from native, never assumed. The HybridObject and its audio session
  // outlive a JavaScript reload: the modem keeps listening and the microphone
  // stays open, while React state goes back to its defaults. Seeding these from
  // the module means a reload shows what is actually happening and offers Stop,
  // rather than offering Start and then refusing it.
  const [listening, setListening] = useState(() => ggwave.isListening);
  // Defaults on, because the one phone test is the common case here. If the
  // modem is already running, show what it is actually set to instead.
  const [hearMyself, setHearMyself] = useState(() =>
    ggwave.isListening ? ggwave.allowSelfReception : true,
  );
  const [protocol, setProtocol] = useState<ProtocolId>(Protocol.ULTRASOUND_FAST);
  const [route, setRoute] = useState(() => ggwave.route);
  const [log, setLog] = useState<Entry[]>([]);
  const sequence = useRef(0);

  const add = useCallback((text: string, kind: Entry['kind']) => {
    const at = new Date().toISOString().slice(11, 23);
    setLog((entries) => [{ at, text, kind }, ...entries].slice(0, 40));
  }, []);

  // Registered once. A void callback is dispatched to the JS thread by Nitro
  // from the modem's worker thread, so this is safe to touch state from.
  useEffect(() => {
    ggwave.onMessage = (payload: ArrayBuffer) => {
      const bytes = Array.from(new Uint8Array(payload));
      const hex = bytes.map((b) => b.toString(16).padStart(2, '0')).join(' ');
      add(`heard ${hex}`, 'in');
    };
    return () => {
      ggwave.onMessage = undefined;
    };
  }, [add]);

  // Say so on arrival when the modem was already running, which is what a
  // JavaScript reload leaves behind.
  useEffect(() => {
    if (ggwave.isListening) {
      add(`already listening from before the reload: ${ggwave.route}`, 'note');
    }
  }, [add]);

  // Never leave the microphone open behind us. This does not run on a full
  // reload, since the runtime is discarded without React unmounting anything,
  // which is exactly why the state above is read from native.
  useEffect(() => {
    return () => {
      try {
        ggwave.stop();
      } catch {
        // already stopped
      }
    };
  }, []);

  const start = useCallback(async () => {
    if (!(await ensureMicrophone())) {
      add('microphone refused', 'bad');
      return;
    }
    try {
      ggwave.allowSelfReception = hearMyself;
      startModem({ protocol, payloadBytes: PAYLOAD_LENGTH });
      // Reflect what start actually produced rather than what was requested.
      setHearMyself(ggwave.allowSelfReception);
      setListening(ggwave.isListening);
      setRoute(ggwave.route);
      add(`listening: ${ggwave.route}`, 'note');
    } catch (error) {
      add(String(error), 'bad');
    }
  }, [add, hearMyself, protocol]);

  const stop = useCallback(() => {
    try {
      ggwave.stop();
    } catch (error) {
      add(String(error), 'bad');
    }
    setListening(ggwave.isListening);
    setRoute(ggwave.route);
    add('stopped', 'note');
  }, [add]);

  const send = useCallback(() => {
    // A Chalk move: kind and move in byte 0, sequence in byte 1, CRC in byte 2.
    sequence.current = (sequence.current + 1) & 0xff;
    const buffer = new ArrayBuffer(PAYLOAD_LENGTH);
    const view = new Uint8Array(buffer);
    view[0] = 0x2a;
    view[1] = sequence.current;
    view[2] = (0x2a ^ sequence.current) & 0xff;
    try {
      ggwave.send(buffer, VOLUME);
      const hex = Array.from(view)
        .map((b) => b.toString(16).padStart(2, '0'))
        .join(' ');
      add(`sent ${hex}`, 'out');
    } catch (error) {
      add(String(error), 'bad');
    }
  }, [add]);

  const protocols: [string, ProtocolId][] = [
    ['U fast', Protocol.ULTRASOUND_FAST],
    ['U fastest', Protocol.ULTRASOUND_FASTEST],
    ['audible', Protocol.AUDIBLE_FAST],
  ];

  return (
    <View style={styles.panel}>
      <Text style={styles.heading}>Modem</Text>
      <Text style={styles.route}>{route}</Text>

      <View style={styles.protocolRow}>
        {protocols.map(([label, id]) => (
          <TouchableOpacity
            key={label}
            disabled={listening}
            onPress={() => setProtocol(id)}
            style={[
              styles.chip,
              protocol === id && styles.chipOn,
              listening && styles.chipDisabled,
            ]}
          >
            <Text style={[styles.chipText, protocol === id && styles.chipTextOn]}>{label}</Text>
          </TouchableOpacity>
        ))}
      </View>

      <View style={styles.switchRow}>
        <Text style={styles.switchLabel}>Hear myself (one phone test)</Text>
        <Switch value={hearMyself} onValueChange={setHearMyself} disabled={listening} />
      </View>

      <View style={styles.buttonRow}>
        <TouchableOpacity
          style={[styles.button, listening ? styles.buttonStop : styles.buttonStart]}
          onPress={listening ? stop : start}
        >
          <Text style={styles.buttonText}>{listening ? 'Stop' : 'Start listening'}</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={[styles.button, styles.buttonSend, !listening && styles.chipDisabled]}
          onPress={send}
          disabled={!listening}
        >
          <Text style={styles.buttonText}>Send a move</Text>
        </TouchableOpacity>
      </View>

      <ScrollView style={styles.log} contentContainerStyle={styles.logContent}>
        {log.map((entry, index) => (
          <View key={`${entry.at}-${index}`} style={styles.logRow}>
            <Text style={styles.logTime}>{entry.at}</Text>
            <Text
              style={[
                styles.logText,
                entry.kind === 'in' && styles.good,
                entry.kind === 'out' && styles.out,
                entry.kind === 'bad' && styles.bad,
              ]}
            >
              {entry.text}
            </Text>
          </View>
        ))}
      </ScrollView>
    </View>
  );
}

const styles = StyleSheet.create({
  panel: { flex: 1, marginTop: 8 },
  heading: { fontSize: 22, fontWeight: '700', color: '#f4f4f5' },
  route: { fontSize: 12, color: '#7c7c85', marginTop: 2, marginBottom: 10 },
  protocolRow: { flexDirection: 'row', gap: 8, marginBottom: 10 },
  chip: {
    paddingVertical: 7,
    paddingHorizontal: 12,
    borderRadius: 999,
    backgroundColor: '#1e1e24',
  },
  chipOn: { backgroundColor: '#2f6fed' },
  chipDisabled: { opacity: 0.45 },
  chipText: { color: '#a1a1aa', fontSize: 13, fontWeight: '600' },
  chipTextOn: { color: '#ffffff' },
  switchRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    marginBottom: 10,
  },
  switchLabel: { color: '#d4d4d8', fontSize: 14, flex: 1 },
  buttonRow: { flexDirection: 'row', gap: 10 },
  button: { flex: 1, paddingVertical: 13, borderRadius: 12, alignItems: 'center' },
  buttonStart: { backgroundColor: '#2f6fed' },
  buttonStop: { backgroundColor: '#8b3a3a' },
  buttonSend: { backgroundColor: '#2b7a55' },
  buttonText: { color: '#ffffff', fontSize: 15, fontWeight: '600' },
  log: { flex: 1, marginTop: 14 },
  logContent: { paddingBottom: 30 },
  logRow: { flexDirection: 'row', paddingVertical: 4 },
  logTime: { width: 92, color: '#5a5a63', fontSize: 12, fontVariant: ['tabular-nums'] },
  logText: { flex: 1, color: '#d4d4d8', fontSize: 13 },
  good: { color: '#3ecf8e', fontWeight: '600' },
  out: { color: '#8bb4ff' },
  bad: { color: '#f26d6d' },
});
