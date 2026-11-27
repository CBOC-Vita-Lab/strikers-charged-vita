// What the Worker remembers about the object's answers, so that a refused request never wakes the object again (worker.js):
//  * the daily budget level (`x-msc-budget`: browse-over | new-over | over, core/budget.js): the Worker answers the refused classes itself
//    for BLOCK_MAX_MS (or until midnight UTC), then asks the object again (one request) to see whether it is still so;
//  * blocks (`x-msc-block`: addr | addr-open | acct): an address or token over its daily ceiling is answered here until midnight UTC.
// Memory of one isolate only: another isolate learns on its own first refused request. Nothing here is stored or logged.

import { ipKey } from './util.js';
import { dayStart } from './calendar.js';
import { classify, refusedAt, overBudget, dailyCapped, BLOCK_MAX_MS } from './budget.js';

const DAY_MS = 86400000;
const MAX_BLOCKS = 5000;
const midnight = (now) => dayStart(now) + DAY_MS;

export function createGate() {
  let level = '';
  let until = 0;
  const blocks = new Map();
  return {
    /** The object answered `resp` to a request from `ip` with `authorization`: remember what it says about the budget and the blocks. */
    learn(resp, now, ip, authorization) {
      const lv = resp.headers.get('x-msc-budget') || '';
      if (lv) { level = lv; until = Math.min(now + BLOCK_MAX_MS, midnight(now)); } else level = '';
      const blk = resp.headers.get('x-msc-block');
      if (blk) {
        if (blocks.size >= MAX_BLOCKS) blocks.clear();
        const key = blk === 'acct' ? `t:${authorization || ''}` : blk === 'addr-open' ? `o:${ipKey(ip)}` : `a:${ipKey(ip)}`;
        if (blk !== 'acct' || authorization) blocks.set(key, midnight(now));
      }
    },
    /** An ApiError to answer with instead of asking the object, or null. */
    check(method, pathname, ip, authorization, now) {
      if (level && now < until && refusedAt(level, classify(method, pathname))) return overBudget(level, now);
      if (!blocks.size) return null;
      for (const [k, t] of [...blocks]) if (t <= now) blocks.delete(k);
      const open = classify(method, pathname) === 'open';
      const ipk = ipKey(ip || 'unknown');
      if (blocks.has(open ? `o:${ipk}` : `a:${ipk}`)) return dailyCapped(now, 'requests', open ? 'addr-open' : 'addr');
      if (authorization && blocks.has(`t:${authorization}`)) return dailyCapped(now, 'requests', 'acct');
      return null;
    },
    level: () => level,
    blocked: () => blocks.size,
    reset() { level = ''; until = 0; blocks.clear(); },
  };
}
