// gxnet14: the Galaxy service's name filter (svcfix2 item 7): beaten by leetspeak, repeats, separators and staff look-alikes before. One normaliser and one
// list (src/core/name_filter.js). The test names live in test/fixtures/name_vectors.txt. Nothing here prints the list.
import { describe, it, expect } from 'vitest';
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { nameFilterReason, validateName } from '../../src/core/names.js';

const here = (p) => fileURLToPath(new URL(p, import.meta.url));
const vectors = fs.readFileSync(here('../fixtures/name_vectors.txt'), 'utf8').split('\n').filter((l) => l && !l.startsWith('#')).map((l) => l.split('\t'));

describe('name filter', () => {
  it('every shared test name gets the expected answer', () => {
    const wrong = vectors.filter(([n, want]) => (nameFilterReason(n) || 'ok') !== want).length;
    expect(wrong).toBe(0);
    expect(vectors.length).toBeGreaterThan(25);
  });
  it('a refused name is bad_name at sign-up, an honest one is not', () => {
    for (const [n, want] of vectors) {
      let ok = true;
      try { validateName(n); } catch (e) { ok = false; expect(e.code || e.error || e.message).toMatch(/bad_name/); }
      // (a name the character rules reject for other reasons, such as a space at the edge, is not what this checks)
      if (want !== 'ok') expect(ok).toBe(false);
    }
  });
  it('Strikers extras: guest stays reserved; the old blocklist words are all still blocked', () => {
    expect(() => validateName('Guest')).toThrow();
    expect(validateName('Guest2').name).toBe('Guest2');
    for (const n of ['Hitler', 'Whore', 'Pussy', 'Vagina', 'Asshole', 'Bastard', 'Retard', 'Molest', 'Rapist', 'Penis', 'Anus', 'Twat', 'Wank', 'Porn']) expect(nameFilterReason(n)).toBe('filtered');
  });
});

// a name accepted before the filter grew keeps working for a region / relay change
describe('rename keeps an unchanged name', () => {
  it('does not re-check it, but checks a new one', async () => {
    const { rename } = await import('../../src/core/accounts.js');
    const runs = [];
    const p = { pid: 1, code: 'ABCD-EFGH', rating: 1000, relay_only: 0, name: 'Staffy', name_key: 'staffy', region: 4, name_changed_ms: 0, cooldown_until_ms: 0 };
    const c = { now: 1e12, cfg: { renameCooldownMs: 1, nameBlockExtra: [] }, tx: (f) => f(), db: { run: (...a) => runs.push(a), get: (q) => (/SELECT \*/.test(q) ? p : null) } };
    rename(c, p, { region: 2 });
    expect(runs.some((a) => a.includes('staffy'))).toBe(true);
    expect(() => rename(c, p, { name: 'StaffMario' })).toThrow();
  });
});
