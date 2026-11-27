import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { adminBase } from '../../scripts/admin-target.mjs';

describe('deploy guard: wrangler.toml', () => {
  const toml = readFileSync(new URL('../../wrangler.toml', import.meta.url), 'utf8').replace(/#.*$/gm, '');
  it('declares no routes, custom domains, zone or account (a piped `wrangler deploy` would publish them with replace_state)', () => {
    expect(toml).not.toMatch(/^\s*routes?\s*=/m);
    expect(toml).not.toMatch(/custom_domain|zone_name|zone_id|account_id|\[\[routes\]\]/);
  });
  it('turns the version preview URLs off (a second public way to reach each uploaded version; wave 14, from the Galaxy service)', () => {
    expect(toml).toMatch(/^\s*preview_urls\s*=\s*false\s*$/m);
  });
});

describe('admin token target', () => {
  it('accepts the known hosts and loopback only', () => {
    expect(adminBase('https://strikers.psvita.dev/')).toBe('https://strikers.psvita.dev');
    expect(adminBase('http://127.0.0.1:8787')).toBe('http://127.0.0.1:8787');
    for (const bad of ['https://evil.example', 'http://strikers.psvita.dev', 'https://strikers.psvita.dev.evil.example', 'https://u:p@strikers.psvita.dev', 'nope'])
      expect(() => adminBase(bad, [])).toThrow();
    expect(adminBase('https://x.example', ['x.example'])).toBe('https://x.example');
  });
});
