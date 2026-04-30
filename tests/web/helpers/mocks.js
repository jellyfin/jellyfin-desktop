// Recording stub: returns a function that records each call's arguments
// and returns whatever `impl` returns (or undefined).
export function recorder(impl) {
  const fn = (...args) => {
    fn.calls.push(args);
    return impl ? impl(...args) : undefined;
  };
  fn.calls = [];
  return fn;
}

// Connect/disconnect-style signal slot.
function makeSignal() {
  const handlers = [];
  const sig = (...args) => { for (const h of handlers.slice()) h(...args); };
  sig.connect = (h) => { handlers.push(h); };
  sig.disconnect = (h) => {
    const i = handlers.indexOf(h);
    if (i >= 0) handlers.splice(i, 1);
  };
  sig.emit = (...args) => sig(...args);
  sig.handlers = handlers;
  return sig;
}

// Build a bus mock matching window.jmp's contract.
//
// `bus.send` is a recorder so tests can assert on outbound traffic.
// `bus.deliver(name, payload)` simulates inbound messages from C++ (the
// test-side equivalent of g_bus.emit).
export function makeJmpBus() {
  const handlers = Object.create(null);
  const bus = {
    send: recorder(),
    on(name, fn) { (handlers[name] = handlers[name] || []).push(fn); },
    off(name, fn) {
      const list = handlers[name];
      if (!list) return;
      const i = list.indexOf(fn);
      if (i >= 0) list.splice(i, 1);
    },
    onMessage(name, payload) {
      const list = handlers[name] || [];
      for (const fn of list.slice()) fn(payload || {});
    },
    deliver(name, payload) {
      bus.onMessage(name, payload);
    },
    _handlers: handlers,
  };
  return bus;
}

// api.player signal surface — what mpv-player-core's connectSignals/
// disconnectSignals interacts with. native-shim wires these to bus inbound
// messages in production; tests usually trigger the signals directly.
export function makeApiPlayer() {
  return {
    playing: makeSignal(),
    paused: makeSignal(),
    finished: makeSignal(),
    stopped: makeSignal(),
    canceled: makeSignal(),
    error: makeSignal(),
    seeking: makeSignal(),
    positionUpdate: makeSignal(),
    updateDuration: makeSignal(),
    buffering: makeSignal(),
    stateChanged: makeSignal(),
    videoPlaybackActive: makeSignal(),
    windowVisible: makeSignal(),
    onVideoRecangleChanged: makeSignal(),
    onMetaData: makeSignal(),

    _bufferedRanges: [],
    getBufferedRanges() { return this._bufferedRanges; },
    getPositionAsync(cb) { this._lastPositionCb = cb; },
  };
}

// Build a vm-runnable sandbox that doubles as window/globalThis.
export function makeWindow({ jmp, apiPlayer, document: docOverride } = {}) {
  const win = {};
  win.window = win;
  win.globalThis = win;
  win.console = { log: () => {}, warn: () => {}, error: () => {} };
  win.setTimeout = (fn, ms, ...args) => setTimeout(fn, ms, ...args);
  win.clearTimeout = (id) => clearTimeout(id);
  win.setInterval = (fn, ms, ...args) => setInterval(fn, ms, ...args);
  win.clearInterval = (id) => clearInterval(id);
  win.Promise = Promise;
  win.Date = Date;
  win.Math = Math;
  win.Number = Number;
  win.Boolean = Boolean;
  win.Array = Array;
  win.Object = Object;
  win.Error = Error;
  win.JSON = JSON;
  win.isNaN = isNaN;
  win.document = docOverride || makeDocument();
  if (jmp !== undefined) win.jmp = jmp;
  if (apiPlayer) win.api = { player: apiPlayer };
  return win;
}

export function makeDocument() {
  return {
    addEventListener: () => {},
    removeEventListener: () => {},
    querySelector: () => null,
    querySelectorAll: () => [],
    getElementById: () => null,
    createElement: () => makeElement(),
    body: makeElement(),
  };
}

function makeElement() {
  const el = {
    style: {},
    classList: {
      add: () => {},
      remove: () => {},
      contains: () => false,
    },
    children: [],
    appendChild: (c) => { el.children.push(c); return c; },
    insertBefore: (c) => { el.children.unshift(c); return c; },
    removeChild: () => {},
    remove: () => {},
    querySelector: () => null,
    setAttribute: () => {},
    addEventListener: () => {},
    parentNode: null,
  };
  return el;
}

// Minimal jellyfin-web Events shim: trigger(target, name, args) → handlers.
export function makeEvents() {
  const map = new Map();
  const ev = {
    on: (target, name, fn) => {
      const key = keyOf(target, name);
      if (!map.has(key)) map.set(key, []);
      map.get(key).push(fn);
    },
    off: (target, name, fn) => {
      const list = map.get(keyOf(target, name));
      if (!list) return;
      const i = list.indexOf(fn);
      if (i >= 0) list.splice(i, 1);
    },
    trigger: (target, name, args) => {
      ev.triggers.push({ target, name, args });
      const list = map.get(keyOf(target, name));
      if (!list) return;
      for (const fn of list.slice()) fn({ type: name, target }, ...(args || []));
    },
    triggers: [],
  };
  return ev;
}

function keyOf(target, name) {
  return `${name}::${target ? "T" : "_"}`;
}

export function makeAppSettings({ aspectRatio = undefined } = {}) {
  const store = new Map();
  const settings = {
    get: (k) => store.get(k),
    set: (k, v) => { store.set(k, v); },
    _store: store,
  };
  if (aspectRatio !== undefined) {
    let val = aspectRatio;
    settings.aspectRatio = (v) => {
      if (v !== undefined) val = v;
      return val;
    };
  }
  return settings;
}

// Bring a sandbox-realm object into the outer realm so deepEqual works.
function reify(v) {
  if (v == null) return v;
  return JSON.parse(JSON.stringify(v));
}

// Find the last `bus.send` call for a given message name (payload reified).
export function lastSend(bus, name) {
  for (let i = bus.send.calls.length - 1; i >= 0; i--) {
    if (bus.send.calls[i][0] === name) return reify(bus.send.calls[i][1] || {});
  }
  return null;
}

// All `bus.send` payloads for a name (reified).
export function sendsOf(bus, name) {
  return bus.send.calls
    .filter(c => c[0] === name)
    .map(c => reify(c[1] || {}));
}
