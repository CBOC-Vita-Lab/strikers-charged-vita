// The daily summary of problem reports (core/reports.js makeSummary / summaryView, wave 13 O7): counts by kind / build / app, no personal data.
import { describe, it, expect } from 'vitest';
import { makeServer, signUp } from '../helpers.js';
import { kindOf, KINDS } from '../../src/core/reports.js';

const ADMIN = 'test-admin-token-0123456789';
const DAY = 86400000;
const rep = (extra = {}) => ({ v: 1, app: '1.3.0', build: 'abcdef0123456789', name: 'Ana', log: 'T+1.0s link lost', events: 'session start', conn: 'router: cone', ...extra });

describe('kindOf', () => {
  it('first matching rule wins', () => {
    expect(kindOf({ note: 'it froze', events: 'desync', conn: '', log: '' })).toBe('note');
    expect(kindOf({ note: '', session: { e: 1, dr: 9 }, events: 'desync' })).toBe('session_error');
    expect(kindOf({ note: '', events: 'lockstep checksum mismatch at tick 4', log: '', conn: 'timeout' })).toBe('desync');
    expect(kindOf({ note: '', events: 'ok', log: '', conn: 'STUN timed out' })).toBe('connection');
    expect(kindOf({ note: '', events: 'ok', log: '', conn: 'fine', session: { dr: 2 } })).toBe('session_trouble');
    expect(kindOf({ note: '', events: 'ok', log: '', conn: 'fine' })).toBe('other');
    for (const k of ['note', 'session_error', 'desync', 'connection', 'session_trouble', 'other']) expect(KINDS).toContain(k);
  });
});

describe('daily report summary', () => {
  async function seed(s) {
    const a = await signUp(s, 'SumA');
    const b = await signUp(s, 'SumB');
    const c = await signUp(s, 'SumC');
    await s.call('POST', '/v1/report', rep({ build: 'aaaaaaaa', session: { d: 100, e: 1 } }), a.token);
    await s.call('POST', '/v1/report', rep({ build: 'aaaaaaaa', events: 'desync at tick 9' }), b.token);
    await s.call('POST', '/v1/report', rep({ build: 'bbbbbbbb', app: '1.3.1', conn: 'connection refused' }), c.token);
    await s.call('POST', '/v1/report', rep({ build: 'bbbbbbbb', note: 'help' }), c.token);
    return { a, b, c };
  }

  it('the cron writes yesterday\'s row with counts by kind, build and app; running twice rewrites the same row; nothing personal', async () => {
    const lines = [];
    const s = makeServer({}, { deps: { log: (l) => lines.push(String(l)) } });
    await seed(s);
    s.advance(DAY); // the next UTC day: the cron runs just after midnight
    const row = s.app.dailySummary();
    expect(row.reports).toBe(4);
    expect(row.byKind).toMatchObject({ session_error: 1, desync: 1, connection: 1, note: 1, other: 0, session_trouble: 0 });
    expect(row.byBuild).toEqual([{ name: 'aaaaaaaa', n: 2 }, { name: 'bbbbbbbb', n: 2 }]);
    expect(row.byApp).toEqual([{ name: '1.3.0', n: 3 }, { name: '1.3.1', n: 1 }]);
    expect(row.bytes).toBeGreaterThan(0);
    s.app.dailySummary();
    const stored = s.store.sql.exec(`SELECT * FROM report_summary`).toArray();
    expect(stored).toHaveLength(1);
    const text = JSON.stringify(stored) + lines.join('\n');
    for (const personal of ['SumA', 'SumB', 'SumC', 'Ana', 'help', 'link lost', 'desync at tick']) expect(text, personal).not.toContain(personal);
    expect(lines.some((l) => /^\[report-summary\] 2026-10-30: 4 reports \(note 1, session_error 1, desync 1, connection 1, session_trouble 0, other 0\)/.test(l))).toBe(true);
  });

  it('a day without reports is a row of zeros (the cron still ran); other days are not mixed in', async () => {
    const s = makeServer();
    await seed(s);
    s.advance(2 * DAY); // yesterday (relative to the cron) is empty
    expect(s.app.dailySummary().reports).toBe(0);
    s.advance(-DAY);
    expect(s.app.dailySummary().reports).toBe(4);
  });

  it('admin GET /v1/admin/report-summary: stored rows (newest first) and today so far; needs the admin token', async () => {
    const s = makeServer();
    await seed(s);
    s.advance(DAY);
    s.app.dailySummary();
    expect((await s.call('GET', '/v1/admin/report-summary')).status).toBe(401); // no token
    const bad = await s.call('GET', '/v1/admin/report-summary', undefined, 'wrong-token-wrong-token');
    expect(bad.status).toBe(401);
    const r = await s.call('GET', '/v1/admin/report-summary?days=3', undefined, ADMIN);
    expect(r.status).toBe(200);
    expect(r.data.today.reports).toBe(0);
    expect(r.data.days).toHaveLength(1);
    expect(r.data.days[0]).toMatchObject({ date: '2026-10-30', reports: 4 });
    expect(JSON.stringify(r.data)).not.toMatch(/"pid"|"name":"Sum/);
  });

  it('old reports without a kind (stored before this migration) count as unknown; summaries are kept 90 days', async () => {
    const s = makeServer();
    const a = await signUp(s, 'Old');
    s.store.sql.exec(`INSERT INTO reports (pid, created_ms, app, build, bytes, body) VALUES (?, ?, '1.3.0', 'cccccccc', 10, '{}')`, 1, s.clock.now);
    void a;
    s.advance(DAY);
    expect(s.app.dailySummary().byKind.unknown).toBe(1);
    s.advance(95 * DAY);
    s.app.sweep();
    s.advance(3600_000);
    s.app.sweep();
    expect(s.store.sql.exec(`SELECT COUNT(*) AS n FROM report_summary`).toArray()[0].n).toBe(0);
  });
});
