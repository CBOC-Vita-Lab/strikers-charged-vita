// The Durable Object: one instance ("main") owns all state, so every request is serialized and storage stays
// consistent without any locking. SQLite lives in the object (free plan: use new_sqlite_classes).

import { DurableObject } from 'cloudflare:workers';
import { createApp } from './core/app.js';

const SWEEP_MS = 60_000;

export class GameServer extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.app = createApp({
      sql: ctx.storage.sql,
      tx: (fn) => ctx.storage.transactionSync(fn),
      env,
    });
  }

  async fetch(request) {
    const resp = await this.app.handle(request);
    if ((await this.ctx.storage.getAlarm()) === null) {
      await this.ctx.storage.setAlarm(Date.now() + SWEEP_MS);
      this.app.budget.rows(Date.now(), 1); // setAlarm() writes a row
    }
    return resp;
  }

  /** Called by the daily cron through the Worker (RPC, not a route): writes yesterday's report summary row (core/reports.js). */
  async daily() {
    const row = this.app.dailySummary();
    return { day: row.date, reports: row.reports };
  }

  async alarm() {
    const more = this.app.sweep();
    if (more) {
      await this.ctx.storage.setAlarm(Date.now() + SWEEP_MS);
      this.app.budget.rows(Date.now(), 1);
    }
  }
}
