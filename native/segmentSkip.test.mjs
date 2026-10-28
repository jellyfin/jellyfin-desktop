// Run with: node --test native/segmentSkip.test.mjs
// The repo has no JS test runner, so this pulls the pure helper straight out of
// mpvVideoPlayer.js (which is a browser script, not a module) and evals it.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import test from 'node:test';

const source = readFileSync(fileURLToPath(new URL('./mpvVideoPlayer.js', import.meta.url)), 'utf8');
const body = source
    .split('// --- segment-skip:begin')[1]
    ?.split('// --- segment-skip:end')[0]
    .replace(/^.*\n/, ''); // drop the rest of the marker line
assert.ok(body, 'segment-skip markers missing from mpvVideoPlayer.js');
const pickSegmentSkip = new Function(`${body}; return pickSegmentSkip;`)();

const INTRO = { id: 'i', type: 'Intro', start: 30_000, end: 90_000 };
const OUTRO = { id: 'o', type: 'Outro', start: 600_000, end: 660_000 };
const DURATION = 660_000;
const opts = { skipIntro: true, skipOutro: true, outroDelayMs: 5000 };
const pick = (segs, time, o = opts, skipped = new Set()) =>
    pickSegmentSkip(segs, time, DURATION, o, skipped);

test('skips an intro that is playing', () => {
    assert.deepEqual(pick([INTRO], 35_000), { id: 'i', target: 90_000 });
});

test('leaves the intro alone when disabled', () => {
    assert.equal(pick([INTRO], 35_000, { ...opts, skipIntro: false }), null);
});

test('waits out the outro delay', () => {
    assert.equal(pick([OUTRO], 602_000), null);
    assert.ok(pick([OUTRO], 606_000));
});

test('lands before EOF so playback finishes', () => {
    assert.deepEqual(pick([OUTRO], 606_000), { id: 'o', target: DURATION - 500 });
});

test('skips each segment only once', () => {
    assert.equal(pick([INTRO], 35_000, opts, new Set(['i'])), null);
});

test('ignores a segment that is nearly over', () => {
    assert.equal(pick([INTRO], 89_500), null);
});
