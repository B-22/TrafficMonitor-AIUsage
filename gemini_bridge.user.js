// ==UserScript==
// @name         Gemini 用量 → 限额卡片桥接
// @namespace    https://github.com/claude-usage-assistant
// @version      1.0
// @description  把 gemini.google.com 的 5 小时 / 本周用量推送给本地限额卡片(quota_card.py)
// @match        https://gemini.google.com/*
// @run-at       document-start
// @grant        GM_xmlhttpRequest
// @grant        unsafeWindow
// @connect      127.0.0.1
// ==/UserScript==

/*
 * 为什么要有这个脚本?
 *
 * Gemini 的用量只存在于网页会话里:没有 CLI 凭证可读,而浏览器 cookie 从 Chrome 127+ 起
 * 用 App-Bound Encryption(v20)加密,DPAPI 密钥解不开——绕过它要么冒充浏览器的 COM 提权
 * 接口、要么杀 network service 子进程,都不是一张用量卡片该干的事。
 *
 * 所以反过来:页面里本来就有活着的会话,让脚本在页面里取数,把**算好的两个百分比**
 * 推给卡片。卡片只在 127.0.0.1 上收数显示,永远不接触任何 Google 凭证。
 *
 * 装好后不需要任何配置:开着 Gemini 标签页,卡片上就会有数;关了就显示最后一次的值
 * 和它的时间。
 */

(function () {
    'use strict';

    const CARD_URL = 'http://127.0.0.1:__PORT__/gemini';
    const POLL_SECONDS = 60;

    const win = typeof unsafeWindow !== 'undefined' ? unsafeWindow : window;

    // --- 1. 嗅探真实的 batchexecute 地址 ---
    // 多账号登录时路由里会带 /u/1/ 之类的前缀,写死会 404。这里从页面自己发出的请求里
    // 偷一个真实 URL,再把 rpcids 换成配额那个 RPC。
    let realApiUrl = 'https://gemini.google.com/_/BardChatUi/data/batchexecute?rpcids=jSf9Qc&rt=c';

    function rememberUrl(u) {
        if (typeof u !== 'string' || !u.includes('batchexecute')) return;
        try {
            const o = new URL(u, location.origin);
            o.searchParams.set('rpcids', 'jSf9Qc');
            realApiUrl = o.toString();
        } catch (e) { /* 不是合法 URL 就算了 */ }
    }

    const origFetch = win.fetch;
    win.fetch = function (...args) {
        rememberUrl(typeof args[0] === 'string' ? args[0] : args[0] && args[0].url);
        return origFetch.apply(this, args);
    };

    const origOpen = win.XMLHttpRequest.prototype.open;
    win.XMLHttpRequest.prototype.open = function (method, url) {
        rememberUrl(url);
        return origOpen.apply(this, arguments);
    };

    // --- 2. 取数 ---
    function getToken() {
        try { if (win.WIZ_global_data && win.WIZ_global_data.SNlM0e) return win.WIZ_global_data.SNlM0e; } catch (e) {}
        const m = document.documentElement.outerHTML.match(/"SNlM0e":"([^"]+)"/);
        return m ? m[1] : null;
    }

    // 推给卡片。这里**必须**用 GM_xmlhttpRequest,不能图省事换成 fetch —— 有三道坎,
    // 只有它能同时过:
    //   1. CSP:gemini.google.com 的 connect-src 里没有 127.0.0.1(响应头由 Google 下发,
    //      我们改不了),页面内的 fetch/XHR 打到本地监听会被直接掐断。
    //   2. CORS:页面内请求要跨源就得卡片配 ACAO 头,而 GM 请求根本不走这套。
    //   3. Chrome 142+ 的本地网络访问限制:网页请求回环地址要弹权限框,但扩展豁免
    //      (Google 的 LNA 采用指南明确说不对扩展设限),而 GM_xmlhttpRequest 是走
    //      油猴的扩展后台发出去的。
    // 另外地址只能写 127.0.0.1 字面量,不能写 localhost:前者无条件算"可信源",
    // 后者要看 UA 是否实现了 let-localhost-be-localhost。
    function push(payload) {
        try {
            GM_xmlhttpRequest({
                method: 'POST',
                url: CARD_URL,
                headers: { 'Content-Type': 'application/json' },
                data: JSON.stringify(payload),
                timeout: 5000,
                onerror: () => {},      // 卡片没开着是常态,静默即可
                ontimeout: () => {},
            });
        } catch (e) { /* 同上 */ }
    }

    async function poll() {
        const token = getToken();
        if (!token) return;   // 页面还没渲染出 token,下一轮再说

        const body = new URLSearchParams({
            'f.req': '[[["jSf9Qc","[]",null,"generic"]]]',
            'at': token,
        });

        let text;
        try {
            const res = await origFetch.call(win, realApiUrl, {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
                body: body.toString(),
            });
            if (!res.ok) return;
            text = await res.text();
        } catch (e) {
            return;
        }

        // batchexecute 的响应带 )]}' 前缀 + 分块行,只挑我们要的那一行
        if (text.startsWith(")]}'")) text = text.slice(4);
        for (const line of text.split('\n')) {
            if (!line.includes('jSf9Qc') || !line.includes('wrb.fr')) continue;
            let quotas;
            try {
                quotas = JSON.parse(JSON.parse(line)[0][2])[1];
            } catch (e) {
                return;
            }
            const out = {};
            for (const q of quotas || []) {
                // q[2]: 1=短期(5 小时) 2=长期(本周);q[1]=已用比例(0~1);q[3][0][0]=重置时刻(秒)
                const slot = q[2] === 1 ? 'five_hour' : (q[2] === 2 ? 'seven_day' : null);
                if (!slot) continue;
                out[slot] = {
                    percent: q[1] * 100,
                    resets_at: (q[3] && q[3][0] && q[3][0][0]) || null,
                };
            }
            if (Object.keys(out).length) push(out);
            return;
        }
    }

    poll();
    setInterval(poll, POLL_SECONDS * 1000);
})();
