import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import http from 'node:http';
import path from 'node:path';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');
const packages = process.env.PULSE_NODE_MODULES || path.join(process.env.USERPROFILE || process.env.HOME || '', '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules');
const require = createRequire(path.join(packages, '.pulse-loader.cjs'));
const {chromium} = require('playwright');
const chrome = process.env.PULSE_CHROME || 'C:/Program Files/Google/Chrome/Application/chrome.exe';
let checks = 0;
function check(value, expected, message) {assert.deepEqual(value, expected, message); ++checks;}

const server = http.createServer(async (request, response) => {
  const pathname = new URL(request.url, 'http://localhost').pathname;
  const filename = pathname === '/widget/inline.js' ? 'inline.js' : pathname === '/widget/preview.html' ? 'preview.html' : null;
  if (!filename) {response.writeHead(404).end(); return;}
  try {response.writeHead(200, {'content-type': filename.endsWith('.js') ? 'text/javascript; charset=utf-8' : 'text/html; charset=utf-8'}); response.end(await fs.readFile(path.join(root, 'widget', filename)));}
  catch {response.writeHead(500).end();}
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const browser = await chromium.launch({headless: true, executablePath: chrome, args: ['--no-first-run', '--no-default-browser-check']});
const context = await browser.newContext({viewport: {width: 900, height: 740}, colorScheme: 'dark'});
const page = await context.newPage();
const errors = [];
page.on('pageerror', error => errors.push(error.message));
const state = () => page.evaluate(() => window.__codexDashboard.state());
const metric = index => page.locator('[data-codex-dashboard]').locator('.metric strong').nth(index).innerText();
try {
  await page.goto(`http://127.0.0.1:${server.address().port}/widget/preview.html`);
  const ids = await page.evaluate(() => fixture.ids);
  check(await state(), {conversationId: ids.a, mounted: true}, 'mount and exact local UUID');
  check(await metric(0), '75%', 'cache ratio');
  check(await metric(1), '21.0k', 'total Tokens');
  check(await metric(2), '8.0%', 'recent request / context window');
  await page.locator('[data-codex-dashboard]').locator('.chip-speed').hover();
  await page.waitForTimeout(300);
  check(await page.locator('[data-codex-dashboard]').locator('.panel').isVisible(), false, 'brief hover does not open details');
  await page.mouse.move(0, 0);
  await page.waitForTimeout(900);
  check(await page.locator('[data-codex-dashboard]').locator('.panel').isVisible(), false, 'leaving cancels pending hover');
  await page.locator('[data-codex-dashboard]').locator('.chip-cache').hover();
  await page.waitForFunction(() => !document.querySelector('[data-codex-dashboard]').shadowRoot.querySelector('.panel').hidden);
  check(await page.locator('[data-codex-dashboard]').locator('.panel').isVisible(), true, 'sustained hover opens details');
  await page.keyboard.press('Escape'); await page.mouse.move(0, 0);
  await page.evaluate(() => {window.persistentDashboardHost=document.querySelector('[data-codex-dashboard]');});
  const originalText = await page.locator('[data-codex-composer]').innerText();
  await page.waitForTimeout(500);
  await page.evaluate(() => {const usage=structuredClone(fixture.samples.a); usage.total.outputTokens+=100; window.__codexDashboard.update({conversationId:fixture.ids.a,usage});});
  check(await page.locator('[data-codex-dashboard]').locator('.speed-val').innerText().then(Number).then(value => value > 0), true, 'output increment produces sampled tok/s');
  await page.evaluate(() => window.__codexDashboard.update({conversationId:fixture.ids.a,usage:fixture.samples.a}));
  check(await page.locator('[data-codex-dashboard]').locator('.speed-val').innerText(), '—', 'counter rollback clears speed');
  await page.evaluate(() => window.__codexDashboard.update({conversationId:fixture.ids.a,usage:{...fixture.samples.a,outputTokensPerSecond:40}}));
  check(await page.locator('[data-codex-dashboard]').locator('.speed-val').innerText(), '40.0', 'native timestamp speed displays immediately');
  await page.evaluate(() => window.__codexDashboard.update({conversationId:fixture.ids.a,usage:{...fixture.samples.a,outputTokensPerSecond:40}}));
  check(await page.locator('[data-codex-dashboard]').locator('.speed-val').innerText(), '40.0', 'unchanged poll retains native speed');
  check(await page.evaluate(() => {const node=document.querySelector('[data-codex-dashboard]').shadowRoot.querySelector('.total-metric strong').firstChild;window.__codexDashboard.update({conversationId:fixture.ids.a,usage:{...fixture.samples.a,outputTokensPerSecond:40}});return node===document.querySelector('[data-codex-dashboard]').shadowRoot.querySelector('.total-metric strong').firstChild;}), true, 'unchanged poll does not replace metric text node');
  await page.locator('[data-codex-dashboard]').locator('.quota').click();
  await page.evaluate(() => {window.savedPanel = document.querySelector('[data-codex-dashboard]').shadowRoot.querySelector('.panel');});
  await page.evaluate(() => window.__codexDashboard.update({conversationId: fixture.ids.a, usage: fixture.samples.a, quotas: fixture.quotaSample()}));
  check(await page.evaluate(() => savedPanel === document.querySelector('[data-codex-dashboard]').shadowRoot.querySelector('.panel') && !savedPanel.hidden), true, 'open popup is retained during updates');
  check(await page.locator('[data-codex-dashboard]').locator('.window-summary').first().innerText(), '5 小时窗口\n剩余 76%', 'dynamic primary duration');
  check(await page.locator('[data-codex-dashboard]').locator('.window-summary').nth(1).innerText(), '7 天窗口\n剩余 32%', 'dynamic secondary duration');
  check(await page.locator('[data-codex-dashboard]').locator('.row').last().innerText(), '最近采样速度\n40.0 tok/s', 'speed detail keeps last measured sample');
  const accepted = await page.evaluate(() => window.__codexDashboard.update({conversationId: fixture.ids.b, usage: fixture.samples.b, quotas: {pools: [{id: 'codex', ordinaryUsageAllowed: false, primary: {usedPercent: 0}, secondary: null}]}}));
  check(accepted, {sessionAccepted: false, quotaAccepted: true}, 'mismatched session ignored, account quota accepted');
  check(await metric(1), '21.0k', 'mismatch cannot replace session data');
  check(await page.locator('[data-codex-dashboard]').locator('.quota').innerText(), '普通用量不可用', 'backend permission overrides healthy color');
  await page.evaluate(() => window.__codexDashboard.update({quotas: {...fixture.quotaSample(), error: 'offline'}}));
  check(await page.locator('[data-codex-dashboard]').locator('.quota').innerText(), '额度更新失败', 'failed quota fetch is explicit');
  check(await page.locator('[data-codex-dashboard]').locator('.foot').nth(1).innerText().then(text => text.includes('上次快照')), true, 'stale numbers are identified');

  await page.evaluate(() => fixture.switchTo('b', false));
  check(await page.evaluate(() => document.querySelector('[data-codex-dashboard]')===persistentDashboardHost), true, 'chat switch reuses widget instead of recreating it');
  check(await state(), {conversationId: ids.b, mounted: true}, 'second session follows route and composer');
  check(await metric(1), '—', 'route change clears old session');
  await page.evaluate(() => window.__codexDashboard.update({conversationId: fixture.ids.b, usage: fixture.samples.b}));
  check(await metric(1), '10.0k', 'current second session update accepted');
  await page.evaluate(() => {document.querySelector('#surface').replaceChildren(fixture.composerElement()); window.__codexDashboard.state();});
  check(await metric(1), '10.0k', 'composer remount preserves same-session usage');
  check(await page.evaluate(() => document.querySelector('[data-codex-dashboard]')===persistentDashboardHost), true, 'composer remount retains widget identity');
  await page.evaluate(() => fixture.switchTo('a', false));
  check(await metric(1), '21.0k', 'returning chat restores its own snapshot immediately');
  await page.evaluate(() => fixture.switchTo('b', false));
  check(await metric(1), '10.0k', 'cached snapshots remain isolated by conversation');
  check(await page.locator('[data-codex-dashboard]').count(), 1, 'exactly one component');
  await page.evaluate(() => fixture.unknown());
  check(await metric(0), '—', 'missing counters remain unavailable');
  check(await page.locator('[data-codex-dashboard]').locator('.quota').innerText(), '额度 —', 'null windows are unavailable');

  await page.evaluate(() => {
    history.replaceState({}, '', '/widget/preview.html?initialRoute=/local/' + fixture.ids.a);
    const root = document.querySelector('[data-codex-composer-root]');
    root.__reactFiber$fixture = {memoizedProps: {}, return: {memoizedProps: {value: {value: {routeKind: 'local-thread', conversationId: fixture.ids.b, pathname: '/local/' + fixture.ids.b}}}, return: null}};
  });
  check(await state(), {conversationId: ids.b, mounted: true}, 'React route scope outranks stale initialRoute');
  await page.evaluate(() => history.replaceState({}, '', '/local/' + fixture.ids.a));
  check((await state()).conversationId, null, 'conflicting route evidence rejected');
  await page.evaluate(() => {delete document.querySelector('[data-codex-composer-root]').__reactFiber$fixture; history.replaceState({}, '', '/local/' + fixture.ids.b);});
  await page.evaluate(() => document.querySelector('#surface').append(fixture.composerElement()));
  check(await state(), {conversationId: null, mounted: false}, 'ambiguous visible composers rejected');
  await page.evaluate(() => {const roots = document.querySelectorAll('[data-codex-composer-root]'); roots[1].hidden = true;});
  check(await state(), {conversationId: ids.b, mounted: true}, 'hidden composer excluded');
  await page.evaluate(() => {history.replaceState({}, '', '/remote/' + fixture.ids.b);});
  check((await state()).conversationId, null, 'remote task ID not treated as local conversation');
  await page.evaluate(() => {history.replaceState({}, '', '/local/not-a-uuid');});
  check((await state()).conversationId, null, 'invalid UUID rejected');

  await page.evaluate(() => fixture.switchTo('a'));
  async function resetQuota(remaining) {
    await page.evaluate(remaining => window.__codexDashboard.update({quotas: {updatedAt: Math.floor(Date.now()/1000), resetCredits: 2, resetEligible: true, pools: [{id: 'codex', primary: {usedPercent: 100-remaining, remainingPercent: remaining}}]}}), remaining);
  }
  await resetQuota(1.01);
  check(await page.locator('[data-codex-dashboard]').getByRole('button', {name:'重置额度', exact:true}).isDisabled(), true, 'above 1% cannot reset');
  await resetQuota(1);
  const resetBtn = page.locator('[data-codex-dashboard]').getByRole('button', {name:'重置额度', exact:true});
  check(await resetBtn.isDisabled(), false, 'exactly 1% enables reset');
  if (!await page.locator('[data-codex-dashboard]').locator('.panel').isVisible()) await page.locator('[data-codex-dashboard]').locator('.quota').click();
  page.once('dialog', dialog => dialog.accept());
  await resetBtn.click();
  check((await state()).resetRequest.confirmed, true, 'reset requires confirmation');
  await page.evaluate(() => {const id=window.__codexDashboard.state().resetRequest.id; window.__codexDashboard.update({resetResult:{id,outcome:'thresholdNotMet'}});});
  check((await state()).resetRequest, undefined, 'backend result clears request');
  await page.locator('[data-codex-dashboard]').getByRole('button', {name:'关闭用量详情'}).click();
  await page.setViewportSize({width: 360, height: 740});
  await page.locator('[data-codex-dashboard]').locator('.quota').click();
  check(await page.evaluate(() => {
    const host = document.querySelector('[data-codex-dashboard]');
    const panel = host.shadowRoot.querySelector('.panel').getBoundingClientRect();
    return document.documentElement.scrollWidth <= innerWidth && panel.left >= 0 && panel.right <= innerWidth && panel.top >= 0 && panel.bottom <= innerHeight;
  }), true, 'narrow viewport has no clipping or horizontal overflow');
  if (process.argv.includes('--screenshot')) {
    await page.setViewportSize({width: 900, height: 740});
    await page.screenshot({path: path.join(root, 'widget', 'preview.png'), fullPage: true});
  }
  check(await page.locator('[data-codex-composer]').innerText(), originalText, 'input text remains untouched');
  await page.setViewportSize({width: 900, height: 740});
  await page.evaluate(() => {
    const root=fixture.composerElement(); root.querySelector('.actions').remove();
    document.querySelector('#surface').replaceChildren(root);
    window.__codexDashboard.state();
    window.__codexDashboard.update({conversationId:fixture.ids.a,usage:fixture.samples.a});
    window.lateToolbarHost=document.querySelector('[data-codex-dashboard]');
    const toolbar=document.createElement('div'); toolbar.className='late-toolbar';
    toolbar.style.cssText='display:flex;justify-content:flex-end;align-items:center';
    toolbar.innerHTML='<span class="model-wrapper"><span style="display:contents"><button type="button" data-testid="model-selector-trigger">GPT · Ultra</button></span></span><button type="button" aria-label="语音">◉</button>';
    root.append(toolbar); window.__codexDashboard.state();
  });
  check(await page.evaluate(() => document.querySelector('[data-codex-dashboard]')===lateToolbarHost && lateToolbarHost.parentElement.classList.contains('late-toolbar')), true, 'late toolbar reparents existing widget into control row');
  check(await metric(1), '21.0k', 'placement repair preserves usage');
  await page.evaluate(() => {const toolbar=document.querySelector('.late-toolbar');toolbar.before(lateToolbarHost);toolbar.replaceWith(toolbar.cloneNode(true));window.__codexDashboard.state();});
  check(await page.evaluate(() => lateToolbarHost.parentElement===document.querySelector('.late-toolbar') && lateToolbarHost.nextElementSibling.classList.contains('model-wrapper')), true, 'rebuilt voice toolbar restores widget before model');
  check(await page.evaluate(() => document.querySelector('[data-testid="model-selector-trigger"]').hasAttribute('data-codex-dashboard-compact-control')), true, 'replacement model control keeps compact styling');
  await page.evaluate(() => {window.savedAPI = window.__codexDashboard; savedAPI.dispose(); document.querySelector('#surface').replaceChildren(fixture.composerElement());});
  await page.waitForTimeout(1100);
  check(await page.locator('[data-codex-dashboard]').count(), 0, 'dispose prevents observer/timer remount');
  check(await page.evaluate(() => savedAPI.state()), {conversationId: null, mounted: false}, 'disposed state is empty');
  check(errors, [], 'no runtime errors');
  console.log(`PASS: ${checks} widget checks; Chromium fixture, not Codex runtime acceptance`);
} finally {
  await context.close(); await browser.close(); await new Promise(resolve => server.close(resolve));
}
