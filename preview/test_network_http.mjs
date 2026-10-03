// Non-mutating live checks. Wrong setup codes briefly exercise the rate limiter.
// Never sends valid credentials and never calls a packing/inventory endpoint.
import assert from 'node:assert/strict';

const origin = new URL(process.argv[2] || 'http://192.168.29.156');
const request = (path, options = {}) => fetch(new URL(path, origin), {
  ...options, signal: AbortSignal.timeout(8000),
});
const initial = await (await request('/api/network')).json();
assert(!initial.configuring, 'Do not run during a real Wi-Fi change');
for (const forbidden of ['password', 'code', 'setup_code', 'key']) assert(!(forbidden in initial));
const page = await request('/');
assert.equal(page.status, 200);
assert.equal(page.headers.get('cache-control'), 'no-store');
assert.equal(page.headers.get('x-frame-options'), 'DENY');
assert((await page.text()).includes('Connect &amp; save'));
const post = (body, type = 'application/json') => request('/api/wifi', {
  method: 'POST', headers: { 'Content-Type': type }, body,
});
assert.equal((await post('ssid=x', 'application/x-www-form-urlencoded')).status, 415);
assert.equal((await post('a'.repeat(513))).status, 413);
assert.equal((await post('{bad json')).status, 400);
assert.equal((await post('{"ssid":"x","password":"12345678"}')).status, 400);
assert.equal((await post('{"ssid":"x\\u0000tail","password":"12345678","code":"not-code"}')).status, 400);
let limited = false;
for (let i = 0; i < 6; ++i) {
  const r = await post(JSON.stringify({ ssid: 'TEST-DO-NOT-CONNECT', password: '12345678', code: 'not-code' }));
  assert([403, 429].includes(r.status));
  if (r.status === 429) { limited = true; break; }
}
assert(limited, 'Incorrect setup codes must be rate limited');
const after = await (await request('/api/network')).json();
assert.equal(after.ssid, initial.ssid);
assert.equal(after.configuring, false);
assert.equal(after.wifi, initial.wifi);
console.log('PASS: page, status privacy, content type, size limits, malformed/NUL JSON, authentication and rate limiting; no Wi-Fi change');
