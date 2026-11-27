// The number of accounts, kept in memory: counting the table at every sign-up was a scan of every row (rows read are limited to 5 million a day on the free plan,
// and a table of 50,000 accounts costs 50,000 rows per sign-up). The object counts once when it starts and then adds and subtracts as it changes the table.
export function createFill(db) {
  let n = null;
  return {
    count() {
      if (n === null) n = db.get(`SELECT COUNT(*) AS n FROM profiles`).n;
      return n;
    },
    add(d) { if (n !== null) n += d; },
    reset() { n = null; },
  };
}
