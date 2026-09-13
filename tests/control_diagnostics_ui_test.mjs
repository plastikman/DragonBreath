import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

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
        controller_request: 0,
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
    control: { loop: { controller: 'pid', preferred_source: 'local_ntc', effective_source: 'unavailable', process_variable_c: null, controller_request: 0, allowed_output: 0, constraint: 'off' } }
  });
  const view = DBControlDiagnostics.view(value);
  assert.equal(view.controllerSource, 'PID using no active temperature input');
  assert.equal(view.request, '0%');
  assert.equal(view.allowed, '0%');
  assert.equal(view.delivered, 'OFF');
  assert.match(view.constraint.text, /^IDLE/);
});

test('partial PID duty renders requested, allowed, and delivered independently', () => {
  const value = state();
  value.control.loop.controller_request = 0.372;
  value.control.loop.allowed_output = 0.372;
  value.heater.output = true;
  const view = DBControlDiagnostics.view(value);
  assert.equal(view.request, '37%');
  assert.equal(view.allowed, '37%');
  assert.equal(view.delivered, 'ON');
  assert.equal(view.constraint.text, 'NORMAL — no output limit');
});

test('foldback shows request above allowed output and the authoritative limiter', () => {
  const value = state();
  value.control.loop.controller_request = 0.72;
  value.control.loop.allowed_output = 0;
  value.control.loop.constraint = 'local_foldback';
  const view = DBControlDiagnostics.view(value);
  assert.equal(view.request, '72%');
  assert.equal(view.allowed, '0%');
  assert.equal(view.delivered, 'OFF');
  assert.equal(view.constraint.text, 'LOCAL CHAMBER FOLDBACK');
  assert.match(view.safety.text, /^LIMITED/);
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

test('SSE failure closes once and enters polling immediately', () => {
  const listeners = {};
  let closes = 0; let polls = 0; let status = '';
  const stream = { addEventListener(name, fn) { listeners[name] = fn; }, close() { closes += 1; } };
  DBControlDiagnostics.connect({
    createEventSource: (url) => { assert.equal(url, '/api/v2/events'); return stream; },
    apply() {},
    status: (value) => { status = value; },
    poll: () => { polls += 1; }
  });
  stream.onerror();
  stream.onerror();
  assert.equal(closes, 1);
  assert.equal(polls, 1);
  assert.match(status, /switching to polling/);
});

let passed = 0;
for (const { name, body } of tests) {
  try { await body(); passed += 1; console.log('PASS', name); }
  catch (error) { console.error('FAIL', name); throw error; }
}
console.log(`control diagnostics UI tests: ${passed}/${tests.length} PASS`);
