// Run with: node --test native/volumeClamp.test.mjs
// The repo has no JS test runner, so this pulls the pure helper straight out of
// mpvVideoPlayer.js (which is a browser script, not a module) and evals it.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import assert from 'node:assert/strict';
import test from 'node:test';

const source = readFileSync(fileURLToPath(new URL('./mpvVideoPlayer.js', import.meta.url)), 'utf8');
const body = source
    .split('// --- volume-clamp:begin')[1]
    ?.split('// --- volume-clamp:end')[0]
    .replace(/^.*\n/, ''); // drop the rest of the marker line
assert.ok(body, 'volume-clamp markers missing from mpvVideoPlayer.js');
const clampVolume = new Function(`${body}; return clampVolume;`)();

test('allows boosting up to the configured maximum', () => {
    assert.equal(clampVolume(150, 200), 150);
    assert.equal(clampVolume(200, 200), 200);
});

test('clamps at the configured maximum', () => {
    assert.equal(clampVolume(250, 200), 200);
    assert.equal(clampVolume(150, 125), 125);
});

test('floors at zero', () => {
    assert.equal(clampVolume(-20, 200), 0);
    assert.equal(clampVolume(0, 200), 0);
});

test('falls back to 100 when the setting is unset', () => {
    assert.equal(clampVolume(150, undefined), 100);
    assert.equal(clampVolume(80, undefined), 80);
});

test('never clamps below 100 even for a bogus maximum', () => {
    assert.equal(clampVolume(100, 50), 100);
    assert.equal(clampVolume(100, 0), 100);
    assert.equal(clampVolume(100, 'nonsense'), 100);
});

test('accepts numeric strings, matching the old Number() coercion', () => {
    assert.equal(clampVolume('150', 200), 150);
    assert.equal(clampVolume('150', '200'), 150);
});

test('returns null for non-numeric input so setVolume ignores it', () => {
    assert.equal(clampVolume(undefined, 200), null);
    assert.equal(clampVolume('loud', 200), null);
    assert.equal(clampVolume(NaN, 200), null);
});
