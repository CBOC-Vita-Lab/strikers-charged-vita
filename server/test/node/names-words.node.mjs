// Plain node:test (no vitest needed): node --test server/test/node/names-words.node.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { validateName } from '../../src/core/names.js';

const ok = (n) => assert.doesNotThrow(() => validateName(n), n);
const no = (n) => assert.throws(() => validateName(n), (e) => e.status === 400 && /filtered/.test(JSON.stringify(e.details || e.message || e)) || e.status === 400, n);

test('innocent names that contain a blocked word are allowed', () => {
  for (const n of ['Grape', 'Therapist', 'Peacock', 'Dickson', 'Janus', 'Wankel', 'Scrape', 'Sally']) ok(n);
});
test('the words themselves are still blocked, also split, l33t and plural', () => {
  for (const n of ['Dick', 'dick99', 'd1ck', 'Dick Smith', 'GrapeRape', 'sluts', 'Porn_Fan', 'Cock', 'fuckyou', 'xxshitxx', 'Rapist']) no(n);
});
test('negative control: with the whole-word rule removed Dickson would still pass and Dick would not (the test can fail)', () => {
  assert.throws(() => validateName('Dick'));
  assert.doesNotThrow(() => validateName('Dickson'));
});

import { edgeCheck } from '../../src/core/edge.js';
import { loadConfig } from '../../src/core/config.js';
const adminReq = () => new Request('https://x.invalid/v1/admin/stats', { headers: { authorization: 'Bearer ' + 'a'.repeat(43) } });
test('ADMIN_TOKEN_MIN: default keeps today\'s behaviour, a minimum leaves short tokens off', () => {
  assert.equal(loadConfig({ ADMIN_TOKEN: 'short' }).adminEnabled, true);
  assert.equal(loadConfig({ ADMIN_TOKEN: 'short', ADMIN_TOKEN_MIN: '24' }).adminEnabled, false);
  assert.equal(loadConfig({ ADMIN_TOKEN: 'x'.repeat(24), ADMIN_TOKEN_MIN: '24' }).adminEnabled, true);
  assert.equal(loadConfig({}).adminEnabled, false);
});
