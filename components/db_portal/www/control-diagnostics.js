(function (global) {
  'use strict';

  function finite(value) { return typeof value === 'number' && isFinite(value); }
  function temperature(value) { return finite(value) ? value.toFixed(1) + ' °C' : '—'; }
  function percent(value) { return finite(value) ? Math.round(value * 100) + '%' : '—'; }

  function sourceLabel(value) {
    if (value === 'bambu') return 'Bambu chamber telemetry';
    if (value === 'local_ntc') return 'local chamber NTC';
    if (value === 'unavailable') return 'no active temperature input';
    return String(value || 'unknown source');
  }

  function controllerLabel(value) {
    if (value === 'pid') return 'PID';
    return String(value || 'Unknown controller');
  }

  function modeLabel(value) {
    var labels = { off: 'Off', power_on: 'Manual', manual: 'Manual', auto: 'Automatic', drying: 'Drying' };
    return labels[value] || String(value || 'Unknown');
  }

  function constraintPresentation(value) {
    var labels = {
      off: 'IDLE — controller not requesting heat',
      none: 'NORMAL — no output limit',
      approach_limit: 'APPROACH LIMIT',
      target_reached: 'TARGET REACHED',
      local_foldback: 'LOCAL CHAMBER FOLDBACK',
      element_foldback: 'ELEMENT FOLDBACK',
      pid_error: 'CONTROLLER ERROR',
      safety_inhibited: 'SAFETY INHIBITED'
    };
    var kind = value === 'none' || value === 'off' || value === 'target_reached'
      ? 'ok'
      : value === 'pid_error' || value === 'safety_inhibited' ? 'bad' : 'warn';
    return { text: labels[value] || String(value || 'unknown').replace(/_/g, ' ').toUpperCase(), kind: kind };
  }

  function view(state) {
    if (!state || state.api_version !== 2) return null;
    var loop = ((state.control || {}).loop) || {};
    var safety = state.safety || {};
    var environment = state.environment || {};
    var heater = state.heater || {};
    var sensors = state.sensors || {};
    var chamber = sensors.chamber || {};
    var ptc = sensors.ptc || {};
    var target = state.target || {};
    var fan = state.fan || {};
    var bambuEligible = state.mode === 'auto' && environment.control_source === 'bambu';
    var fallback = bambuEligible && loop.preferred_source === 'bambu' &&
      loop.effective_source === 'local_ntc';
    var constraintValue = loop.constraint || 'off';
    var constraint = constraintPresentation(constraintValue);
    var faulted = !!safety.fault_latched;
    var inhibited = !!safety.inhibited || constraintValue === 'safety_inhibited';
    var limited = constraintValue !== 'none' && constraintValue !== 'off' &&
      constraintValue !== 'target_reached' && constraintValue !== 'safety_inhibited';
    var safetyText = 'NORMAL';
    var safetyKind = 'ok';
    if (faulted) {
      safetyText = 'FAULT — ' + (safety.reason || 'reason unavailable');
      safetyKind = 'bad';
    } else if (inhibited) {
      safetyText = 'SAFETY INHIBITED — ' + (safety.reason || 'reason unavailable');
      safetyKind = 'bad';
    } else if (fallback) {
      safetyText = 'DEGRADED — using local NTC fallback';
      safetyKind = 'warn';
    } else if (constraintValue === 'pid_error') {
      safetyText = 'DEGRADED — controller output unavailable';
      safetyKind = 'bad';
    } else if (limited) {
      safetyText = 'LIMITED — ' + constraint.text.toLowerCase();
      safetyKind = 'warn';
    }

    return {
      controllerSource: controllerLabel(loop.controller) + ' using ' + sourceLabel(loop.effective_source),
      controlTemperature: 'Controlling temperature: ' + temperature(loop.process_variable_c) +
        (fallback ? ' · preferred Bambu telemetry unavailable' : ''),
      target: temperature(target.effective_c),
      mode: modeLabel(state.mode),
      request: percent(loop.controller_request),
      allowed: percent(loop.allowed_output),
      delivered: heater.output ? 'ON' : 'OFF',
      constraint: constraint,
      safety: { text: safetyText, kind: safetyKind },
      preferredSource: sourceLabel(loop.preferred_source),
      localTemperature: temperature(chamber.temperature_c),
      printerTemperature: temperature(environment.printer_chamber_temperature_c),
      ptcTemperature: temperature(ptc.temperature_c),
      fan: (fan.effective_percent || 0) + '% · ' + (fan.reason || 'off'),
      sample: {
        pv: loop.process_variable_c,
        target: target.effective_c,
        ptc: ptc.temperature_c,
        duty: loop.allowed_output,
        controller: loop.controller || '',
        source: loop.effective_source || '',
        constraint: constraintValue,
        mode: state.mode || ''
      }
    };
  }

  function connect(options) {
    var stream = options.createEventSource('/api/v2/events');
    var failed = false;
    function receive(event) {
      try {
        options.apply(JSON.parse(event.data));
        options.status('Telemetry: live');
      } catch (_) {}
    }
    stream.addEventListener('state', receive);
    stream.addEventListener('telemetry', receive);
    stream.onerror = function () {
      if (failed) return;
      failed = true;
      stream.close();
      options.status('Live stream unavailable — switching to polling');
      options.poll();
    };
    return stream;
  }

  global.DBControlDiagnostics = { view: view, connect: connect };
}(typeof window !== 'undefined' ? window : globalThis));
