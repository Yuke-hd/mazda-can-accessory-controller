'use strict';

// Unit tests for the browser LED-strip renderer core in
// lib/replay/web/app.js. They run under plain Node.js with no packages and no
// DOM: the renderer core is a pure state reducer over WebSocket events.
//
// Usage: node web_emulator_renderer_tests.js <path-to-app.js>

const assert = require('node:assert/strict');
const path = require('node:path');

const appPath = process.argv[2];
if (!appPath) {
  console.error('usage: node web_emulator_renderer_tests.js <path-to-app.js>');
  process.exit(2);
}
const Renderer = require(path.resolve(appPath));

const header = (pixelCount, version = 1) =>
  JSON.stringify({ type: 'header', version, pixel_count: pixelCount });
const pixels = (timestampUs, values) =>
  JSON.stringify({ type: 'pixels', timestamp_us: timestampUs, pixels: values });
const end = () => JSON.stringify({ type: 'end' });
const playback = (paused, rate) => JSON.stringify({ type: 'playback', paused, rate });
const restart = () => JSON.stringify({ type: 'restart' });
const rejected = (reason) => JSON.stringify({ type: 'rejected', reason });
const solid = (count, rgb) => Array.from({ length: count }, () => rgb.slice());

function run(...events) {
  return events.reduce(Renderer.reduce, Renderer.initialState());
}
const open = { kind: 'open' };
const close = { kind: 'close' };
const error = { kind: 'error' };
const message = (data) => ({ kind: 'message', data });

function streaming(pixelCount, ...messages) {
  return run(open, message(header(pixelCount)), ...messages.map(message));
}

const tests = [];
function test(name, body) {
  tests.push({ name, body });
}

test('initial state is connecting with no pixels', () => {
  const state = Renderer.initialState();
  assert.equal(state.phase, 'connecting');
  assert.equal(state.pixels, null);
  assert.match(Renderer.statusText(state), /connecting/i);
});

test('header declares the pixel count and awaits the first frame', () => {
  const state = streaming(100);
  assert.equal(state.phase, 'streaming');
  assert.equal(state.pixelCount, 100);
  assert.equal(state.pixels, null);
  assert.equal(state.timestampUs, null);
});

test('incoming all-black frame renders black', () => {
  const state = streaming(100, pixels(0, solid(100, [0, 0, 0])));
  assert.equal(state.pixels.length, 100);
  for (const pixel of state.pixels) assert.equal(Renderer.rgbCss(pixel), 'rgb(0, 0, 0)');
});

test('exact incoming RGB values are preserved in strip order', () => {
  const frame = solid(100, [0, 0, 0]);
  frame[0] = [0, 8, 8];
  frame[37] = [0, 16, 16];
  frame[62] = [128, 16, 0];
  frame[99] = [255, 254, 1];
  const state = streaming(100, pixels(10000, frame));
  assert.deepEqual(state.pixels, frame);
  assert.equal(Renderer.rgbCss(state.pixels[0]), 'rgb(0, 8, 8)');
  assert.equal(Renderer.rgbCss(state.pixels[62]), 'rgb(128, 16, 0)');
  assert.equal(Renderer.rgbCss(state.pixels[99]), 'rgb(255, 254, 1)');
});

test('stored frame is a copy, not the parsed message', () => {
  const frame = solid(3, [1, 2, 3]);
  const state = streaming(3, pixels(0, frame));
  frame[0][0] = 99;
  assert.deepEqual(state.pixels[0], [1, 2, 3]);
});

test('each frame replaces the previous one without inventing intermediate states', () => {
  const first = solid(100, [0, 0, 0]);
  const second = solid(100, [0, 0, 0]);
  for (let index = 25; index < 75; ++index) second[index] = [0, 16, 16];
  const afterFirst = streaming(100, pixels(0, first));
  const afterSecond = Renderer.reduce(afterFirst, message(pixels(100000, second)));
  assert.deepEqual(afterFirst.pixels, first);
  assert.deepEqual(afterSecond.pixels, second);
  assert.equal(afterSecond.timestampUs, 100000);
});

test('the header pixel count, not a fixed strip length, drives rendering', () => {
  for (const count of [1, 7, 100, 144]) {
    const state = streaming(count, pixels(0, solid(count, [4, 5, 6])));
    assert.equal(state.phase, 'streaming');
    assert.equal(state.pixels.length, count);
  }
});

test('region guides describe the 100-pixel production layout only', () => {
  const guides = Renderer.regionGuides(100);
  assert.deepEqual(
    guides.map(({ start, count }) => [start, count]),
    [
      [0, 35],
      [35, 30],
      [65, 35],
    ],
  );
  assert.deepEqual(Renderer.regionGuides(7), []);
  assert.deepEqual(Renderer.regionGuides(144), []);
});

test('relative replay time is displayed in seconds with microsecond precision', () => {
  assert.equal(Renderer.formatReplayTime(null), '—');
  assert.equal(Renderer.formatReplayTime(0), '0.000000 s');
  assert.equal(Renderer.formatReplayTime(1234567), '1.234567 s');
  assert.equal(Renderer.formatReplayTime(600000), '0.600000 s');
});

test('unknown message types are ignored', () => {
  const frame = solid(2, [9, 9, 9]);
  const before = streaming(2, pixels(5, frame));
  const after = Renderer.reduce(before, message(JSON.stringify({ type: 'signal', value: 1 })));
  assert.deepEqual(after, before);
});

test('end marks the replay complete and keeps the final streamed frame', () => {
  const frame = solid(2, [0, 0, 0]);
  const state = run(open, message(header(2)), message(pixels(7, frame)), message(end()), close);
  assert.equal(state.phase, 'complete');
  assert.deepEqual(state.pixels, frame);
  assert.match(Renderer.statusText(state), /complete/i);
});

test('disconnect before end clears the display', () => {
  const state = run(open, message(header(2)), message(pixels(7, solid(2, [1, 1, 1]))), close);
  assert.equal(state.phase, 'disconnected');
  assert.equal(state.pixels, null);
  assert.equal(state.timestampUs, null);
  assert.match(Renderer.statusText(state), /disconnected/i);
});

test('socket error before end clears the display', () => {
  const state = run(open, message(header(2)), message(pixels(7, solid(2, [1, 1, 1]))), error);
  assert.equal(state.phase, 'disconnected');
  assert.equal(state.pixels, null);
});

test('a server that stops mid-replay never leaves the page looking live', () => {
  // The transcript a browser receives when the server fails or shuts down
  // mid-replay: records while playing, then the connection closes without end.
  const live = streaming(100, playback(false, 1), pixels(0, solid(100, [0, 0, 0])),
    pixels(100000, solid(100, [0, 16, 16])));
  assert.equal(live.phase, 'streaming');
  for (const failure of [close, error]) {
    const state = Renderer.reduce(live, failure);
    assert.equal(state.phase, 'disconnected');
    assert.equal(state.pixels, null);
    const available = Renderer.controlAvailability(state);
    assert.deepEqual([available.toggle, available.restart, available.rate], [false, false, false]);
    assert.match(Renderer.statusText(state), /^Disconnected before the replay completed/);
  }
});

const invalidCases = [
  ['malformed JSON', [open, message('{not json')]],
  ['non-object message', [open, message('[1,2,3]')]],
  ['pixels before header', [open, message(pixels(0, solid(2, [0, 0, 0])))]],
  ['unsupported version', [open, message(header(2, 2))]],
  ['zero pixel count', [open, message(header(0))]],
  ['fractional pixel count', [open, message(header(2.5))]],
  ['oversized pixel count', [open, message(header(1000000))]],
  ['duplicate header', [open, message(header(2)), message(header(2))]],
  ['too few pixels', [open, message(header(3)), message(pixels(0, solid(2, [0, 0, 0])))]],
  ['too many pixels', [open, message(header(1)), message(pixels(0, solid(2, [0, 0, 0])))]],
  ['channel above 255', [open, message(header(1)), message(pixels(0, [[256, 0, 0]]))]],
  ['negative channel', [open, message(header(1)), message(pixels(0, [[0, -1, 0]]))]],
  ['fractional channel', [open, message(header(1)), message(pixels(0, [[0, 0, 0.5]]))]],
  ['short pixel', [open, message(header(1)), message(pixels(0, [[0, 0]]))]],
  ['negative timestamp', [open, message(header(1)), message(pixels(-1, [[0, 0, 0]]))]],
  ['missing timestamp', [open, message(header(1)), message(JSON.stringify({ type: 'pixels', pixels: [[0, 0, 0]] }))]],
  [
    'timestamp going backwards',
    [open, message(header(1)), message(pixels(10, [[0, 0, 0]])), message(pixels(9, [[0, 0, 0]]))],
  ],
  ['pixels after end', [open, message(header(1)), message(end()), message(pixels(0, [[0, 0, 0]]))]],
  ['playback before header', [open, message(playback(false, 1))]],
  ['unsupported playback rate', [open, message(header(1)), message(playback(false, 3))]],
  ['non-boolean paused flag', [open, message(header(1)), message(playback('yes', 1))]],
  ['restart before header', [open, message(restart())]],
  ['header without restart after end', [open, message(header(1)), message(end()), message(header(1))]],
  ['non-string rejection', [open, message(header(1)), message(JSON.stringify({ type: 'rejected' }))]],
];

for (const [name, events] of invalidCases) {
  test(`invalid stream (${name}) clears the display and is marked invalid`, () => {
    const state = run(...events);
    assert.equal(state.phase, 'invalid', name);
    assert.equal(state.pixels, null, name);
    assert.equal(state.timestampUs, null, name);
    assert.match(Renderer.statusText(state), /invalid/i, name);
  });
}

test('an invalid stream stays invalid after later valid-looking messages and close', () => {
  const state = run(
    open,
    message(header(1)),
    message(pixels(0, [[0, 0, 0, 0]])),
    message(pixels(1, [[1, 1, 1]])),
    message(end()),
    close,
  );
  assert.equal(state.phase, 'invalid');
  assert.equal(state.pixels, null);
});

test('a previously shown frame is cleared when a later message is invalid', () => {
  const state = streaming(1, pixels(0, [[5, 5, 5]]), pixels(1, [[5, 5]]));
  assert.equal(state.phase, 'invalid');
  assert.equal(state.pixels, null);
});

test('the connection is closed only when the stream first becomes invalid', () => {
  const streamingState = streaming(1, pixels(0, [[5, 5, 5]]));
  const invalidState = Renderer.reduce(streamingState, message('{not json'));
  assert.equal(Renderer.shouldCloseConnection(streamingState, invalidState), true);
  assert.equal(Renderer.shouldCloseConnection(invalidState, invalidState), false);
  const disconnected = Renderer.reduce(streamingState, close);
  assert.equal(Renderer.shouldCloseConnection(streamingState, disconnected), false);
  assert.equal(Renderer.shouldCloseConnection(streamingState, streamingState), false);
});

test('acknowledged playback state is shown, never the requested one', () => {
  const playing = streaming(1, pixels(0, [[1, 1, 1]]), playback(false, 1));
  assert.equal(playing.paused, false);
  assert.equal(playing.rate, 1);
  const paused = Renderer.reduce(playing, message(playback(true, 0.25)));
  assert.equal(paused.paused, true);
  assert.equal(paused.rate, 0.25);
  assert.deepEqual(paused.pixels, playing.pixels);
  assert.equal(paused.timestampUs, playing.timestampUs);
  assert.match(Renderer.statusText(paused), /paused/i);
  for (const rate of [0.25, 0.5, 1, 2, 5]) {
    assert.equal(Renderer.reduce(playing, message(playback(false, rate))).rate, rate);
  }
});

test('control messages match the server protocol', () => {
  const playing = streaming(1, playback(false, 1));
  const paused = Renderer.reduce(playing, message(playback(true, 1)));
  assert.deepEqual(JSON.parse(Renderer.controlMessage(playing, 'toggle')), {
    type: 'control',
    command: 'pause',
  });
  assert.deepEqual(JSON.parse(Renderer.controlMessage(paused, 'toggle')), {
    type: 'control',
    command: 'play',
  });
  assert.deepEqual(JSON.parse(Renderer.controlMessage(playing, 'restart')), {
    type: 'control',
    command: 'restart',
  });
  assert.equal(
    Renderer.controlMessage(playing, '0.25'),
    '{"type":"control","command":"rate","rate":0.25}',
  );
  assert.deepEqual(Renderer.playbackRates, [0.25, 0.5, 1, 2, 5]);
});

test('controls are available only while a replay is live', () => {
  const unavailable = (state) => {
    const available = Renderer.controlAvailability(state);
    return !available.toggle && !available.restart && !available.rate;
  };
  assert.ok(unavailable(Renderer.initialState()));
  assert.ok(unavailable(run(open)));
  assert.ok(unavailable(run(open, message(header(1)), close)));
  assert.ok(unavailable(run(open, message('{not json'))));

  const live = Renderer.controlAvailability(streaming(1, playback(false, 1)));
  assert.deepEqual([live.toggle, live.restart, live.rate, live.toggleLabel],
    [true, true, true, 'Pause']);
  const paused = Renderer.controlAvailability(streaming(1, playback(true, 1)));
  assert.equal(paused.toggleLabel, 'Play');
  const complete = Renderer.controlAvailability(streaming(1, end()));
  assert.deepEqual([complete.toggle, complete.restart, complete.rate], [false, true, true]);
});

test('restart returns to the initial black state and accepts a fresh header', () => {
  for (const before of [
    streaming(1, pixels(0, [[9, 9, 9]]), playback(true, 2)),
    streaming(1, pixels(0, [[9, 9, 9]]), end()),
  ]) {
    const restarted = Renderer.reduce(before, message(restart()));
    assert.equal(restarted.phase, 'awaiting-header');
    assert.equal(restarted.pixels, null);
    assert.equal(restarted.timestampUs, null);
    const replaying = [header(1), playback(false, before.rate), pixels(0, [[0, 0, 0]])]
      .map(message)
      .reduce(Renderer.reduce, restarted);
    assert.equal(replaying.phase, 'streaming');
    assert.equal(replaying.paused, false);
    assert.equal(replaying.timestampUs, 0);
    assert.deepEqual(replaying.pixels, [[0, 0, 0]]);
  }
});

test('a rejected control is reported until the next acknowledged state', () => {
  const playing = streaming(1, playback(false, 1));
  const refused = Renderer.reduce(playing, message(rejected('unsupported_rate')));
  assert.equal(refused.phase, 'streaming');
  assert.match(Renderer.statusText(refused), /rejected \(unsupported_rate\)/);
  const accepted = Renderer.reduce(refused, message(playback(true, 1)));
  assert.doesNotMatch(Renderer.statusText(accepted), /rejected/);
});

// Minimal stand-ins for the DOM nodes LedStripView.render touches when the
// pixel count is unchanged, counting status writes (an aria-live region).
function fakeElements(pixelCount) {
  const status = {
    writes: 0,
    value: '',
    get textContent() {
      return this.value;
    },
    set textContent(text) {
      this.writes += 1;
      this.value = text;
    },
  };
  const strip = { children: Array.from({ length: pixelCount }, () => ({ style: {} })) };
  return {
    root: { dataset: {} },
    status,
    time: { textContent: '' },
    strip,
    guides: {},
    toggle: { disabled: true, textContent: '' },
    restart: { disabled: true },
    rate: { disabled: true, value: '1' },
  };
}

test('the view reflects control availability and the acknowledged rate', () => {
  const elements = fakeElements(1);
  const playing = streaming(1, playback(false, 1));
  Renderer.LedStripView.render(elements, playing, playing);
  assert.equal(elements.toggle.disabled, false);
  assert.equal(elements.toggle.textContent, 'Pause');
  assert.equal(elements.restart.disabled, false);
  assert.equal(elements.rate.disabled, false);

  // A choice still awaiting acknowledgement is left alone.
  elements.rate.value = '5';
  const paused = Renderer.reduce(playing, message(playback(true, 1)));
  Renderer.LedStripView.render(elements, paused, playing);
  assert.equal(elements.rate.value, '5');
  assert.equal(elements.toggle.textContent, 'Play');

  const slower = Renderer.reduce(paused, message(playback(true, 0.5)));
  Renderer.LedStripView.render(elements, slower, paused);
  assert.equal(elements.rate.value, '0.5');

  const disconnected = Renderer.reduce(slower, close);
  Renderer.LedStripView.render(elements, disconnected, slower);
  assert.equal(elements.toggle.disabled, true);
  assert.equal(elements.restart.disabled, true);
  assert.equal(elements.rate.disabled, true);
});

test('a rejected rate choice reverts to the acknowledged rate', () => {
  const elements = fakeElements(1);
  const playing = streaming(1, playback(false, 1));
  Renderer.LedStripView.render(elements, playing, playing);

  elements.rate.value = '5';
  const refused = Renderer.reduce(playing, message(rejected('unsupported_rate')));
  Renderer.LedStripView.render(elements, refused, playing);
  assert.equal(elements.rate.value, '1');

  // The same reason again is still a new rejection.
  elements.rate.value = '2';
  const refusedAgain = Renderer.reduce(refused, message(rejected('unsupported_rate')));
  Renderer.LedStripView.render(elements, refusedAgain, refused);
  assert.equal(elements.rate.value, '1');

  // Later frames while the notice is shown leave a new choice alone.
  elements.rate.value = '2';
  const later = Renderer.reduce(refusedAgain, message(pixels(1, [[1, 1, 1]])));
  Renderer.LedStripView.render(elements, later, refusedAgain);
  assert.equal(elements.rate.value, '2');
});

test('a control request is sent only on an open socket, otherwise the rate reverts', () => {
  const elements = fakeElements(1);
  const playing = streaming(1, playback(false, 0.5));
  const sent = [];
  const socket = { OPEN: 1, readyState: 1, send: (text) => sent.push(text) };

  elements.rate.value = '2';
  Renderer.LedStripView.requestControl(elements, playing, socket, '2');
  assert.deepEqual(sent.map((text) => JSON.parse(text)), [
    { type: 'control', command: 'rate', rate: 2 },
  ]);
  assert.equal(elements.rate.value, '2');

  socket.readyState = 2;
  elements.rate.value = '5';
  Renderer.LedStripView.requestControl(elements, playing, socket, '5');
  assert.equal(sent.length, 1);
  assert.equal(elements.rate.value, '0.5');
});

test('an unavailable rate control shows the acknowledged rate', () => {
  const elements = fakeElements(1);
  const playing = streaming(1, playback(false, 2));
  Renderer.LedStripView.render(elements, playing, playing);
  elements.rate.value = '5';
  const disconnected = Renderer.reduce(playing, close);
  Renderer.LedStripView.render(elements, disconnected, playing);
  assert.equal(elements.rate.value, '2');
});

test('the status text is rewritten only when it changes', () => {
  const elements = fakeElements(1);
  const first = streaming(1, pixels(0, [[1, 1, 1]]));
  const second = Renderer.reduce(first, message(pixels(1, [[2, 2, 2]])));
  const complete = Renderer.reduce(second, message(end()));
  elements.status.textContent = Renderer.statusText(first);
  elements.status.writes = 0;

  Renderer.LedStripView.render(elements, second, first);
  assert.equal(elements.status.writes, 0);
  assert.equal(elements.strip.children[0].style.backgroundColor, 'rgb(2, 2, 2)');

  Renderer.LedStripView.render(elements, complete, second);
  assert.equal(elements.status.writes, 1);
  assert.match(elements.status.textContent, /complete/i);
});

let failures = 0;
for (const { name, body } of tests) {
  try {
    body();
    console.log(`ok - ${name}`);
  } catch (caught) {
    failures += 1;
    console.log(`not ok - ${name}`);
    console.log(caught && caught.stack ? caught.stack : caught);
  }
}
console.log(`${tests.length - failures}/${tests.length} renderer tests passed`);
process.exit(failures === 0 ? 0 : 1);
