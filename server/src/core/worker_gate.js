// What the Worker remembers about the object's answers, so that a refused request never wakes the object again (worker.js):
//  * the daily budget level (`x-msc-budget`: browse-over | new-over | over, core/budget.js): the Worker answers the refused classes itself
//    for BLOCK_MAX_MS (or until midnight UTC), then asks the object again (one request) to see whether it is still so;
//  * blocks (`x-msc-block`: addr | addr-open | acct): an address or token over its daily ceiling is answered here until midnight UTC.
// Memory of one isolate only: another isolate learns on its own first refused request. Nothing here is stored or logged.

import { ipKey, ApiError } from './util.js';
import { dayStart } from './calendar.js';
import { classify, refusedAt, overBudget, dailyCapped, BLOCK_MAX_MS } from './budget.js';

const DAY_MS = 86400000;
const MAX_BLOCKS = 5000;
const midnight = (now) => dayStart(now) + DAY_MS;

export function createGate() {
  let level = '';
  let until = 0;
  const blocks = new Map();
  const good = new Set(); // tokens that got a good answer from this isolate: they pass an `addr-bad` block (a NAT's honest consoles keep working)
  return {
    /** The object answered `resp` to a request from `ip` with `authorization`: remember what it says about the budget and the blocks. */
    learn(resp, now, ip, authorization) {
      const lv = resp.headers.get('x-msc-budget') || '';
      if (lv) { level = lv; until = Math.min(now + BLOCK_MAX_MS, midnight(now)); } else level = '';
      const blk = resp.headers.get('x-msc-block');
      const adm = blk && /^adm:(\d{1,5})$/.exec(blk);   // gxnet14: the object refused this address's admin requests for N seconds
      if (adm) {
        if (blocks.size >= MAX_BLOCKS) blocks.clear();
        blocks.set(`m:${ipKey(ip)}`, now + Number(adm[1]) * 1000);
      } else if (blk) {
        if (blocks.size >= MAX_BLOCKS) blocks.clear();
        const key = blk === 'acct' ? `t:${authorization || ''}` : blk === 'addr-open' ? `o:${ipKey(ip)}` : blk === 'addr-bad' ? `b:${ipKey(ip)}` : `a:${ipKey(ip)}`;
        if (blk !== 'acct' || authorization) blocks.set(key, midnight(now));
      }
      if (authorization && resp.status < 400) { if (good.size >= MAX_BLOCKS) good.clear(); good.add(authorization); }
    },
    /** An ApiError to answer with instead of asking the object, or null. */
    check(method, pathname, ip, authorization, now) {
      if (level && now < until) {
        const klass = classify(method, pathname);
        if (refusedAt(level, klass)) return overBudget(level, now, klass);
      }
      if (!blocks.size) return null;
      for (const [k, t] of [...blocks]) if (t <= now) blocks.delete(k);
      if (pathname.startsWith('/v1/admin/') && blocks.has(`m:${ipKey(ip || 'unknown')}`)) {
        const wait = Math.max(1, Math.ceil((blocks.get(`m:${ipKey(ip || 'unknown')}`) - now) / 1000));
        return new ApiError(429, 'rate_limited', 'slow down', { retryAfterSec: wait }, { 'retry-after': String(wait) });
      }
      const open = classify(method, pathname) === 'open';
      const ipk = ipKey(ip || 'unknown');
      if (!open && blocks.has(`b:${ipk}`) && !(authorization && good.has(authorization))) return dailyCapped(now, 'requests', 'addr-bad');
      if (blocks.has(open ? `o:${ipk}` : `a:${ipk}`)) return dailyCapped(now, 'requests', open ? 'addr-open' : 'addr');
      if (authorization && blocks.has(`t:${authorization}`)) return dailyCapped(now, 'requests', 'acct');
      return null;
    },
    level: () => level,
    blocked: () => blocks.size,
    reset() { level = ''; until = 0; blocks.clear(); good.clear(); },
  };
}
