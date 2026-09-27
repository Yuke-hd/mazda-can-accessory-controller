'use strict';

// Browser LED-strip view for the local replay emulator.
//
// This file is a pure renderer of streamed D1 records (header, pixels, end).
// It draws exactly the RGB values the server sends, in strip order, and never
// derives, mixes, or schedules LED states of its own. All vehicle decoding and
// lighting behaviour stays in the production C++ pipeline.
//
// Playback controls only send requests. The server owns the replay clock and
// acknowledges each change with a playback record; the view shows that state
// rather than assuming a request took effect.
const LedStripRenderer = (() => {
  const kProtocolVersion = 1;
  const kMaximumPixelCount = 4096;
  const kProductionPixelCount = 100;
  const kPlaybackRates = Object.freeze([0.25, 0.5, 1, 2, 5]);
  const kProductionRegions = Object.freeze([
    Object.freeze({ label: 'First 35', start: 0, count: 35 }),
    Object.freeze({ label: 'Center 30', start: 35, count: 30 }),
    Object.freeze({ label: 'Final 35', start: 65, count: 35 }),
  ]);

  class InvalidStream extends Error {}

  function ensure(condition, reason) {
    if (!condition) throw new InvalidStream(reason);
  }

  const isObject = (value) => value !== null && typeof value === 'object' && !Array.isArray(value);
  const isChannel = (value) => Number.isInteger(value) && value >= 0 && value <= 255;
  const isPixel = (value) => Array.isArray(value) && value.length === 3 && value.every(isChannel);
  const isTimestamp = (value) => Number.isSafeInteger(value) && value >= 0;
  const isPixelCount = (value) =>
    Number.isInteger(value) && value > 0 && value <= kMaximumPixelCount;

  function initialState() {
    return Object.freeze({
      phase: 'connecting',
      pixelCount: null,
      timestampUs: null,
      pixels: null,
      reason: null,
      paused: false,
      rate: 1,
      notice: null,
    });
  }

  const withChanges = (state, changes) => Object.freeze({ ...state, ...changes });
  const cleared = (state, phase, reason) =>
    withChanges(state, { phase, timestampUs: null, pixels: null, reason });

  function acceptHeader(state, record) {
    ensure(state.phase === 'awaiting-header', 'duplicate or unexpected header');
    ensure(record.version === kProtocolVersion, 'unsupported protocol version');
    ensure(isPixelCount(record.pixel_count), 'invalid pixel count');
    return withChanges(state, { phase: 'streaming', pixelCount: record.pixel_count });
  }

  function acceptPixels(state, record) {
    ensure(state.phase === 'streaming', 'pixels outside a streaming replay');
    ensure(isTimestamp(record.timestamp_us), 'invalid timestamp');
    ensure(state.timestampUs === null || record.timestamp_us >= state.timestampUs,
      'timestamp went backwards');
    ensure(Array.isArray(record.pixels) && record.pixels.length === state.pixelCount,
      'pixel count does not match header');
    ensure(record.pixels.every(isPixel), 'invalid RGB pixel');
    const pixels = Object.freeze(record.pixels.map((pixel) => Object.freeze(pixel.slice())));
    return withChanges(state, { timestampUs: record.timestamp_us, pixels });
  }

  function acceptEnd(state) {
    ensure(state.phase === 'streaming', 'end outside a streaming replay');
    return withChanges(state, { phase: 'complete' });
  }

  // The server's acknowledged playback state; it may arrive at any point after
  // the header, including once the replay is complete.
  function acceptPlayback(state, record) {
    ensure(state.pixelCount !== null, 'playback state before a header');
    ensure(typeof record.paused === 'boolean', 'invalid paused flag');
    ensure(kPlaybackRates.includes(record.rate), 'unsupported playback rate');
    return withChanges(state, { paused: record.paused, rate: record.rate, notice: null });
  }

  // The server interrupted (or finished) the replay and is starting again at
  // t=0 with a fresh header, so the display returns to its initial state.
  function acceptRestart(state) {
    ensure(state.phase === 'streaming' || state.phase === 'complete',
      'restart outside a replay');
    return withChanges(cleared(state, 'awaiting-header', null), { notice: null });
  }

  function acceptRejected(state, record) {
    ensure(typeof record.reason === 'string', 'invalid rejection');
    // A fresh object per rejection, so the view notices repeated reasons too.
    return withChanges(state, { notice: Object.freeze({ reason: record.reason }) });
  }

  const recordHandlers = Object.freeze({
    header: acceptHeader,
    pixels: acceptPixels,
    end: acceptEnd,
    playback: acceptPlayback,
    restart: acceptRestart,
    rejected: acceptRejected,
  });

  function acceptMessage(state, data) {
    let record;
    try {
      record = JSON.parse(data);
    } catch (_) {
      throw new InvalidStream('malformed JSON');
    }
    ensure(isObject(record), 'record is not an object');
    const handler = Object.prototype.hasOwnProperty.call(recordHandlers, record.type)
      ? recordHandlers[record.type]
      : null;
    return handler === null ? state : handler(state, record);
  }

  const kTerminalPhases = new Set(['complete', 'disconnected', 'invalid']);

  function acceptDisconnect(state) {
    if (kTerminalPhases.has(state.phase)) return state;
    return cleared(state, 'disconnected', null);
  }

  // Applies one WebSocket event: {kind: 'open' | 'message' | 'close' | 'error', data?}.
  function reduce(state, event) {
    if (state.phase === 'invalid') return state;
    if (event.kind === 'open') return withChanges(state, { phase: 'awaiting-header' });
    if (event.kind === 'close' || event.kind === 'error') return acceptDisconnect(state);
    if (event.kind !== 'message') return state;
    try {
      return acceptMessage(state, event.data);
    } catch (caught) {
      if (!(caught instanceof InvalidStream)) throw caught;
      return cleared(state, 'invalid', caught.message);
    }
  }

  const regionGuides = (pixelCount) =>
    pixelCount === kProductionPixelCount ? kProductionRegions : [];

  const rgbCss = ([red, green, blue]) => `rgb(${red}, ${green}, ${blue})`;

  function formatReplayTime(timestampUs) {
    if (timestampUs === null) return '—';
    const seconds = Math.floor(timestampUs / 1e6);
    const micros = String(timestampUs % 1e6).padStart(6, '0');
    return `${seconds}.${micros} s`;
  }

  const kStatusText = Object.freeze({
    connecting: () => 'Connecting to local replay…',
    'awaiting-header': () => 'Connected; waiting for the stream header',
    streaming: (state) =>
      `${state.paused ? 'Paused' : 'Streaming'} replay (${state.pixelCount} pixels)`,
    complete: () => 'Replay complete; showing the final streamed frame',
    disconnected: () => 'Disconnected before the replay completed; display cleared',
    invalid: (state) => `Invalid replay stream (${state.reason}); display cleared`,
  });

  function statusText(state) {
    const status = kStatusText[state.phase](state);
    return state.notice === null ? status : `${status}; control rejected (${state.notice.reason})`;
  }

  const kLivePhases = new Set(['streaming', 'complete']);

  // Which playback controls can act on the current stream.
  function controlAvailability(state) {
    const live = kLivePhases.has(state.phase);
    return Object.freeze({
      toggle: state.phase === 'streaming',
      restart: live,
      rate: live,
      toggleLabel: state.paused ? 'Play' : 'Pause',
    });
  }

  // The control message for a button or rate choice; the server validates it.
  function controlMessage(state, request) {
    if (request === 'toggle') {
      return JSON.stringify({ type: 'control', command: state.paused ? 'play' : 'pause' });
    }
    if (request === 'restart') return JSON.stringify({ type: 'control', command: 'restart' });
    return JSON.stringify({ type: 'control', command: 'rate', rate: Number(request) });
  }

  const formatRate = (rate) => String(rate);

  // An invalid stream is never recovered, so the view drops the connection
  // once, on the event that made the stream invalid.
  const shouldCloseConnection = (previous, next) =>
    previous.phase !== 'invalid' && next.phase === 'invalid';

  return Object.freeze({
    initialState,
    reduce,
    shouldCloseConnection,
    regionGuides,
    rgbCss,
    formatReplayTime,
    statusText,
    controlAvailability,
    controlMessage,
    formatRate,
    playbackRates: kPlaybackRates,
  });
})();

// DOM view: rebuilds the strip only when the declared pixel count changes and
// otherwise copies each streamed RGB value straight onto its pixel element.
const LedStripView = (() => {
  function buildStrip(elements, pixelCount) {
    elements.strip.replaceChildren();
    elements.guides.replaceChildren();
    if (pixelCount === null) return;
    elements.strip.style.setProperty('--pixel-count', String(pixelCount));
    elements.guides.style.setProperty('--pixel-count', String(pixelCount));
    for (let index = 0; index < pixelCount; ++index) {
      const pixel = document.createElement('span');
      pixel.className = 'pixel';
      pixel.title = `Pixel ${index}`;
      elements.strip.append(pixel);
    }
    for (const region of LedStripRenderer.regionGuides(pixelCount)) {
      const guide = document.createElement('span');
      guide.className = 'guide';
      guide.style.gridColumn = `${region.start + 1} / span ${region.count}`;
      guide.textContent = `${region.label} (${region.start}–${region.start + region.count - 1})`;
      elements.guides.append(guide);
    }
  }

  function paintPixels(elements, pixels) {
    const nodes = elements.strip.children;
    for (let index = 0; index < nodes.length; ++index) {
      nodes[index].style.backgroundColor = pixels === null ? '' : LedStripRenderer.rgbCss(pixels[index]);
    }
  }

  function renderControls(elements, state, previous) {
    const available = LedStripRenderer.controlAvailability(state);
    elements.toggle.disabled = !available.toggle;
    elements.toggle.textContent = available.toggleLabel;
    elements.restart.disabled = !available.restart;
    elements.rate.disabled = !available.rate;
    // Show only acknowledged rates, without undoing a choice still in flight:
    // a rejection or an unavailable control drops that choice.
    const rejectedNow = state.notice !== null && (previous === null || previous.notice !== state.notice);
    if (previous === null || previous.rate !== state.rate || rejectedNow || !available.rate) {
      showAcknowledgedRate(elements, state);
    }
  }

  function showAcknowledgedRate(elements, state) {
    elements.rate.value = LedStripRenderer.formatRate(state.rate);
  }

  // Sends a control request; one that cannot be sent leaves no pending choice.
  function requestControl(elements, state, socket, control) {
    if (socket.readyState !== socket.OPEN) {
      showAcknowledgedRate(elements, state);
      return;
    }
    socket.send(LedStripRenderer.controlMessage(state, control));
  }

  function render(elements, state, previous) {
    if (previous === null || previous.pixelCount !== state.pixelCount) {
      buildStrip(elements, state.pixelCount);
    }
    paintPixels(elements, state.pixels);
    elements.root.dataset.phase = state.phase;
    elements.root.dataset.live = String(state.pixels !== null);
    // #status is an aria-live region: rewriting unchanged text re-announces it.
    const status = LedStripRenderer.statusText(state);
    if (elements.status.textContent !== status) elements.status.textContent = status;
    elements.time.textContent = LedStripRenderer.formatReplayTime(state.timestampUs);
    renderControls(elements, state, previous);
  }

  return Object.freeze({ render, requestControl });
})();

function startLedStripEmulator() {
  const elements = {
    root: document.querySelector('#emulator'),
    status: document.querySelector('#status'),
    time: document.querySelector('#replay-time'),
    strip: document.querySelector('#strip'),
    guides: document.querySelector('#guides'),
    toggle: document.querySelector('#play-pause'),
    restart: document.querySelector('#restart'),
    rate: document.querySelector('#rate'),
  };
  let state = LedStripRenderer.initialState();
  let rendered = null;
  let renderPending = false;

  // Coalesce repaints to display refreshes. Skipped frames are simply not
  // shown; no state is synthesized between streamed frames.
  function scheduleRender() {
    if (renderPending) return;
    renderPending = true;
    requestAnimationFrame(() => {
      renderPending = false;
      LedStripView.render(elements, state, rendered);
      rendered = state;
    });
  }

  const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
  const socket = new WebSocket(`${scheme}://${location.host}/ws`);

  function dispatch(event) {
    const previous = state;
    state = LedStripRenderer.reduce(state, event);
    if (LedStripRenderer.shouldCloseConnection(previous, state)) socket.close();
    scheduleRender();
  }

  const request = (control) => LedStripView.requestControl(elements, state, socket, control);

  elements.toggle.onclick = () => request('toggle');
  elements.restart.onclick = () => request('restart');
  elements.rate.onchange = () => request(elements.rate.value);
  socket.onopen = () => dispatch({ kind: 'open' });
  socket.onmessage = (event) => dispatch({ kind: 'message', data: event.data });
  socket.onerror = () => dispatch({ kind: 'error' });
  socket.onclose = () => dispatch({ kind: 'close' });
  scheduleRender();
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = Object.freeze({ ...LedStripRenderer, LedStripView });
} else {
  startLedStripEmulator();
}
