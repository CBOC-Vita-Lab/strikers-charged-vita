// The admin token is sent only to a host this repo knows (or to loopback for the local fake). Anyone can run a server that
// answers /v1/info like ours, so "looks like our service" is never a reason to hand it the bearer token.
export const ADMIN_HOSTS = ['strikers.psvita.dev', 'msc-online.cboc-vita.workers.dev'];
export function adminBase(arg, extraHosts = (process.env.ADMIN_EXTRA_HOSTS || '').split(',').filter(Boolean)) {
  let u;
  try { u = new URL(arg); } catch { throw new Error('admin target: not a URL'); }
  const loop = ['localhost', '127.0.0.1', '[::1]'].includes(u.hostname);
  if (u.username || u.password) throw new Error('admin target: no credentials in the URL');
  if (!loop && u.protocol !== 'https:') throw new Error('admin target: https only');
  if (loop && u.protocol !== 'http:' && u.protocol !== 'https:') throw new Error('admin target: http(s) only');
  if (!loop && ![...ADMIN_HOSTS, ...extraHosts].includes(u.hostname)) throw new Error(`admin target: ${u.hostname} is not a known service host (set ADMIN_EXTRA_HOSTS to add one)`);
  return u.origin;
}
