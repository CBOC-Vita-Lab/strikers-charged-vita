// Checks that need nothing but the request itself, shared by the Worker (so junk never wakes the Durable Object, which is
// where the free tier's request and row budgets are spent) and by the application (defence in depth, and the fake server).

import { ApiError, VERSION_RE } from './util.js';

export const MAX_BODY = 8192;

/**
 * Every endpoint: [method, pattern, auth, open].
 * auth: 'none' | 'player' (Bearer token, 43 url-safe chars) | 'admin' (Bearer secret).
 * open: no version header required (info, time, servers and the two public leaderboard routes, which a browser calls). Open routes are never
 * switched off, except /v1/public/leaderboard, which follows the `ranking` kill switch (app.js: `web`).
 * app.js asserts (and a test checks) that its routing table is exactly this list.
 */
export const ROUTE_META = [
  ['GET', '/v1/info', 'none', true],
  ['GET', '/v1/servers', 'none', true],
  ['GET', '/v1/time', 'none', true],
  ['GET', '/v1/register/challenge', 'none', false],
  ['POST', '/v1/register', 'none', false],
  ['GET', '/v1/me', 'player', false],
  ['PUT', '/v1/me', 'player', false],
  ['DELETE', '/v1/me', 'player', false],
  ['GET', '/v1/me/matches', 'player', false],
  ['GET', '/v1/players/:code', 'player', false],
  ['POST', '/v1/rooms', 'player', false],
  ['POST', '/v1/rooms/join', 'player', false],
  ['GET', '/v1/rooms/:code', 'player', false],
  ['POST', '/v1/rooms/:code/endpoint', 'player', false],
  ['POST', '/v1/rooms/:code/punch', 'player', false],
  ['POST', '/v1/rooms/:code/started', 'player', false],
  ['POST', '/v1/rooms/:code/leave', 'player', false],
  ['DELETE', '/v1/rooms/:code', 'player', false],
  ['POST', '/v1/ranked/queue', 'player', false],
  ['POST', '/v1/ranked/queue/resume', 'player', false],
  ['GET', '/v1/ranked/queue', 'player', false],
  ['DELETE', '/v1/ranked/queue', 'player', false],
  ['POST', '/v1/matches/:id/result', 'player', false],
  ['GET', '/v1/matches/:id', 'player', false],
  ['POST', '/v1/turn', 'player', false],
  ['POST', '/v1/turn/usage', 'player', false],
  ['POST', '/v1/rank/put', 'player', false],
  ['POST', '/v1/rank/get', 'player', false],
  ['GET', '/v1/public/leaderboard', 'none', true],
  ['GET', '/leaderboard', 'none', true],
  ['GET', '/v1/admin/stats', 'admin', false],
  ['GET', '/v1/admin/export', 'admin', false],
  ['POST', '/v1/admin/import', 'admin', false],
  ['POST', '/v1/admin/switches', 'admin', false],
  ['POST', '/v1/admin/ban', 'admin', false],
  ['GET', '/v1/admin/flags', 'admin', false],
  ['POST', '/v1/admin/flags/clear', 'admin', false],
];

/** pattern -> RegExp with named groups; one path segment is 1..64 characters. */
export const compilePattern = (pattern) => new RegExp(`^${pattern.replace(/:(\w+)/g, '(?<$1>[^/]{1,64})')}$`);

const COMPILED = ROUTE_META.map(([method, pattern, auth, open]) => ({ method, pattern, auth, open, re: compilePattern(pattern) }));

export const PLAYER_AUTH_RE = /^Bearer [A-Za-z0-9_-]{43}$/;
export const ADMIN_AUTH_RE = /^Bearer [\x21-\x7e]{1,256}$/;
const PROTO_RE = /^[0-9]{1,4}$/;
const BUILD_RE = /^[0-9a-fA-F]{8,64}$/;

/**
 * Format checks of the version headers. Returns an ApiError or null.
 * A malformed X-Msc-Proto used to be ignored (Number('abc') is NaN, which passed every comparison): version gating failed open.
 */
export function versionHeaderError(headers, { requireApp, open }) {
  const proto = headers.get('x-msc-proto');
  if (proto !== null && !PROTO_RE.test(proto)) {
    return new ApiError(400, 'bad_app_version', 'X-Msc-Proto must be a whole number such as 1', { field: 'x-msc-proto' });
  }
  const app = headers.get('x-msc-app');
  if (app !== null && !VERSION_RE.test(app)) return new ApiError(400, 'bad_app_version', 'X-Msc-App must look like 1.2.0');
  if (app === null && requireApp && !open) return new ApiError(400, 'bad_app_version', 'X-Msc-App header is required');
  const build = headers.get('x-msc-build');
  if (build !== null && !BUILD_RE.test(build)) return new ApiError(400, 'bad_app_version', 'X-Msc-Build must be a hex build hash');
  return null;
}

/**
 * What the Worker may decide without the object: unknown path, wrong method, oversized body, malformed Authorization
 * header, malformed or missing version headers. Returns an ApiError or null (= forward to the object).
 * @param {Request} request
 * @param {{ADMIN_TOKEN?: string, REQUIRE_APP_VERSION?: string}} [env]
 */
export function edgeCheck(request, env = {}) {
  const path = new URL(request.url).pathname;
  let route = null;
  const allow = [];
  for (const r of COMPILED) {
    if (!r.re.test(path)) continue;
    allow.push(r.method);
    if (r.method === request.method) route = r;
  }
  if (!route) {
    if (allow.length) return new ApiError(405, 'method_not_allowed', 'method not allowed', {}, { allow: allow.join(', ') });
    return new ApiError(404, 'not_found', 'no such endpoint');
  }
  // A body must announce its size (a chunked upload has no Content-Length: 411), and the announced size is capped. The object also reads the
  // stream with a hard cap (readBodyCapped), so a lying length cannot get past it either. The game's HTTPS client always sends Content-Length
  // with a body and none without one (np_https.cpp), so it is unaffected.
  if (request.method === 'POST' || request.method === 'PUT') {
    const cl = request.headers.get('content-length');
    if (cl === null ? request.body !== null : !/^\d{1,9}$/.test(cl)) {
      return new ApiError(411, 'length_required', 'Content-Length is required');
    }
  }
  if (Number(request.headers.get('content-length') || 0) > MAX_BODY) return new ApiError(413, 'too_large', 'request body too large');
  const auth = request.headers.get('authorization') || '';
  if (route.auth === 'admin') {
    if (!env.ADMIN_TOKEN) return new ApiError(404, 'not_found', 'no such endpoint');
    if (!ADMIN_AUTH_RE.test(auth)) return new ApiError(401, 'unauthorized', 'bad admin token', {}, { 'www-authenticate': 'Bearer' });
    return null;
  }
  const verr = versionHeaderError(request.headers, { requireApp: env.REQUIRE_APP_VERSION === '1', open: route.open });
  if (verr) return verr;
  if (route.auth === 'player' && !PLAYER_AUTH_RE.test(auth)) {
    return new ApiError(401, 'unauthorized', 'missing or malformed token', {}, { 'www-authenticate': 'Bearer' });
  }
  return null;
}
