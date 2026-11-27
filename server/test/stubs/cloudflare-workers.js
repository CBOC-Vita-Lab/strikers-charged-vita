// The part of `cloudflare:workers` the core tests need to import src/worker.js and src/do.js in node.
export class DurableObject {
  constructor(ctx, env) { this.ctx = ctx; this.env = env; }
}
export const env = {};
