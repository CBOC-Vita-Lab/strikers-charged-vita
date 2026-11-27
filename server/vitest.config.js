import { defineConfig } from 'vitest/config';
import { fileURLToPath } from 'node:url';
import { extractKey, DEFAULT_SOURCE } from './scripts/extract-hmac-key.mjs';

// The ranking digest key is read from the game source so the tests use the real value without it being copied into the repo.
const HMAC = extractKey(DEFAULT_SOURCE);

// The workerd project needs @cloudflare/vitest-plugin. Where it is not installed (a plain container with only vitest) the
// core project still runs: `npm test` skips the worker project and says so; `npm run test:worker` then fails loudly.
let cloudflareTest = null;
try { ({ cloudflareTest } = await import('@cloudflare/vitest-plugin')); } catch (e) { if (e?.code !== 'ERR_MODULE_NOT_FOUND') throw e; }
const only = process.argv.some((a) => a === '--project=worker' || a === 'worker') || process.argv.join(' ').includes('--project worker');
if (!cloudflareTest && !process.env.MSC_QUIET_NO_WORKERD) console.warn('[vitest.config] @cloudflare/vitest-plugin not installed: the workerd project (test/worker) is skipped; core project only.');

export default defineConfig({
  test: {
    projects: [
      {
        resolve: { alias: { 'cloudflare:workers': fileURLToPath(new URL('./test/stubs/cloudflare-workers.js', import.meta.url)) } }, // core tests import src/worker.js with a fake object namespace
        test: { name: 'core', include: ['test/core/**/*.test.js'], environment: 'node', testTimeout: 20000 },
      },
      ...(cloudflareTest || only ? [{
        plugins: [
          cloudflareTest({
            wrangler: { configPath: './wrangler.toml' },
            miniflare: {
              bindings: {
                RANK_HMAC_KEY_HEX: HMAC,
                ADMIN_TOKEN: 'test-admin-token-0123456789',
                TURN_PROVIDER: 'stub',
                ALLOW_TEST_CLOCK: '1',
                RATE_LIMIT_SCALE: '1000',
                REQUIRE_APP_VERSION: '0', // production sets 1 in wrangler.toml; this suite sends no version headers
                MIN_MATCH_SEC: '0',
              },
            },
          }),
        ],
        test: { name: 'worker', include: ['test/worker/**/*.test.js'], testTimeout: 30000 },
      }] : []),
    ],
  },
});
