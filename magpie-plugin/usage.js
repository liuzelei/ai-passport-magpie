"use strict";

const text = {
  en: {
    title: "Token Usage Badge", refresh: "Refresh", today: "Today",
    week: "Last 7 days", month: "Last 30 days", period: "Usage period",
    total: "Total tokens", input: "Uncached input", output: "Output",
    cacheRead: "Cache reads", cacheWrite: "Cache writes",
    formula: "Total includes cache reads and writes, matching Magpie Usage.",
    loading: "Reading Magpie usage…", synced: "Updated", empty: "No usage in this period",
    error: "Magpie usage is unavailable. Try refreshing.",
    stale: "Refresh failed. Showing the last reading from",
    outside: "Open this page from the Magpie sidebar.",
  },
  zh: {
    title: "Token 用量牌", refresh: "刷新", today: "今天",
    week: "近 7 天", month: "近 30 天", period: "统计时段",
    total: "Token 总量", input: "未缓存输入", output: "输出",
    cacheRead: "缓存读取", cacheWrite: "缓存写入",
    formula: "总量包含缓存读取和写入，与 Magpie 用量页一致。",
    loading: "正在读取 Magpie 用量…", synced: "更新于", empty: "此时段暂无用量",
    error: "无法读取 Magpie 用量，请重试。",
    stale: "刷新失败，上次读取时间为",
    outside: "请在 Magpie 侧边栏打开此页面。",
  },
};

function firstDate(date, days) {
  const boundary = new Date(`${date}T00:00:00Z`);
  if (Number.isNaN(boundary.getTime()) || boundary.toISOString().slice(0, 10) !== date) {
    throw new Error("Invalid usage date");
  }
  boundary.setUTCDate(boundary.getUTCDate() - days + 1);
  return boundary.toISOString().slice(0, 10);
}

async function readUsage(api, days) {
  // Let Magpie determine today so the card and page share its day-start setting.
  const day = await api.read("usage:day", {});
  const date = day.date;
  const from = firstDate(date, days);
  const usage = days === 1 ? day : await api.read("usage:day", { from, date });
  const { input, output, cacheRead, cacheWrite, calls } = usage;
  const total = input + output + cacheRead + cacheWrite;
  if (usage.date !== date || (usage.from ?? usage.date) !== from ||
      [input, output, cacheRead, cacheWrite, calls, total].some(value => !Number.isSafeInteger(value) || value < 0)) {
    throw new Error("Invalid usage counters or period");
  }
  return { input, output, cacheRead, cacheWrite, total, calls, from, date, updatedAt: new Date() };
}

function startDashboard(api, root) {
  const refreshButton = root.getElementById("refresh");
  const periodBar = root.querySelector(".period-bar");
  const periods = [...root.querySelectorAll("[data-days]")];
  const fields = ["total", "dates", "input", "output", "cache-read", "cache-write", "status"];
  const elements = Object.fromEntries(fields.map(id => [id, root.getElementById(id)]));
  const labels = root.querySelectorAll("[data-text]");
  const samples = new Map();
  let locale = "en";
  let selected = 1;
  let generation = 0;
  let loading = false;
  let failed = false;

  function render() {
    const words = text[locale];
    const format = new Intl.NumberFormat(locale);
    const sample = samples.get(selected);
    const show = value => sample ? format.format(value) : "—";
    elements.total.textContent = show(sample?.total);
    elements.input.textContent = show(sample?.input);
    elements.output.textContent = show(sample?.output);
    elements["cache-read"].textContent = show(sample?.cacheRead);
    elements["cache-write"].textContent = show(sample?.cacheWrite);
    elements.dates.textContent = !sample ? "" : sample.from === sample.date ? sample.date : `${sample.from} – ${sample.date}`;
    for (const button of periods) {
      button.setAttribute("aria-pressed", String(Number(button.dataset.days) === selected));
    }
    refreshButton.disabled = loading;
    const time = sample?.updatedAt.toLocaleTimeString(locale, { hour12: false, hour: "2-digit", minute: "2-digit" });
    elements.status.dataset.state = failed ? "error" : "";
    if (loading) {
      elements.status.textContent = words.loading;
    } else if (failed) {
      elements.status.textContent = sample ? `${words.stale} ${time}` : words.error;
    } else {
      elements.status.textContent = sample?.calls ? `${words.synced} ${time}` : words.empty;
    }
  }

  async function refresh() {
    const days = selected;
    const request = ++generation;
    loading = true;
    failed = false;
    render();
    try {
      const sample = await readUsage(api, days);
      if (request === generation) samples.set(days, sample);
    } catch {
      if (request === generation) failed = true;
    } finally {
      if (request === generation) {
        loading = false;
        render();
      }
    }
  }

  function translate({ lang }) {
    locale = lang?.toLowerCase().startsWith("zh") ? "zh" : "en";
    for (const element of labels) element.textContent = text[locale][element.dataset.text];
    periodBar.setAttribute("aria-label", text[locale].period);
  }

  if (!api) {
    elements.status.textContent = text.en.outside;
    return;
  }

  api.ready.then(theme => {
    translate(theme);
    api.onTheme(nextTheme => { translate(nextTheme); render(); });
    refreshButton.addEventListener("click", refresh);
    for (const button of periods) {
      button.disabled = false;
      button.addEventListener("click", () => {
        selected = Number(button.dataset.days);
        refresh();
      });
    }
    refresh();
    setInterval(() => {
      if (api.visible !== false && !root.hidden && !loading) refresh();
    }, 60_000);
  }, () => {
    elements.status.dataset.state = "error";
    elements.status.textContent = text[locale].error;
  });
}

startDashboard(window.magpie, document);
