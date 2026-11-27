// Optional light proof of work for POST /v1/register (REGISTER_POW_BITS, off by default).
//
// Stateless server nonce: challenge = "<expMs>.<rand16hex>.<mac16hex>", mac = HMAC-SHA256(serverKey, "<expMs>.<rand>.<ipKey>")
// truncated to 16 hex. It is bound to the client address (an IPv6 /64) and valid for CHALLENGE_TTL_MS, once.
// Client work: find a string `nonce` (1..20 characters of [0-9A-Za-z]) such that SHA-256(utf8(challenge + ":" + nonce))
// starts with at least `bits` zero bits. Expected cost 2^bits hashes (16..20 bits is well under a second on a PC; budget a
// few seconds on a Vita). The server key is generated once and kept in the settings table.

import { ApiError, randomBytes, hex, hmacSha256Hex, leadingZeroBits, safeEqual, te } from './util.js';

export const CHALLENGE_TTL_MS = 120_000;
const KEY_NAME = 'pow_key';
const NONCE_RE = /^[0-9A-Za-z]{1,20}$/;

function serverKey(c) {
  const r = c.db.get(`SELECT value FROM settings WHERE key = ?`, KEY_NAME);
  if (r) return Uint8Array.from(r.value.match(/../g).map((h) => parseInt(h, 16)));
  const k = randomBytes(32);
  c.db.run(`INSERT INTO settings (key, value) VALUES (?, ?)`, KEY_NAME, hex(k));
  return k;
}

export async function issueChallenge(c, ipk) {
  const bits = c.cfg.register.powBits;
  if (!bits) return { bits: 0 };
  const exp = c.now + CHALLENGE_TTL_MS;
  const rand = hex(randomBytes(8));
  const mac = (await hmacSha256Hex(serverKey(c), `${exp}.${rand}.${ipk}`)).slice(0, 16);
  return {
    bits,
    algo: 'sha256',
    challenge: `${exp}.${rand}.${mac}`,
    expiresMs: exp,
    rule: 'sha256(challenge + ":" + nonce) must start with `bits` zero bits; send {"pow":{"challenge","nonce"}} in the register body',
  };
}

/** Verifies the `pow` member of a register body. Throws 428 pow_required (with a fresh challenge) or 400 bad_pow. */
export async function verifyPow(c, ipk, pow) {
  const bits = c.cfg.register.powBits;
  if (!bits) return;
  if (pow === undefined) {
    throw new ApiError(428, 'pow_required', 'solve the proof of work and resend', await issueChallenge(c, ipk));
  }
  const bad = (reason) => new ApiError(400, 'bad_pow', 'proof of work rejected', { reason });
  if (typeof pow !== 'object' || pow === null || typeof pow.challenge !== 'string' || typeof pow.nonce !== 'string') throw bad('format');
  if (Object.keys(pow).some((k) => k !== 'challenge' && k !== 'nonce')) throw bad('format');
  const m = /^(\d{10,16})\.([0-9a-f]{16})\.([0-9a-f]{16})$/.exec(pow.challenge);
  if (!m || !NONCE_RE.test(pow.nonce)) throw bad('format');
  const exp = Number(m[1]);
  if (exp < c.now) throw bad('expired');
  if (exp > c.now + CHALLENGE_TTL_MS + 1000) throw bad('format');
  const mac = (await hmacSha256Hex(serverKey(c), `${m[1]}.${m[2]}.${ipk}`)).slice(0, 16);
  if (!(await safeEqual(mac, m[3]))) throw bad('mac');
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', te.encode(`${pow.challenge}:${pow.nonce}`)));
  if (leadingZeroBits(digest) < bits) throw bad('hash');
  if (c.powUsed.has(m[2])) throw bad('used');
  c.powUsed.set(m[2], exp);
  if (c.powUsed.size > 5000) for (const [k, e] of c.powUsed) if (e < c.now) c.powUsed.delete(k);
}

// exported for the tests and for a reference solver
export async function solve(challenge, bits, limit = 50_000_000) {
  for (let i = 0; i < limit; i++) {
    const nonce = i.toString(36);
    const d = new Uint8Array(await crypto.subtle.digest('SHA-256', te.encode(`${challenge}:${nonce}`)));
    if (leadingZeroBits(d) >= bits) return nonce;
  }
  throw new Error('no solution');
}
