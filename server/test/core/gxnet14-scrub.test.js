// gxnet14: the report scrubber with the rules of the Galaxy service's maskText (wave 14). Every line below slipped through the older word-boundary rules.
import { describe, it, expect } from 'vitest';
import { scrubText } from '../../src/core/reports.js';

const TOKEN = 'q8Zk3mFv0pLw7xYtR2uN5oJcH1dGb4aE9sIeKfMh6Vc';
describe('scrubText (second line of defence for problem reports)', () => {
  it('addresses glued to letters or underscores, with ports', () => {
    for (const t of ['ip_1.2.3.4', 'x192.168.1.1', '1.2.3.4b', 'peer=10.0.0.5:41100', 'a 1.2.3.4:5 b']) expect(scrubText(t)).not.toMatch(/\d+\.\d+\.\d+\.\d+/);
    expect(scrubText('a 1.2.3.4:5 b')).toBe('a <address> b');
  });
  it('IPv6 (also with an IPv4 tail), but not a time of day or a version', () => {
    expect(scrubText('via fe80::1c2d:3eff:fe4f:5a6b ok')).toBe('via <address> ok');
    expect(scrubText('v4in6 ::ffff:192.0.2.128 end')).toBe('v4in6 <address> end');
    expect(scrubText('2001:db8:0:0:0:0:0:1')).toBe('<address>');
    expect(scrubText('at 12:34:56 build 1.3.0 key=a:b')).toBe('at 12:34:56 build 1.3.0 key=a:b');
  });
  it('MAC addresses in colon, dash and dotted spelling', () => {
    for (const m of ['aa:bb:cc:dd:ee:ff', 'AA-BB-CC-DD-EE-FF', 'aabb.ccdd.eeff']) expect(scrubText(`mac ${m} x`)).toBe('mac <address> x');
  });
  it('e-mail addresses, bearer tokens, token-shaped and long hex runs', () => {
    expect(scrubText('mail me at some.one+x@example.co.uk now')).toBe('mail me at <email> now');
    expect(scrubText(`Authorization: Bearer ${TOKEN}`)).toBe('Authorization: Bearer <secret>');
    expect(scrubText(`t=${TOKEN}.`)).not.toContain(TOKEN);
    expect(scrubText('0123456789abcdef'.repeat(4))).toBe('<secret>');
  });
  it('profile / pid / account ids of four or more digits; short numbers stay', () => {
    expect(scrubText('profile 123456 pid=98765 account: 4711')).toBe('profile <id> pid=<id> account: <id>');
    expect(scrubText('player 12 pid4711')).toBe('player 12 pid4711');
  });
  it('control characters go, newline and tab stay; the result is idempotent', () => {
    expect(scrubText('a\u0000b\u001b[31mc\nd\te')).toBe('ab[31mc\nd\te');
    const once = scrubText(`x ${TOKEN} 1.2.3.4 aa:bb:cc:dd:ee:ff me@x.org profile 123456`);
    expect(scrubText(once)).toBe(once);
  });
  it('linear on a 48 KB log of hostile shapes', () => {
    const t0 = Date.now();
    scrubText('a:'.repeat(20000) + '1.'.repeat(10000) + 'f'.repeat(30000) + '@'.repeat(5000));
    expect(Date.now() - t0).toBeLessThan(3000);
  });
});
