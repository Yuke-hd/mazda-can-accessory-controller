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
