// Player name rules. The game stores 11 UTF-16 units including the terminator (NetworkRankingSubmission.mName),
// so a name has at most 10 characters. Only plain ASCII so every font and every console renders it.

import { ApiError } from './util.js';

export const NAME_MIN = 2;
export const NAME_MAX = 10;

// A deliberately small filter: catches the obvious and the common l33t spellings, not a moderation system.
// Words of 4+ letters match as substrings of the folded name; shorter ones only as the whole name.
const BLOCK_SUBSTRING = [
  'fuck', 'shit', 'cunt', 'nigg', 'fagg', 'bitch', 'whore', 'slut', 'rape', 'rapist', 'nazi', 'hitler', 'cock',
  'dick', 'pussy', 'penis', 'vagina', 'anus', 'asshole', 'bastard', 'retard', 'twat', 'wank', 'porn', 'molest',
];
const BLOCK_EXACT = ['ass', 'fag', 'cum', 'tit', 'tits', 'sex', 'kkk', 'gay', 'jew', 'dyke', 'hoe'];
const RESERVED = [
  'admin', 'administrator', 'moderator', 'mod', 'system', 'server', 'staff', 'support', 'official', 'cboc', 'null',
  'undefined', 'nintendo', 'nextlevel', 'anonymous', 'guest',
];

/** Fold look-alikes so "Sally", "SaIly" and "sa11y" collide. Letters and digits only. */
export function foldKey(name) {
  const map = { 0: 'o', 1: 'l', 3: 'e', 4: 'a', 5: 's', 7: 't', 8: 'b', 9: 'g' };
  let out = '';
  for (const ch of name.toLowerCase()) {
    if (/[a-z0-9]/.test(ch)) out += map[ch] || ch;
    else if (ch === '$') out += 's';
    else if (ch === '@') out += 'a';
  }
  return out.replace(/i/g, 'l');
}

/**
 * @param {unknown} raw
 * @param {{extraBlock?: string[]}} [opts]
 * @returns {{name: string, key: string}}
 */
export function validateName(raw, opts = {}) {
  if (typeof raw !== 'string') throw new ApiError(400, 'bad_name', 'name must be a string', { reason: 'type' });
  const name = raw.trim();
  const bad = (reason, msg) => new ApiError(400, 'bad_name', msg, { reason });
  if (name.length < NAME_MIN) throw bad('too_short', `name needs at least ${NAME_MIN} characters`);
  if (name.length > NAME_MAX) throw bad('too_long', `name has at most ${NAME_MAX} characters`);
  if (!/^[A-Za-z0-9 ._-]+$/.test(name)) throw bad('charset', 'letters, digits, space, dot, dash and underscore only');
  if (!/^[A-Za-z0-9]/.test(name) || !/[A-Za-z0-9]$/.test(name)) throw bad('edges', 'must start and end with a letter or digit');
  if (/ {2,}/.test(name)) throw bad('spaces', 'no double spaces');
  if ((name.match(/[A-Za-z0-9]/g) || []).length < 2) throw bad('too_few_alnum', 'needs at least two letters or digits');
  const key = foldKey(name);
  if (RESERVED.map(foldKey).includes(key)) throw bad('reserved', 'that name is reserved');
  const extra = (opts.extraBlock || []).map(foldKey).filter((s) => s.length >= 3);
  if (BLOCK_EXACT.map(foldKey).includes(key) || [...BLOCK_SUBSTRING.map(foldKey), ...extra].some((w) => key.includes(w))) {
    throw bad('filtered', 'that name is not allowed');
  }
  return { name, key };
}
