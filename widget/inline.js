(() => {
  'use strict';
  window.__codexDashboard?.dispose?.();

  // Codex desktop: composer-controls / app-primary expose this root.
  // Native windows use app://-/index.html with React route scope.
  const COMPOSER = '[data-codex-composer-root], ._ComposerLayoutRoot_boifs_2';
  const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
  let disposed = false, composer = null, host = null, shadow = null;
  let conversationId = null, usage = null, quotas = null;
  let syncScheduled = false, closeTimer = 0, hoverTimer = 0, pinned = false, panelOpen = false;
  const sessionSnapshots = new Map();
  const nativeControls = new Set();
  const nativeContexts = new Set();
  const nativeStyle = document.createElement('style');
  nativeStyle.textContent = '[data-codex-dashboard-compact-control]{font-size:12px!important;padding-left:6px!important;padding-right:6px!important}[data-codex-dashboard-compact-control] svg{width:14px!important;height:14px!important}[data-codex-dashboard-native-context]{display:none!important}';
  document.head.append(nativeStyle);
  let view = null, rows = [];
  let lastOutputTokens = null, lastTimestamp = 0, speedValue = null;
  let resetRequest = null, resetMessage = '';

  const text = (value, maximum = 120) => typeof value === 'string' ? value.slice(0, maximum) : '';
  const number = value => typeof value === 'number' && Number.isFinite(value) ? value : null;
  const counter = value => Number.isSafeInteger(value) && value >= 0 ? value : null;
  const percent = value => number(value) === null ? null : Math.max(0, Math.min(100, value));
  const showNumber = value => counter(value) === null ? '—' : value.toLocaleString('zh-CN');
  const compact = value => counter(value) === null ? '—' : value >= 1e9 ? `${(value / 1e9).toFixed(1)}B` : value >= 1e6 ? `${(value / 1e6).toFixed(1)}M` : value >= 1e3 ? `${(value / 1e3).toFixed(1)}k` : String(value);
  const pct = value => number(value) === null ? '—' : `${value.toFixed(value % 1 ? 1 : 0)}%`;
  const uuid = value => typeof value === 'string' && UUID.test(value) ? value.toLowerCase() : null;

  function visible(element) {
    if (!element || !element.isConnected || element.closest('[hidden], [inert], [aria-hidden="true"]')) return false;
    const style = getComputedStyle(element);
    const rect = element.getBoundingClientRect();
    return style.display !== 'none' && style.visibility !== 'hidden' && rect.width > 0 && rect.height > 0;
  }

  function uniqueComposer() {
    const candidates = [...document.querySelectorAll(COMPOSER)].filter(visible);
    const leaves = candidates.filter(root => !candidates.some(other => other !== root && root.contains(other)));
    return leaves.length === 1 ? leaves[0] : null;
  }

  function pathId(pathname) {
    if (typeof pathname !== 'string') return null;
    if (/^\/remote\//i.test(pathname)) return 'invalid';
    const match = /^(?:\/local\/|\/hotkey-window\/thread\/)([^/]+)\/?$/.exec(pathname);
    if (!match) {
      if (/^\/local\//i.test(pathname)) return 'invalid';
      return null;
    }
    return uuid(decodeURIComponent(match[1])) || 'invalid';
  }

  function currentId(root) {
    if (!root) return null;
    const fromPath = pathId(location.pathname);
    if (fromPath === 'invalid') return null;

    let fiberId = null;
    let conflict = false;
    const fiberKey = Object.keys(root).find(key => key.startsWith('__reactFiber$'));
    let fiber = fiberKey ? root[fiberKey] : null;
    for (let depth = 0; fiber && depth < 100; ++depth, fiber = fiber.return) {
      const props = fiber.memoizedProps;
      if (!props || typeof props !== 'object') continue;
      for (const route of [props, props.value, props.value?.value]) {
        if (!route || typeof route !== 'object') continue;
        if (route.routeKind === 'remote-thread' || route.routeKind === 'remote-task') {
          return null;
        }
        const rPath = typeof route.pathname === 'string' ? pathId(route.pathname) : null;
        if (rPath === 'invalid') return null;
        const cand = uuid(route.conversationId) || uuid(route.threadId) || uuid(route.id);
        if (cand) {
          if (rPath && rPath !== cand) { conflict = true; continue; }
          fiberId = cand;
          break;
        }
      }
      if (fiberId || conflict) break;
    }

    if (conflict) return null;
    if (fromPath && fiberId && fromPath !== fiberId) return null;
    return fromPath || fiberId || null;
  }

  function tokenGroup(group) {
    if (!group || typeof group !== 'object') return null;
    const fields = ['inputTokens', 'cachedInputTokens', 'outputTokens', 'reasoningOutputTokens', 'totalTokens'];
    const result = {};
    for (const field of fields) result[field] = counter(group[field]);
    return result;
  }

  function sessionData(data) {
    if (!data || typeof data !== 'object') return null;
    return {
      total: tokenGroup(data.total), last: tokenGroup(data.last),
      contextWindow: counter(data.contextWindow), timestamp: text(data.timestamp, 64), outputTokensPerSecond: number(data.outputTokensPerSecond),
      error: Boolean(data.error)
    };
  }

  function windowData(data) {
    if (!data || typeof data !== 'object') return null;
    const used = number(data.usedPercent);
    if (used === null) return null;
    return {
      usedPercent: used,
      remainingPercent: number(data.remainingPercent) === null ? 100 - percent(used) : percent(data.remainingPercent),
      windowDurationMins: counter(data.windowDurationMins), resetsAt: counter(data.resetsAt)
    };
  }

  function quotaData(data) {
    if (!data || typeof data !== 'object') return null;
    return {
      updatedAt: counter(data.updatedAt), error: Boolean(data.error),
      resetCredits: counter(data.resetCredits), resetEligible: data.resetEligible === true,
      pools: Array.isArray(data.pools) ? data.pools.slice(0, 16).map(pool => ({
        id: text(pool?.id), name: text(pool?.name), plan: text(pool?.plan, 64),
        ordinaryUsageAllowed: typeof pool?.ordinaryUsageAllowed === 'boolean' ? pool.ordinaryUsageAllowed : null,
        primary: windowData(pool?.primary), secondary: windowData(pool?.secondary)
      })) : []
    };
  }

  function duration(minutes) {
    if (minutes === null || minutes === 0) return '窗口 —';
    if (minutes % 1440 === 0) return `${minutes / 1440} 天窗口`;
    if (minutes % 60 === 0) return `${minutes / 60} 小时窗口`;
    return `${minutes} 分钟窗口`;
  }

  function resets(at) {
    if (at === null) return '重置时间 —';
    const delta = Math.ceil(at - Date.now() / 1000);
    if (delta <= 0) return '已到重置时间 · 等待刷新';
    if (delta < 60) return `${delta} 秒后重置`;
    if (delta < 3600) return `${Math.ceil(delta / 60)} 分钟后重置`;
    if (delta < 86400) return `${Math.ceil(delta / 3600)} 小时后重置`;
    return `${Math.ceil(delta / 86400)} 天后重置`;
  }

  function freshness(value) {
    if (!value) return '更新时间 —';
    const date = new Date(typeof value === 'number' ? value * 1000 : value);
    return Number.isFinite(date.getTime()) ? `更新 ${date.toLocaleTimeString('zh-CN', {hour: '2-digit', minute: '2-digit', second: '2-digit'})}` : '更新时间 —';
  }

  function status() {
    if (quotas?.error) return {text: '额度更新失败', tone: 'unknown'};
    if (!quotas || !quotas.pools.length) return {text: '额度 —', tone: 'unknown'};
    if (quotas.pools.some(pool => pool.ordinaryUsageAllowed === false)) return {text: '普通用量不可用', tone: 'critical'};
    const remaining = quotas.pools.flatMap(pool => [pool.primary, pool.secondary]).filter(Boolean).map(w => w.remainingPercent);
    if (!remaining.length) return {text: '额度 —', tone: 'unknown'};
    const lowest = Math.min(...remaining);
    return {text: `${lowest < 20 ? '额度紧张' : lowest <= 50 ? '额度偏低' : '额度剩余'} ${pct(lowest)}`, tone: lowest < 20 ? 'critical' : lowest <= 50 ? 'low' : 'healthy'};
  }

  function node(tag, className, content) {
    const result = document.createElement(tag);
    if (className) result.className = className;
    if (content !== undefined) result.textContent = content;
    return result;
  }

  function findMountPoint(root) {
    if (!root) return null;
    const allButtons = [...root.querySelectorAll('button')].filter(visible);
    const model = allButtons.find(b => b.querySelector('._ModelPickerTriggerContent_b9jvu_2') || b.querySelector('[class*="ModelPicker"]') || b.textContent.includes('选择强度') || b.getAttribute('data-testid') === 'model-selector-trigger');
    const send = allButtons.find(b => ['停止', '发送', 'Send', 'Stop'].includes(b.getAttribute('aria-label')) || b.getAttribute('data-testid') === 'send-button' || b.classList.contains('send'));
    if (!model && location.protocol === 'app:') return null;

    if (model) {
      let branch = model;
      for (let parent = branch.parentElement; parent && parent !== root; branch = parent, parent = parent.parentElement) {
        const style = getComputedStyle(parent);
        if (style.display === 'flex' && style.justifyContent === 'flex-end') {
          return {parent, nextSibling: branch};
        }
      }
    }

    if (model && send) {
      let ancestor = model.parentElement;
      while (ancestor && ancestor !== root && !ancestor.contains(send)) {
        ancestor = ancestor.parentElement;
      }
      if (ancestor && ancestor.contains(send)) {
        let branch = model;
        while (branch.parentElement !== ancestor) branch = branch.parentElement;
        return { parent: ancestor, nextSibling: branch };
      }
    }
    if (model) {
      let branch = model;
      while (branch.parentElement !== root && !['flex', 'inline-flex'].includes(getComputedStyle(branch.parentElement).display)) branch = branch.parentElement;
      return {parent: branch.parentElement, nextSibling: branch};
    }
    const actions = root.querySelector('.actions');
    if (actions) {
      const sendBtn = actions.querySelector('.send') || actions.querySelector('button');
      return { parent: actions, nextSibling: sendBtn || null };
    }
    return { parent: root, nextSibling: null };
  }

  function hasNativeContext(root) {
    if (!root) return false;
    const explicit = root.querySelector([
      '[class*="context-window"]',
      '[class*="contextWindow"]',
      '[class*="ContextWindow"]',
      '[class*="context_window"]',
      '[class*="context-usage"]',
      '[class*="contextUsage"]',
      '[class*="token-usage"]',
      '[class*="tokenUsage"]',
      '[aria-label*="上下文"]',
      '[aria-label*="context" i]',
      '[title*="上下文"]',
      '[title*="context" i]',
      '[data-testid*="context"]',
      '[data-testid*="token"]'
    ].join(', '));
    if (explicit && visible(explicit)) return true;

    const allButtons = [...root.querySelectorAll('button')].filter(visible);
    const model = allButtons.find(b => b.querySelector('._ModelPickerTriggerContent_b9jvu_2') || b.querySelector('[class*="ModelPicker"]') || b.textContent.includes('选择强度') || b.getAttribute('data-testid') === 'model-selector-trigger' || (b.textContent && (b.textContent.includes('Sol') || b.textContent.includes('GPT'))));

    if (model) {
      const isNative = location.protocol === 'app:' || Boolean(document.querySelector('._ModelPickerTriggerContent_b9jvu_2'));
      if (isNative && (model.textContent.includes('Sol') || model.textContent.includes('GPT') || model.textContent.includes('Ultra'))) {
        return true;
      }

      const parent = model.parentElement;
      if (parent) {
        for (const child of parent.children) {
          if (child === model || child.id === 'codex-dashboard-inline') continue;
          if (!visible(child)) continue;
          if (child.textContent.includes('完全访问') || child.textContent.includes('访问')) continue;
          if (child.querySelector('svg circle, circle, canvas, [role="progressbar"]') ||
              (child.tagName.toLowerCase() === 'svg' && child.querySelector('circle, path')) ||
              (child.offsetWidth > 0 && child.offsetWidth <= 26 && getComputedStyle(child).borderRadius.includes('50%'))) {
            return true;
          }
        }
      }

      let prev = model.previousElementSibling;
      while (prev) {
        if (visible(prev) && !prev.textContent.includes('完全访问') && !prev.textContent.includes('访问')) {
          if (prev.querySelector('svg circle, circle, canvas, [role="progressbar"]') ||
              (prev.tagName.toLowerCase() === 'svg' && prev.querySelector('circle, path')) ||
              (prev.offsetWidth > 0 && prev.offsetWidth <= 26 && getComputedStyle(prev).borderRadius.includes('50%'))) {
            return true;
          }
        }
        prev = prev.previousElementSibling;
      }
    }

    return false;
  }

  function hideNativeContext(root) {
    if (!root) return;
    for (const element of nativeContexts) if (!element.isConnected) { element.removeAttribute('data-codex-dashboard-native-context'); nativeContexts.delete(element); }
    for (const ring of root.querySelectorAll('[role="img"][aria-label^="上下文用量"], [role="img"][aria-label^="Context usage" i]')) {
      const wrapper = ring.parentElement;
      const target = wrapper?.tagName === 'SPAN' && wrapper.children.length === 1 ? wrapper : ring;
      if (!nativeContexts.has(target)) {
        target.setAttribute('data-codex-dashboard-native-context', '');
        nativeContexts.add(target);
      }
    }
  }

  function compactNativeControls(root) {
    if (!root) return;
    for (const element of nativeControls) if (!element.isConnected) { element.removeAttribute('data-codex-dashboard-compact-control'); nativeControls.delete(element); }
    for (const button of root.querySelectorAll('button')) {
      if (button.textContent.includes('完全访问') || button.querySelector('[class*="ModelPicker"]') || button.getAttribute('data-testid') === 'model-selector-trigger') {
        if (!nativeControls.has(button)) {
          button.setAttribute('data-codex-dashboard-compact-control', ''); nativeControls.add(button);
        }
      }
    }
  }

  function mount(root) {
    const mountTarget = findMountPoint(root);
    if (!mountTarget) return;
    host = node('div');
    host.id = 'codex-dashboard-inline';
    host.dataset.codexDashboard = 'inline';
    host.style.cssText = 'display:inline-flex;align-items:center;flex:0 1 auto;min-width:0;max-width:100%;position:relative;margin:0 4px;';
    const isNative = location.protocol === 'app:' || Boolean(document.querySelector('._ModelPickerTriggerContent_b9jvu_2, [class*="ModelPicker"], [data-testid="model-selector-trigger"]'));
    host.dataset.native = isNative ? 'true' : 'false';
    const nativeCtx = hasNativeContext(root);
    host.dataset.nativeContext = nativeCtx ? 'true' : 'false';

    shadow = host.attachShadow({mode: 'open'});
    const style = node('style');
    style.textContent = `
      :host{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","Microsoft YaHei",sans-serif;font-size:11px;line-height:1.45;color:#c5c8d0;color-scheme:dark light;display:inline-flex;align-items:center;flex:0 1 auto;min-width:0;max-width:100%;position:relative;margin:0 4px}
      *{box-sizing:border-box}button{font:inherit;color:inherit}
      :host([hidden]){display:none!important}
      .bar{display:inline-flex;align-items:center;gap:4px;min-height:28px;padding:1px 0;font-variant-numeric:tabular-nums}
      .chip{display:inline-flex;align-items:center;gap:4px;color:#a9adb6;background:transparent;border:0;border-radius:6px;padding:3px 6px;cursor:pointer;white-space:nowrap;line-height:16px;transition:background 120ms ease,color 120ms ease}
      .chip:hover,.chip[aria-expanded="true"]{background:rgba(255,255,255,0.08);color:#f2f4f8}
      .chip:focus-visible{outline:2px solid #85a8ff;outline-offset:1px}
      .chip strong{font-weight:550;font-variant-numeric:tabular-nums}
      .speed-val{min-width:4.5ch;text-align:right}
      .cache-metric strong,.context-metric strong{min-width:5ch;text-align:right}
      .total-metric strong{min-width:5.5ch;text-align:right}
      .dim{opacity:.48;margin:0 1px}
      .metric{display:inline-flex;align-items:baseline;white-space:nowrap;min-width:0}
      .metric strong{font-weight:550;font-variant-numeric:tabular-nums}
      .chip-cache{display:inline-flex;align-items:center;gap:4px}
      .chip-cache svg{order:0}
      .total-metric{order:1;display:inline-flex}
      .chip-cache .chip-unit{order:2}
      .chip-sep{order:3}
      .cache-label{order:4}
      .cache-metric{order:5;display:inline-flex}
      .context-ring{display:inline-block;flex:none;width:11px;height:11px;border-radius:50%;background:conic-gradient(#85a8ff var(--used-angle,0deg),rgba(255,255,255,0.18) 0);mask:radial-gradient(circle,transparent 42%,#000 46%);-webkit-mask:radial-gradient(circle,transparent 42%,#000 46%)}
      .dot{width:5px;height:5px;border-radius:50%;background:#888;flex:none}.healthy .dot{background:#439467}.low .dot{background:#ba8733}.critical .dot{background:#cd6464}
      .critical .context-ring{background:conic-gradient(#cd6464 var(--used-angle,0deg),rgba(255,255,255,0.18) 0)}
      .low .context-ring{background:conic-gradient(#ba8733 var(--used-angle,0deg),rgba(255,255,255,0.18) 0)}
      svg{width:13px;height:13px;fill:none;stroke:currentColor;stroke-width:1.3;stroke-linecap:round;stroke-linejoin:round;flex:none}
      :host([data-native="true"]) .quota{display:none}
      :host([data-native-context="true"]) .chip-context{display:inline-flex}
      :host(.compact) .bar{gap:2px}
      :host(.compact) .chip{padding:2px 4px;gap:3px;font-size:10.5px}
      :host(.compact) .cache-label,:host(.compact) .chip-sep,:host(.compact) .context-label,:host(.compact) .quota-label{display:none}
      :host(.narrow) .chip-unit{display:none}
      :host(.narrow) .chip-speed{display:inline-flex}
      @media(max-width:760px){.chip{font-size:10.5px;padding:3px}.cache-label,.context-label{display:none}}
      .panel{position:fixed;z-index:2147483000;width:min(340px,calc(100vw - 24px));max-height:min(480px,75vh);overflow:auto;background:#222325;color:#e6e6e6;border:1px solid rgba(255,255,255,0.14);border-radius:12px;box-shadow:0 8px 32px rgba(0,0,0,0.4);padding:14px;overscroll-behavior:contain;font-size:12px}
      .panel[hidden]{display:none}
      .head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:10px}
      .head strong{font-size:13px;font-weight:600}
      .close{border:0;background:transparent;padding:3px 5px;border-radius:4px;cursor:pointer;font-size:17px;line-height:1;color:#a9adb6}
      .close:hover{background:rgba(255,255,255,0.1);color:#fff}
      .section-title{font-size:11px;opacity:.64;margin:12px 0 6px}
      .row{display:flex;justify-content:space-between;gap:12px;padding:3px 0}
      .row span:last-child{text-align:right;font-variant-numeric:tabular-nums}
      .pool{border-top:1px solid rgba(255,255,255,0.1);padding:9px 0 2px}
      .pool-title{font-weight:550;overflow-wrap:anywhere}
      .pool-meta{font-size:11px;opacity:.64;margin-top:2px}
      .pool-permission{font-size:11px;margin-top:4px}
      .window{margin-top:7px}
      .window-summary{display:flex;justify-content:space-between;gap:10px;font-variant-numeric:tabular-nums}
      .window-reset{font-size:11px;opacity:.64;margin-top:1px}
      .track{height:4px;border-radius:2px;background:rgba(255,255,255,0.12);margin:5px 0;overflow:hidden}
      .fill{height:100%;background:#6e947e;border-radius:2px;width:0}.low .fill{background:#b59251}.critical .fill{background:#bd7777}
      .foot{font-size:11px;opacity:.64;line-height:1.55;margin-top:10px}
      .empty{opacity:.64;padding:6px 0}
      @media(max-width:440px){
        .bar{gap:1px}
        .chip{padding:2px 3px;font-size:10px}
        .cache-label,.chip-sep,.context-label,.quota-label{display:none}
      }
      @media(prefers-color-scheme:light){
        :host{color:#485261}
        .chip{color:#555d6b}.chip:hover,.chip[aria-expanded="true"]{background:rgba(0,0,0,0.06);color:#111}
        .context-ring{background:conic-gradient(#315fc4 var(--used-angle,0deg),rgba(0,0,0,0.12) 0)}
        .panel{background:#fff;color:#202631;border-color:#ccd3df;box-shadow:0 8px 32px rgba(20,35,64,0.15)}
        .pool{border-top-color:rgba(0,0,0,0.08)}
        .track{background:rgba(0,0,0,0.08)}
      }
    `;

    const bar = node('div', 'bar');
    bar.setAttribute('aria-label', 'Codex 用量统计');

    // 1. Speed chip
    const speedBtn = node('button', 'chip chip-speed');
    speedBtn.type = 'button';
    speedBtn.dataset.card = 'speed';
    speedBtn.setAttribute('aria-label', '查看生成速率与统计');
    speedBtn.setAttribute('aria-expanded', 'false');
    speedBtn.setAttribute('aria-description', '最近 Token 事件采样速度，非逐 Token 流式速率');
    speedBtn.innerHTML = `<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M3 12a6 6 0 1 1 10 0M8 9l3-4"/></svg><strong class="speed-val">—</strong><span class="dim chip-unit">tok/s</span>`;

    // 2. Cache & Total chip
    const cacheBtn = node('button', 'chip chip-cache');
    cacheBtn.type = 'button';
    cacheBtn.dataset.card = 'cache';
    cacheBtn.setAttribute('aria-label', '查看 Token 缓存详情');
    cacheBtn.setAttribute('aria-expanded', 'false');
    cacheBtn.innerHTML = `
      <svg viewBox="0 0 16 16" aria-hidden="true"><ellipse cx="8" cy="4.5" rx="5" ry="1.8"/><path d="M3 4.5v7c0 1.2 2.2 2 5 2s5-.8 5-2v-7M3 8c0 1.2 2.2 2 5 2s5-.8 5-2"/></svg>
      <span class="metric cache-metric"><strong>—</strong></span>
      <span class="metric total-metric"><strong>—</strong></span>
      <span class="dim chip-unit">tok</span>
      <span class="dim chip-sep">·</span>
      <span class="cache-label">缓存命中</span>
    `;

    // 3. Context chip
    const contextBtn = node('button', 'chip chip-context');
    contextBtn.type = 'button';
    contextBtn.dataset.card = 'context';
    contextBtn.setAttribute('aria-label', '查看上下文与额度详情');
    contextBtn.setAttribute('aria-expanded', 'false');
    contextBtn.innerHTML = `
      <span class="context-ring" id="context-ring" aria-hidden="true"></span>
      <span class="context-label">上下文</span>
      <span class="metric context-metric"><strong>—</strong></span>
    `;

    // 4. Quota badge
    const quotaBtn = node('button', 'chip quota unknown');
    quotaBtn.type = 'button';
    quotaBtn.dataset.card = 'quota';
    quotaBtn.setAttribute('aria-label', '查看 Codex 额度和用量详情');
    quotaBtn.setAttribute('aria-expanded', 'false');
    quotaBtn.setAttribute('aria-controls', 'codex-dashboard-details');
    quotaBtn.innerHTML = `<span class="dot"></span><span class="quota-label">额度 —</span>`;

    bar.append(speedBtn, cacheBtn, contextBtn, quotaBtn);

    const panel = node('section', 'panel');
    panel.id = 'codex-dashboard-details';
    panel.setAttribute('aria-label', 'Codex 额度和用量详情');
    panel.hidden = true;
    const head = node('div', 'head');
    const close = node('button', 'close', '×');
    close.type = 'button'; close.setAttribute('aria-label', '关闭用量详情');
    head.append(node('strong', '', '用量与额度'), close);
    const sessionTitle = node('div', 'section-title', '当前会话');
    const tokenRows = {};
    panel.append(head, sessionTitle);
    for (const [key, title] of [['input', '累计输入'], ['cached', '缓存读取'], ['output', '累计输出'], ['reasoning', '其中推理'], ['context', '最近请求 / 窗口'], ['speed', '最近采样速度']]) {
      const row = node('div', 'row');
      const val = node('span', '', '—');
      row.append(node('span', 'muted', title), val); panel.append(row); tokenRows[key] = val;
    }
    const sessionNote = node('div', 'foot');
    const quotaTitle = node('div', 'section-title', '账号共享额度');
    const pools = node('div');
    const quotaNote = node('div', 'foot');
    const resetButton = node('button', 'close', '重置额度');
    resetButton.type = 'button'; resetButton.style.cssText = 'width:100%;margin-top:10px;padding:8px;height:auto';
    const resetNote = node('div', 'foot');
    resetButton.addEventListener('click', event => {
      if (!event.isTrusted || !canReset()) return;
      if (!window.confirm('消耗 1 次重置额度？仅剩余额度 ≤1% 时允许，后端将再次核实。')) return;
      resetRequest = {id: crypto.randomUUID(), confirmed: true};
      resetMessage = '正在核实额度…'; render();
    });
    panel.append(sessionNote, quotaTitle, pools, quotaNote, resetButton, resetNote);

    shadow.append(style, bar, panel);

    mountTarget.parent.insertBefore(host, mountTarget.nextSibling);
    compactNativeControls(root);
    hideNativeContext(root);

    const speedVal = speedBtn.querySelector('.speed-val');
    const cacheVal = cacheBtn.querySelector('.cache-metric strong');
    const totalVal = cacheBtn.querySelector('.total-metric strong');
    const contextVal = contextBtn.querySelector('.context-metric strong');
    const contextRing = contextBtn.querySelector('.context-ring');
    const quotaLabel = quotaBtn.querySelector('.quota-label');

    view = {
      speedBtn, cacheBtn, contextBtn, quotaBtn,
      speedVal, cacheVal, totalVal, contextVal, contextRing, quotaLabel,
      button: quotaBtn, label: quotaLabel,
      cache: { value: cacheVal }, total: { value: totalVal }, context: { value: contextVal, container: contextBtn },
      panel, tokenRows, sessionNote, pools, quotaNote, resetButton, resetNote
    };

    rows = []; pinned = false; panelOpen = false;

    for (const btn of [speedBtn, cacheBtn, contextBtn, quotaBtn]) {
      btn.addEventListener('pointerenter', () => scheduleOpen(btn));
      btn.addEventListener('pointerleave', scheduleClose);
      btn.addEventListener('focus', () => openPanel(false));
      btn.addEventListener('blur', event => { if (!panel.contains(event.relatedTarget)) scheduleClose(); });
      btn.addEventListener('click', () => { if (pinned) hidePanel(); else openPanel(true); });
    }

    panel.addEventListener('pointerenter', () => clearTimeout(closeTimer));
    panel.addEventListener('pointerleave', scheduleClose);
    close.addEventListener('click', hidePanel);
    shadow.addEventListener('keydown', event => {
      if (event.key === 'Escape' && panelOpen) { event.preventDefault(); event.stopPropagation(); hidePanel(); quotaBtn.focus(); }
    });
  }

  function clearMount() {
    clearTimeout(hoverTimer); hoverTimer = 0;
    clearTimeout(closeTimer); closeTimer = 0;
    for (const button of nativeControls) button.removeAttribute('data-codex-dashboard-compact-control');
    nativeControls.clear();
    for (const ring of nativeContexts) ring.removeAttribute('data-codex-dashboard-native-context');
    nativeContexts.clear();
    host?.remove(); host = null; shadow = null; view = null; rows = [];
    panelOpen = false; pinned = false;
  }

  function openPanel(pin) {
    if (!view) return;
    clearTimeout(hoverTimer); hoverTimer = 0;
    clearTimeout(closeTimer); closeTimer = 0;
    if (pin) pinned = true;
    panelOpen = true; view.panel.hidden = false;
    view.quotaBtn.setAttribute('aria-expanded', 'true');
    positionPanel();
  }

  function hidePanel() {
    clearTimeout(hoverTimer); hoverTimer = 0;
    clearTimeout(closeTimer); closeTimer = 0;
    pinned = false; panelOpen = false;
    if (view) { view.panel.hidden = true; view.quotaBtn.setAttribute('aria-expanded', 'false'); }
  }

  function scheduleOpen(button) {
    clearTimeout(hoverTimer);
    clearTimeout(closeTimer); closeTimer = 0;
    hoverTimer = setTimeout(() => {
      hoverTimer = 0;
      if (!disposed && host?.isConnected && !host.hidden && button.matches(':hover')) openPanel(false);
    }, 1000);
  }

  function scheduleClose() {
    clearTimeout(hoverTimer); hoverTimer = 0;
    clearTimeout(closeTimer);
    if (!pinned) closeTimer = setTimeout(hidePanel, 180);
  }

  function positionPanel() {
    if (!panelOpen || !view) return;
    const anchorBtn = (view.contextBtn && view.contextBtn.offsetParent !== null ? view.contextBtn : null) ||
                      (view.cacheBtn && view.cacheBtn.offsetParent !== null ? view.cacheBtn : null) ||
                      (view.quotaBtn && view.quotaBtn.offsetParent !== null ? view.quotaBtn : null) ||
                      view.speedBtn;
    const anchor = anchorBtn.getBoundingClientRect();
    const panel = view.panel;
    const left = Math.max(12, Math.min(innerWidth - panel.offsetWidth - 12, anchor.right - panel.offsetWidth));
    const above = anchor.top - panel.offsetHeight - 8;
    const top = Math.max(12, above >= 12 ? above : Math.min(innerHeight - panel.offsetHeight - 12, anchor.bottom + 8));
    panel.style.left = `${left}px`; panel.style.top = `${top}px`;
  }

  function poolRow() {
    const container = node('div', 'pool');
    const title = node('div', 'pool-title'), meta = node('div', 'pool-meta'), permission = node('div', 'pool-permission');
    container.append(title, meta, permission);
    const windows = [];
    for (let i = 0; i < 2; ++i) {
      const wrapper = node('div', 'window');
      const summary = node('div', 'window-summary'), label = node('span'), remaining = node('span');
      const track = node('div', 'track'), fill = node('div', 'fill'), reset = node('div', 'window-reset');
      summary.append(label, remaining); track.append(fill); wrapper.append(summary, track, reset); container.append(wrapper);
      windows.push({wrapper, label, remaining, fill, reset});
    }
    return {container, title, meta, permission, windows};
  }

  function setText(element, value) {
    if (element.textContent !== value) element.textContent = value;
  }

  function render() {
    if (!view) return;
    const total = usage?.error ? null : usage?.total;
    const last = usage?.error ? null : usage?.last;
    const cacheRatio = total?.inputTokens > 0 && total.cachedInputTokens !== null && total.cachedInputTokens <= total.inputTokens ? total.cachedInputTokens / total.inputTokens * 100 : null;
    const contextRatio = last?.totalTokens !== null && last?.totalTokens !== undefined && usage?.contextWindow > 0 ? last.totalTokens / usage.contextWindow * 100 : null;

    // Metric values in exact test order
    setText(view.cacheVal, pct(cacheRatio));
    setText(view.totalVal, compact(total?.totalTokens));
    setText(view.contextVal, pct(contextRatio));

    const angle = contextRatio !== null ? Math.min(360, Math.round(contextRatio * 3.6)) : 0;
    view.contextRing.style.setProperty('--used-angle', `${angle}deg`);


    setText(view.speedVal, speedValue && speedValue > 0 ? speedValue.toFixed(1) : '—');

    // Quota summary & tone
    const summary = status();
    view.quotaBtn.className = `chip quota ${summary.tone}`;
    setText(view.quotaLabel, summary.text);
    view.contextBtn.className = `chip chip-context ${summary.tone}`;
    view.contextBtn.setAttribute('aria-description', `最近请求 / 窗口：${view.contextVal.textContent} · 账号额度：${summary.text}`);
    view.cacheBtn.setAttribute('aria-description', `累计用量：${view.totalVal.textContent} · 缓存命中：${view.cacheVal.textContent} · 账号额度：${summary.text}`);

    // Panel details
    setText(view.tokenRows.input, showNumber(total?.inputTokens));
    setText(view.tokenRows.cached, showNumber(total?.cachedInputTokens));
    setText(view.tokenRows.output, showNumber(total?.outputTokens));
    setText(view.tokenRows.reasoning, showNumber(total?.reasoningOutputTokens));
    setText(view.tokenRows.context, `${showNumber(last?.totalTokens)} / ${showNumber(usage?.contextWindow)}`);
    setText(view.tokenRows.speed, speedValue && speedValue > 0 ? `${speedValue.toFixed(1)} tok/s` : '—');
    setText(view.sessionNote, usage?.error ? '会话统计读取失败' : !conversationId ? '当前会话未确认' : usage ? `${freshness(usage.timestamp)} · tok/s 为相邻 Token 事件的输出增量 / 时间差；保留最近采样，含工具等待；非模型流式速率。` : '等待当前会话的 Token 事件');

    const pools = quotas?.error ? [] : quotas?.pools || [];
    while (rows.length > pools.length) rows.pop().container.remove();
    while (rows.length < pools.length) { const row = poolRow(); rows.push(row); view.pools.append(row.container); }
    if (!pools.length) {
      if (!view.empty) { view.empty = node('div', 'empty'); view.pools.append(view.empty); }
      setText(view.empty, quotas?.error ? '额度读取失败' : '额度窗口 —');
    } else { view.empty?.remove(); view.empty = null; }

    pools.forEach((pool, index) => {
      const row = rows[index];
      setText(row.title, pool.name || pool.id || '未命名额度');
      setText(row.meta, [pool.plan, pool.id !== pool.name ? pool.id : ''].filter(Boolean).join(' · '));
      setText(row.permission, quotas?.error ? '旧快照 · 当前权限未知' : pool.ordinaryUsageAllowed === false ? '当前普通用量不可用' : pool.ordinaryUsageAllowed === true ? '普通用量可用' : '普通用量权限 —');
      [pool.primary, pool.secondary].forEach((window, windowIndex) => {
        const item = row.windows[windowIndex];
        item.wrapper.hidden = !window;
        setText(item.label, window ? duration(window.windowDurationMins) : `${windowIndex ? '次' : '主'}窗口 —`);
        setText(item.remaining, window ? `${quotas?.error ? '上次剩余' : '剩余'} ${pct(window.remainingPercent)}` : '—');
        item.fill.style.width = window ? `${window.remainingPercent}%` : '0%';
        item.wrapper.className = `window ${window && !quotas?.error ? window.remainingPercent < 20 ? 'critical' : window.remainingPercent <= 50 ? 'low' : 'healthy' : 'unknown'}`;
        setText(item.reset, window ? resets(window.resetsAt) : '重置时间 —');
      });
    });
    setText(view.quotaNote, quotas?.error ? `${freshness(quotas.updatedAt)} · 更新失败，以上额度是上次快照，当前权限未知。` : `${freshness(quotas?.updatedAt)} · 百分比与后端使用权限分别显示。`);
    view.resetButton.disabled = !canReset();
    setText(view.resetNote, resetMessage || `剩余额度 ≤1% 才允许重置 · 可用次数 ${quotas?.resetCredits ?? '—'}`);

    // Dynamic native context indicator check
    const nativeCtx = hasNativeContext(composer || document.querySelector(COMPOSER));
    host.dataset.nativeContext = nativeCtx ? 'true' : 'false';

    positionPanel();

  }

  function canReset() {
    if (resetRequest || quotas?.error || !quotas?.resetEligible || !(quotas.resetCredits > 0)) return false;
    const pool = quotas.pools.find(item => item.id === 'codex') || (quotas.pools.length === 1 && !quotas.pools[0].id ? quotas.pools[0] : null);
    return Boolean(pool && [pool.primary, pool.secondary].some(item => item && item.remainingPercent <= 1));
  }

  function sync() {
    if (disposed) return;
    const next = uniqueComposer();
    const id = currentId(next);
    if (id !== conversationId) {
      clearTimeout(hoverTimer); hoverTimer = 0;
      conversationId = id;
      const cached = id ? sessionSnapshots.get(id) : null;
      usage = cached?.usage ?? null;
      speedValue = cached?.speedValue ?? null;
      lastOutputTokens = null; lastTimestamp = 0;
    }
    composer = next;
    if (next && !host) mount(next);
    if (host) {
      const target = next ? findMountPoint(next) : null;
      if (target) {
        if (host.parentElement !== target.parent || host.nextSibling !== target.nextSibling) { clearTimeout(hoverTimer); hoverTimer = 0; target.parent.insertBefore(host, target.nextSibling); }
        if (host.hidden) host.hidden = false;
      } else if (!host.hidden) host.hidden = true;
    }
    compactNativeControls(composer);
    hideNativeContext(composer);
    render();
  }

  function schedule() {
    if (disposed || syncScheduled) return;
    syncScheduled = true;
    queueMicrotask(() => { syncScheduled = false; sync(); });
  }

  const observer = new MutationObserver(schedule);
  observer.observe(document.documentElement, {subtree: true, childList: true, attributes: true, attributeFilter: ['hidden', 'inert', 'aria-hidden', 'class', 'style']});
  const interval = setInterval(sync, 1000);
  window.addEventListener('popstate', schedule);
  window.addEventListener('hashchange', schedule);
  window.addEventListener('resize', positionPanel);
  window.addEventListener('scroll', positionPanel, true);

  function dispose() {
    if (disposed) return;
    disposed = true; observer.disconnect(); clearInterval(interval);
    clearTimeout(hoverTimer); clearTimeout(closeTimer);
    for (const button of nativeControls) button.removeAttribute('data-codex-dashboard-compact-control');
    nativeControls.clear();
    window.removeEventListener('popstate', schedule); window.removeEventListener('hashchange', schedule);
    window.removeEventListener('resize', positionPanel); window.removeEventListener('scroll', positionPanel, true);
    window.removeEventListener('pagehide', dispose);
    clearMount(); composer = null; conversationId = null; usage = null; quotas = null;
    sessionSnapshots.clear();
    nativeStyle.remove();
    if (window.__codexDashboard === api) delete window.__codexDashboard;
  }

  const api = Object.freeze({
    state() { sync(); return {conversationId, mounted: Boolean(host?.isConnected && !host.hidden), ...(resetRequest ? {resetRequest} : {})}; },
    update(data) {
      sync();
      if (disposed || !data || typeof data !== 'object') return {sessionAccepted: false, quotaAccepted: false};
      const quotaAccepted = Object.hasOwn(data, 'quotas');
      if (quotaAccepted) quotas = quotaData(data.quotas);
      const sessionAccepted = Boolean(conversationId && uuid(data.conversationId) === conversationId && Object.hasOwn(data, 'usage'));
      if (sessionAccepted) {
        usage = sessionData(data.usage);
        const output = usage?.error ? null : usage?.total?.outputTokens;
        const now = performance.now(), dt = (now-lastTimestamp)/1000;
        if (usage?.error || output === null || output === undefined || (lastOutputTokens !== null && output < lastOutputTokens)) speedValue = null;
        else if (usage.outputTokensPerSecond !== null && usage.outputTokensPerSecond >= 0) speedValue = usage.outputTokensPerSecond;
        else if (lastOutputTokens !== null && output > lastOutputTokens && dt >= 0.25 && dt <= 120) speedValue = (output-lastOutputTokens)/dt;
        lastOutputTokens = output ?? null; lastTimestamp = now;
        sessionSnapshots.delete(conversationId);
        sessionSnapshots.set(conversationId, {usage, speedValue});
        if (sessionSnapshots.size > 32) sessionSnapshots.delete(sessionSnapshots.keys().next().value);
      }
      if (resetRequest && data.resetResult?.id === resetRequest.id) {
        const messages = {reset:'额度已重置',alreadyRedeemed:'该次重置已完成',thresholdNotMet:'当前剩余额度未满足 ≤1%，或无可用重置次数',noCredit:'无可用重置次数',nothingToReset:'当前无需重置'};
        resetMessage = messages[data.resetResult.outcome] || '重置结果未确认，请刷新额度后核实';
        resetRequest = null;
      }
      render(); return {sessionAccepted, quotaAccepted};
    },
    dispose
  });
  window.__codexDashboard = api;
  window.addEventListener('pagehide', dispose, {once: true});
  sync();
})();
