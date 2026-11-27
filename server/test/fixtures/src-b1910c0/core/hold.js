// Held polls: a request that has nothing to report yet waits inside the object (no CPU, no row) until something it watches changes or its
// time is up, then answers. One held request replaces up to a few dozen plain polls (client at 1.7 s: about 5 per 8 s).
//
// Waiters are keyed (`room:<CODE>`, `queue:<pid>`). `wake(key)` resolves them early. The timers are injectable (virtual time in tests and in
// scripts/estimate-requests.mjs); the park / resume hooks let a test scheduler know when a request is parked on a timer and when it runs again.

export function createHolds({ timers, onPark, onResume } = {}) {
  const t = timers || { setTimeout: (fn, ms) => globalThis.setTimeout(fn, ms), clearTimeout: (id) => globalThis.clearTimeout(id) };
  /** @type {Map<string, Set<{done: (why: string) => void}>>} */
  const waiters = new Map();
  let parked = 0;
  return {
    /** Resolves 'wake' when `wake(key)` is called, 'timeout' after `ms`. */
    wait(key, ms) {
      return new Promise((resolve) => {
        let finished = false;
        let timer = null;
        const w = {
          done: (why) => {
            if (finished) return;
            finished = true;
            if (timer !== null) t.clearTimeout(timer);
            const set = waiters.get(key);
            if (set) { set.delete(w); if (!set.size) waiters.delete(key); }
            parked--;
            if (onResume) onResume();
            resolve(why);
          },
        };
        let set = waiters.get(key);
        if (!set) waiters.set(key, (set = new Set()));
        set.add(w);
        parked++;
        if (onPark) onPark();
        timer = t.setTimeout(() => w.done('timeout'), Math.max(0, ms));
      });
    },
    wake(key) {
      const set = waiters.get(key);
      if (set) for (const w of [...set]) w.done('wake');
    },
    wakePrefix(prefix) {
      for (const [k, set] of [...waiters]) if (k.startsWith(prefix)) for (const w of [...set]) w.done('wake');
    },
    has: (key) => waiters.has(key),
    parked: () => parked,
  };
}
