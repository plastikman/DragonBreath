import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';
import path from 'node:path';

class FakeClassList {
  constructor(node) { this.node = node; }
  add(name) { this.node.className = (this.node.className + ' ' + name).trim(); }
}

class FakeElement {
  constructor(document, tagName) {
    this.ownerDocument = document;
    this.tagName = tagName.toUpperCase();
    this.children = [];
    this.listeners = new Map();
    this.attributes = new Map();
    this.className = '';
    this.classList = new FakeClassList(this);
    this.textContent = '';
    this.checked = false;
    this.disabled = false;
    this.hidden = false;
    this.parentNode = null;
  }
  appendChild(node) { node.parentNode = this; this.children.push(node); return node; }
  replaceChildren(...nodes) { this.children = []; nodes.forEach((node) => this.appendChild(node)); }
  setAttribute(name, value) { this.attributes.set(name, String(value)); }
  getAttribute(name) { return this.attributes.get(name); }
  addEventListener(type, listener) {
    if (!this.listeners.has(type)) this.listeners.set(type, []);
    this.listeners.get(type).push(listener);
  }
  dispatch(type, details = {}) {
    const event = Object.assign({ type, target: this, preventDefault() { this.defaultPrevented = true; } }, details);
    for (const listener of this.listeners.get(type) || []) listener(event);
    return event;
  }
  focus() { this.ownerDocument.activeElement = this; }
  showModal() { this.open = true; }
  close() { this.open = false; }
  remove() {
    if (this.parentNode) this.parentNode.children = this.parentNode.children.filter((node) => node !== this);
    this.parentNode = null;
  }
}

class FakeDocument {
  constructor() { this.body = new FakeElement(this, 'body'); this.activeElement = this.body; }
  createElement(name) { return new FakeElement(this, name); }
}

function find(root, predicate) {
  if (predicate(root)) return root;
  for (const child of root.children || []) {
    const match = find(child, predicate);
    if (match) return match;
  }
  return null;
}

function byClass(root, className) {
  return find(root, (node) => String(node.className || '').split(/\s+/).includes(className));
}

async function settle() {
  await Promise.resolve();
  await Promise.resolve();
  await new Promise((resolve) => setImmediate(resolve));
}

globalThis.document = new FakeDocument();
const modulePath = path.resolve('components/db_portal/www/consequential-toggle.js');
await import(pathToFileURL(modulePath));

function fixture(overrides = {}) {
  const document = new FakeDocument();
  const root = new FakeElement(document, 'div');
  document.body.appendChild(root);
  let value = overrides.initial ?? false;
  let mutations = 0;
  const options = {
    id: 'test-toggle',
    label: 'Automatic mode',
    description: 'May operate without another browser action.',
    read: async () => ({ value, disabled: false }),
    mutate: async (requested) => { mutations += 1; value = requested; return { value }; },
    confirm: () => null,
    ...overrides,
  };
  const api = DBConsequentialToggle.mount(root, options);
  return { document, root, api, get mutations() { return mutations; }, set value(next) { value = next; } };
}

const tests = [];
function test(name, body) { tests.push({ name, body }); }

test('toggle without confirmation mutates once', async () => {
  const f = fixture(); await settle(); await f.api.request(true);
  assert.equal(f.mutations, 1); assert.equal(f.api.state().value, true);
});

test('toggle requiring confirmation waits for explicit confirm', async () => {
  const f = fixture({ confirm: () => ({ title: 'Enable?', body: 'This changes operation.', confirmLabel: 'Enable', cancelLabel: 'Cancel' }) });
  await settle(); const pending = f.api.request(true); await settle();
  assert.equal(f.mutations, 0); byClass(f.document.body, 'ct-confirm').dispatch('click'); await pending;
  assert.equal(f.mutations, 1); assert.equal(f.api.state().value, true);
});

test('cancel causes zero mutation', async () => {
  const f = fixture({ confirm: () => ({ title: 'Enable?' }) }); await settle();
  const pending = f.api.request(true); await settle(); byClass(f.document.body, 'ct-cancel').dispatch('click'); await pending;
  assert.equal(f.mutations, 0); assert.equal(f.api.state().value, false);
});

test('pending mutation rejects duplicate submissions', async () => {
  let finish; let count = 0;
  const f = fixture({ mutate: () => { count += 1; return new Promise((resolve) => { finish = resolve; }); } });
  await settle(); const first = f.api.request(true); await settle(); await f.api.request(false);
  assert.equal(count, 1); assert.equal(f.api.elements.input.disabled, true); assert.match(f.api.elements.status.textContent, /Saving/);
  finish({ value: true }); await first; assert.equal(f.api.state().value, true);
});

test('mutation failure reconciles authoritative state', async () => {
  let reads = 0;
  const f = fixture({
    read: async () => ({ value: reads++ > 0, disabled: false }),
    mutate: async () => { throw new Error('server refused transition'); },
  });
  await settle(); await f.api.request(true);
  assert.equal(f.api.state().value, true); assert.match(f.api.elements.status.textContent, /server refused/); assert.match(f.api.elements.status.className, /is-error/);
});

test('disabled toggle cannot mutate', async () => {
  let count = 0; const f = fixture({ read: async () => ({ value: false, disabled: true, disabledReason: 'Not eligible now.' }), mutate: async () => { count += 1; } });
  await settle(); await f.api.request(true); assert.equal(count, 0); assert.equal(f.api.elements.input.disabled, true); assert.equal(f.api.elements.reason.textContent, 'Not eligible now.');
});

test('modal renders product-supplied text and labels', async () => {
  const f = fixture({ confirm: () => ({ title: 'Consequential title', body: 'Exact operational consequence.', confirmLabel: 'Proceed safely', cancelLabel: 'Keep current state' }) });
  await settle(); const pending = f.api.request(true); await settle();
  assert.equal(byClass(f.document.body, 'ct-dialog-title').textContent, 'Consequential title');
  assert.equal(byClass(f.document.body, 'ct-dialog-body').textContent, 'Exact operational consequence.');
  assert.equal(byClass(f.document.body, 'ct-confirm').textContent, 'Proceed safely');
  assert.equal(byClass(f.document.body, 'ct-cancel').textContent, 'Keep current state');
  byClass(f.document.body, 'ct-cancel').dispatch('click'); await pending;
});

test('keyboard cancellation closes modal without mutation', async () => {
  const f = fixture({ confirm: () => ({ title: 'Enable?' }) }); await settle();
  const pending = f.api.request(true); await settle(); const dialog = byClass(f.document.body, 'ct-dialog');
  const event = dialog.dispatch('keydown', { key: 'Escape' }); await pending;
  await new Promise((resolve) => setTimeout(resolve, 1));
  assert.equal(event.defaultPrevented, true); assert.equal(f.mutations, 0); assert.equal(byClass(f.document.body, 'ct-dialog'), null);
  assert.equal(f.document.activeElement, f.api.elements.input);
});

test('server response differing from request wins', async () => {
  let count = 0; const f = fixture({ mutate: async () => { count += 1; return { value: false, disabled: false }; } });
  await settle(); await f.api.request(true); assert.equal(count, 1); assert.equal(f.api.state().value, false); assert.equal(f.api.elements.input.checked, false);
});

test('stale browser checkbox state cannot override server response', async () => {
  const f = fixture({ mutate: async () => ({ value: true, disabled: false }) }); await settle();
  f.api.elements.input.checked = false; await f.api.request(false);
  assert.equal(f.api.state().value, true); assert.equal(f.api.elements.input.checked, true);
});

let passed = 0;
for (const { name, body } of tests) {
  try { await body(); passed += 1; console.log('PASS', name); }
  catch (error) { console.error('FAIL', name); throw error; }
}
console.log(`consequential toggle UI tests: ${passed}/${tests.length} PASS`);
