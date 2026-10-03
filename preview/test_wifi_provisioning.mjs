// Explicitly invoked hardware test: re-saves current Wi-Fi, then verifies rollback
// from a deliberately nonexistent SSID. Requires Ethernet to remain connected.
// Never calls the warehouse API and never prints credentials or the setup code.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { setTimeout as delay } from 'node:timers/promises';

const origin = new URL(process.argv[2] || 'http://192.168.29.157');
const request = (path, options = {}) => fetch(new URL(path, origin), {
  ...options, signal: AbortSignal.timeout(8000),
});
const status = async () => (await request('/api/network')).json();
const source = readFileSync(new URL('../main/secrets.h', import.meta.url), 'utf8');
const value = name => {
  const match = source.match(new RegExp('^#define\\s+' + name + '\\s+("(?:[^"\\\\]|\\\\.)*")', 'm'));
  assert(match, 'Expected string literal configuration');
  return JSON.parse(match[1]);
};
const ssid = value('STOCKIN_WIFI_SSID'), password = value('STOCKIN_WIFI_PASSWORD');
const before = await status();
assert(before.wifi && before.ethernet && !before.configuring, 'Both uplinks must be ready');
assert.equal(before.ssid, ssid, 'Refusing to replace an unknown saved Wi-Fi network');
process.stdout.write('Enter the eight-digit display code (hidden): ');
const code = await new Promise((resolve, reject) => {
  let text = '';
  if (process.stdin.isTTY) process.stdin.setRawMode(true);
  process.stdin.resume();
  const finish = () => {
    process.stdin.off('data', input);
    if (process.stdin.isTTY) process.stdin.setRawMode(false);
    process.stdin.pause();
    process.stdout.write('\n');
  };
  const input = data => {
    for (const char of data.toString()) {
      if (char === '\u0003') { finish(); reject(new Error('Cancelled')); return; }
      if (char === '\r' || char === '\n') { finish(); resolve(text); return; }
      if (char === '\u007f' || char === '\b') text = text.slice(0, -1);
      else text += char;
    }
  };
  process.stdin.on('data', input);
});
assert(/^\d{8}$/.test(code), 'Expected eight digits');
async function apply(testSSID, testPassword) {
  const r = await request('/api/wifi', { method: 'POST', headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({ssid: testSSID, password: testPassword, code}) });
  assert.equal(r.status, 202, `Expected accepted setup request, got HTTP ${r.status}`);
}
async function until(predicate, seconds) {
  for (let i = 0; i < seconds; ++i) {
    const s = await status();
    assert(s.ethernet, 'Ethernet must remain available throughout the test');
    if (predicate(s)) return s;
    await delay(1000);
  }
  throw new Error('Timed out waiting for provisioning state');
}
await apply(ssid, password);
console.log('Testing existing Wi-Fi settings through Ethernet…');
const saved = await until(s => !s.configuring && s.wifi && s.message.includes('saved'), 40);
assert(saved.hotspot, 'Recovery hotspot should be available during provisioning');
if (before.known_ssids) assert.deepEqual([...saved.known_ssids].sort(), [...before.known_ssids].sort(),
  'Updating an existing SSID must retain all saved networks without duplicates');
console.log('PASS: DHCP-confirmed save, recovery hotspot active, Ethernet uninterrupted');
await apply('Rearly-NoSuch-' + Date.now().toString(16), 'Test-WiFi-Only');
await until(s => s.configuring && !s.wifi && s.hotspot, 15);
console.log('Testing failed-connection rollback (45-second deadline)…');
const restored = await until(s => !s.configuring && s.wifi && s.ssid === ssid, 65);
assert(restored.message.includes('Previous Wi-Fi restored'));
if (before.known_ssids) assert.deepEqual([...restored.known_ssids].sort(), [...before.known_ssids].sort(),
  'A failed test network must not overwrite or enter the known list');
console.log('PASS: invalid network timed out, original Wi-Fi reconnected; Ethernet remained online');
