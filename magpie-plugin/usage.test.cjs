const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const { test } = require("node:test");
const vm = require("node:vm");

const source = fs.readFileSync(path.join(__dirname, "usage.js"), "utf8");
const wait = () => new Promise(resolve => setImmediate(resolve));
const day = {
  date: "2026-01-02", from: "2026-01-02", calls: 2,
  input: 120, output: 30, cacheRead: 5_000_000_000, cacheWrite: 70,
};

function element(days) {
  const text = { textContent: "", dataset: { days }, listeners: {} };
  text.addEventListener = (name, callback) => { text.listeners[name] = callback; };
  text.setAttribute = () => {};
  return text;
}

function dashboard(read) {
  const ids = ["total", "dates", "input", "output", "cache-read", "cache-write", "refresh", "status"];
  const items = Object.fromEntries(ids.map(id => [id, element()]));
  const periods = [1, 7, 30].map(element);
  const document = {
    getElementById: id => items[id],
    querySelector: () => element(),
    querySelectorAll: selector => selector === "[data-days]" ? periods : [],
  };
  const magpie = { ready: Promise.resolve({ lang: "en" }), onTheme: () => {}, read };
  vm.runInNewContext(source, { window: { magpie }, document, setInterval: () => {} });
  return { items, periods };
}

test("adds both cache counters to the totals of Magpie's inclusive calendar spans", async () => {
  const requests = [];
  const { items, periods } = dashboard(async (scope, range) => {
    assert.equal(scope, "usage:day");
    requests.push({ ...range });
    return range.from ? { ...day, from: range.from, input: 2000, output: 40, cacheWrite: 700 } : day;
  });
  await wait();
  assert.equal(items.total.textContent, "5,000,000,220");
  assert.equal(items["cache-read"].textContent, "5,000,000,000");
  assert.equal(items["cache-write"].textContent, "70");
  periods[1].listeners.click();
  await wait();
  assert.equal(items.total.textContent, "5,000,002,740");
  assert.equal(items.dates.textContent, "2025-12-27 – 2026-01-02");
  periods[2].listeners.click();
  await wait();
  assert.equal(items.total.textContent, "5,000,002,740");
  assert.deepEqual(requests, [
    {}, {}, { from: "2025-12-27", date: "2026-01-02" },
    {}, { from: "2025-12-04", date: "2026-01-02" },
  ]);
});

test("an ignored span or refresh failure never displays inaccurate data", async () => {
  let unavailable = false;
  const { items, periods } = dashboard(async () => {
    if (unavailable) throw new Error("Raw upstream failure");
    return day;
  });
  await wait();
  periods[1].listeners.click();
  await wait();
  assert.equal(items.total.textContent, "—");
  assert.match(items.status.textContent, /unavailable/);
  unavailable = true;
  periods[0].listeners.click();
  await wait();
  assert.equal(items.total.textContent, "5,000,000,220");
  assert.match(items.status.textContent, /Showing the last reading/);
  assert.doesNotMatch(items.status.textContent, /Raw upstream failure/);
});

test("a true empty period renders zero", async () => {
  const { items } = dashboard(async () => ({ ...day, calls: 0, input: 0, output: 0, cacheRead: 0, cacheWrite: 0 }));
  await wait();
  assert.equal(items.total.textContent, "0");
  assert.match(items.status.textContent, /No usage/);
});
