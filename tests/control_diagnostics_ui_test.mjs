import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';
import path from 'node:path';
import fs from 'node:fs';
import vm from 'node:vm';

await import(pathToFileURL(path.resolve('components/db_portal/www/control-diagnostics.js')));

function state(overrides = {}) {
  const base = {
    api_version: 2,
    mode: 'power_on',
    target: { effective_c: 55 },
    heater: { output: false },
    fan: { effective_percent: 100, reason: 'heater' },
    sensors: {
      chamber: { temperature_c: 48.2, status: 'ok' },
      ptc: { temperature_c: 72.4, status: 'ok' }
    },
    environment: {
      control_source: 'bambu',
      printer_chamber_temperature_c: 46.5
    },
    control: {
      loop: {
        controller: 'pid',
        preferred_source: 'local_ntc',
        effective_source: 'local_ntc',
        process_variable_c: 48.2,
        pid_output: 0,
        allowed_output: 0,
        constraint: 'none'
      }
    },
    safety: { fault_latched: false, inhibited: false, reason: null }
  };
  return Object.assign(base, overrides);
}

const tests = [];
function test(name, body) { tests.push({ name, body }); }

test('idle state is visibly inactive with zero outputs', () => {
  const value = state({
    mode: 'off', target: { effective_c: 0 }, fan: { effective_percent: 0, reason: 'off' },
    control: { loop: { controller: 'pid', preferred_source: 'local_ntc', effective_source: 'unavailable', process_variable_c: null, pid_output: 0, allowed_output: 0, constraint: 'off' } }
  });
  const view = DBControlDiagnostics.view(value);
  assert.equal(view.controllerSource, 'PID using no active temperature input');
  assert.equal(view.pidOutput, '0%');
  assert.equal(view.allowed, '0%');
  assert.equal(view.delivered, 'OFF');
  assert.match(view.constraint.text, /^IDLE/);
});

test('partial PID duty renders post-approach output, allowed duty, and SSR command', () => {
  const value = state();
  value.control.loop.pid_output = 0.372;
  value.control.loop.allowed_output = 0.372;
  value.heater.output = true;
  const view = DBControlDiagnostics.view(value);
  assert.equal(view.pidOutput, '37%');
  assert.equal(view.allowed, '37%');
  assert.equal(view.delivered, 'ON');
  assert.equal(view.constraint.text, 'NORMAL — no output limit');
});

test('foldback shows post-approach PID output above allowed output and the authoritative limiter', () => {
  const value = state();
  value.control.loop.pid_output = 0.72;
  value.control.loop.allowed_output = 0;
  value.control.loop.constraint = 'local_foldback';
  const view = DBControlDiagnostics.view(value);
  assert.equal(view.pidOutput, '72%');
  assert.equal(view.allowed, '0%');
  assert.equal(view.delivered, 'OFF');
  assert.equal(view.constraint.text, 'LOCAL CHAMBER FOLDBACK');
  assert.match(view.safety.text, /^LIMITED/);
});

test('approach-limited output is not advertised as pre-limit demand', () => {
  const value = state();
  value.control.loop.pid_output = 0.4;
  value.control.loop.allowed_output = 0.4;
  value.control.loop.constraint = 'approach_limit';
  const view = DBControlDiagnostics.view(value);
  assert.equal(view.pidOutput, '40%');
  assert.equal(view.allowed, '40%');
  assert.equal(view.delivered, 'OFF'); // inactive portion of the SSR window
  assert.equal(view.constraint.text, 'APPROACH LIMIT');
  delete value.control.loop.pid_output;
  value.control.loop.controller_request = 0.9; // no misleading legacy fallback
  assert.equal(DBControlDiagnostics.view(value).pidOutput, '—');
});

test('fault state forces the safety explanation to the authoritative reason', () => {
  const value = state({ safety: { fault_latched: true, inhibited: false, reason: 'PTC element over-temp' } });
  value.control.loop.constraint = 'safety_inhibited';
  const view = DBControlDiagnostics.view(value);
  assert.equal(view.allowed, '0%');
  assert.equal(view.delivered, 'OFF');
  assert.equal(view.safety.text, 'FAULT — PTC element over-temp');
  assert.equal(view.safety.kind, 'bad');
});

test('effective source, process variable, and target follow the server snapshot', () => {
  const local = DBControlDiagnostics.view(state());
  const externalState = state({ target: { effective_c: 60 }, mode: 'auto' });
  externalState.control.loop.preferred_source = 'bambu';
  externalState.control.loop.effective_source = 'bambu';
  externalState.control.loop.process_variable_c = 46.5;
  const external = DBControlDiagnostics.view(externalState);
  assert.match(local.controllerSource, /local chamber NTC/);
  assert.match(local.controlTemperature, /48.2/);
  assert.match(external.controllerSource, /Bambu chamber telemetry/);
  assert.match(external.controlTemperature, /46.5/);
  assert.equal(external.target, '60.0 °C');
});

test('eligible Bambu fallback is degraded but an inapplicable saved preference is not', () => {
  const fallbackState = state({ mode: 'auto' });
  fallbackState.control.loop.preferred_source = 'bambu';
  const fallback = DBControlDiagnostics.view(fallbackState);
  assert.match(fallback.safety.text, /^DEGRADED/);

  fallbackState.environment.control_source = 'klipper';
  const inapplicable = DBControlDiagnostics.view(fallbackState);
  assert.equal(inapplicable.safety.text, 'NORMAL');
});

test('SSE state event renders immediately', () => {
  const listeners = {};
  const stream = { addEventListener(name, fn) { listeners[name] = fn; }, close() {} };
  let applied = null; let status = '';
  DBControlDiagnostics.connect({
    createEventSource: () => stream,
    apply: (value) => { applied = value; },
    status: (value) => { status = value; },
    poll() {}
  });
  listeners.state({ data: JSON.stringify({ api_version: 2, marker: 'first' }) });
  assert.equal(applied.marker, 'first');
  assert.equal(status, 'Telemetry: live');
});

function connection() {
  const listeners = {};
  const counts = { closes: 0, polls: 0, applies: 0 };
  const stream = {
    readyState: 0,
    addEventListener(name, fn) { listeners[name] = fn; },
    close() { counts.closes += 1; this.readyState = 2; }
  };
  DBControlDiagnostics.connect({
    createEventSource: (url) => { assert.equal(url, '/api/v2/events'); return stream; },
    apply() { counts.applies += 1; }, status() {},
    poll() { counts.polls += 1; }
  });
  return { stream, listeners, counts };
}

test('transient SSE errors leave EventSource free to reconnect', () => {
  const { stream, listeners, counts } = connection();
  stream.onerror();
  stream.onerror();
  assert.equal(counts.closes, 0);
  assert.equal(counts.polls, 0);
  listeners.telemetry({ data: JSON.stringify(state()) });
  stream.onerror();
  stream.onerror();
  assert.equal(counts.closes, 0); // successful telemetry reset the failure budget
  assert.equal(counts.applies, 1);
});

test('three errors without telemetry close once and start only one polling loop', () => {
  const { stream, listeners, counts } = connection();
  stream.onerror();
  listeners.telemetry({ data: 'invalid JSON' });
  stream.onerror();
  stream.onerror();
  stream.onerror();
  listeners.state({ data: JSON.stringify(state()) }); // queued event after close
  assert.deepEqual(counts, { closes: 1, polls: 1, applies: 0 });
});

test('terminal SSE closure falls back immediately', () => {
  const { stream, counts } = connection();
  stream.readyState = 2;
  stream.onerror();
  stream.onerror();
  assert.deepEqual(counts, { closes: 1, polls: 1, applies: 0 });
});

test('unavailable EventSource falls back once', () => {
  let polls = 0;
  const stream = DBControlDiagnostics.connect({
    createEventSource() { throw new Error('unsupported'); },
    apply() {}, status() {}, poll() { polls += 1; }
  });
  assert.equal(stream, null);
  assert.equal(polls, 1);
});

const html = fs.readFileSync('components/db_portal/www/diagnostics.html', 'utf8');
const themeScript = html.match(/<script id="theme-restore">([\s\S]*?)<\/script>/)[1];
for (const saved of ['light', 'dark', null, 'system', 'invalid']) {
  test(`theme restore honors explicit override and otherwise retains system: ${saved}`, () => {
    const attributes = {};
    vm.runInNewContext(themeScript, {
      localStorage: { getItem(key) { assert.equal(key, 'db_theme'); return saved; } },
      document: { documentElement: { setAttribute(key, value) { attributes[key] = value; } } }
    });
    assert.deepEqual(attributes, saved === 'light' || saved === 'dark' ? { 'data-theme': saved } : {});
  });
}
test('theme restore tolerates unavailable localStorage', () => {
  vm.runInNewContext(themeScript, {
    localStorage: { getItem() { throw new Error('storage denied'); } }
  });
});

let passed = 0;
for (const { name, body } of tests) {
  try { await body(); passed += 1; console.log('PASS', name); }
  catch (error) { console.error('FAIL', name); throw error; }
}
console.log(`control diagnostics UI tests: ${passed}/${tests.length} PASS`);
