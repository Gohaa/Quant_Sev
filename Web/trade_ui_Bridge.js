/** trade_ui_Bridge.js — Trade 页合约看板 / 图表·盘口·报单联动 / 账户刷新 */
(function (global) {
    'use strict';

    var selectedContract = '';
    var searchFilter = '';
    var lastBoard = [];

    function transport() {
        return global.QuantSevBridge && QuantSevBridge.Logger && QuantSevBridge.Logger.Transport;
    }

    function resolveSelected() {
        if (selectedContract) return selectedContract;
        try {
            var stored = global.localStorage && localStorage.getItem('quant_sev_chart_instrument');
            if (stored) return String(stored).toLowerCase();
        } catch (e) { /* ignore */ }
        return 'rb2610';
    }

    function escapeHtml(text) {
        return String(text == null ? '' : text)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;');
    }

    function formatPrice(v) {
        if (v == null || v === '' || Number(v) <= 0) return '—';
        return Number(v).toFixed(2);
    }

    function formatPct(q, item) {
        if (!q) q = {};
        var last = q.last_price || q.LastPrice;
        if (last == null || Number(last) <= 0) return '—';
        var base = q.pre_close || q.PreClosePrice || q.pre_settlement || q.PreSettlementPrice;
        if (base == null || Number(base) <= 0) {
            base = q.open_price || q.OpenPrice;
        }
        if (item && item.pre_close != null && Number(item.pre_close) > 0) {
            base = item.pre_close;
        }
        if (base == null || Number(base) <= 0) return '—';
        var pct = ((Number(last) - Number(base)) / Number(base)) * 100;
        var cls = pct >= 0 ? 'price-up' : 'price-down';
        var sign = pct >= 0 ? '+' : '';
        return '<span class="' + cls + '">' + sign + pct.toFixed(2) + '%</span>';
    }

    function quoteRow(item) {
        return (item && item.quote) || item || {};
    }

    function instrumentId(item) {
        return String((item && item.instrument_id) || '').toLowerCase();
    }

    function selectContract(instrumentId) {
        if (!instrumentId) return;
        var id = String(instrumentId).toLowerCase();
        selectedContract = id;
        try {
            global.localStorage.setItem('quant_sev_chart_instrument', id);
            global.localStorage.setItem('quant_sev_last_contract', id);
        } catch (e) { /* ignore */ }

        global.dispatchEvent(new CustomEvent('quant-sev-chart-instrument', { detail: id }));

        var controlFrame = document.getElementById('trade-control-frame-right');
        if (controlFrame && controlFrame.contentWindow) {
            try {
                controlFrame.contentWindow.postMessage({ type: 'quant-sev-set-contract', instrument_id: id }, '*');
            } catch (e) { /* ignore */ }
        }

        renderContractTable(lastBoard);
    }

    function renderContractTable(quotes) {
        lastBoard = quotes || [];
        var tbody = document.getElementById('trade-contract-tbody');
        if (!tbody) return;

        var selected = resolveSelected();
        var filter = (searchFilter || '').trim().toUpperCase();
        var rows = lastBoard.filter(function (item) {
            var id = instrumentId(item);
            if (!id) return false;
            if (filter && id.toUpperCase().indexOf(filter) < 0) {
                var name = String(item.name || '').toUpperCase();
                if (name.indexOf(filter) < 0) return false;
            }
            return true;
        });

        if (!rows.length) {
            tbody.innerHTML = '<tr><td colspan="8">暂无行情（请先连接 Md 并订阅合约）</td></tr>';
            return;
        }

        tbody.innerHTML = rows.map(function (item) {
            var id = instrumentId(item);
            var q = quoteRow(item);
            var cls = id === selected ? ' class="selected"' : '';
            return '<tr data-inst="' + escapeHtml(id) + '"' + cls + '>' +
                '<td>' + escapeHtml(id) + '</td>' +
                    '<td>' + formatPrice(q.last_price) + '</td>' +
                    '<td>' + formatPct(q, item) + '</td>' +
                '<td>' + formatPrice(q.bid_price1) + '</td>' +
                '<td>' + formatPrice(q.ask_price1) + '</td>' +
                '<td>' + escapeHtml(q.volume != null ? q.volume : '—') + '</td>' +
                '<td>' + escapeHtml(q.open_interest != null ? q.open_interest : '—') + '</td>' +
                '<td>' + escapeHtml(q.update_time || '—') + '</td></tr>';
        }).join('');

        tbody.querySelectorAll('tr[data-inst]').forEach(function (tr) {
            tr.addEventListener('click', function () {
                selectContract(tr.getAttribute('data-inst'));
            });
        });
    }

    function setText(id, text) {
        var el = document.getElementById(id);
        if (el) el.textContent = text;
    }

    function setMoney(id, value, prefix) {
        var el = document.getElementById(id);
        if (!el) return;
        if (value == null || isNaN(Number(value))) {
            el.textContent = '—';
            return;
        }
        el.textContent = (prefix || '¥') + Number(value).toLocaleString('zh-CN', {
            minimumFractionDigits: 0,
            maximumFractionDigits: 0
        });
    }

    function refreshAccount(refresh) {
        if (!global.QuantSevBridge || !QuantSevBridge.Trade) return Promise.resolve();
        var uid = QuantSevBridge.Trade.resolveUserId();
        if (!uid) {
            setText('trade-acct-user', '未连接');
            return Promise.resolve();
        }
        return QuantSevBridge.Trade.queryTradingAccount({ user_id: uid, refresh: !!refresh })
            .then(function (data) {
                var acc = (data && data.account) || {};
                if (data.error) {
                    setText('trade-acct-user', uid);
                    return;
                }
                if (!data.cached && !acc.balance) {
                    setText('trade-acct-user', uid + ' (无缓存)');
                    return;
                }
                setText('trade-acct-user', acc.account_id || uid);
                setMoney('trade-acct-available', acc.available);
                setMoney('trade-acct-balance', acc.balance);
                setMoney('trade-acct-margin', acc.curr_margin);
                var pnlEl = document.getElementById('trade-acct-pnl');
                if (pnlEl) {
                    var pnl = acc.position_profit != null ? acc.position_profit : acc.close_profit;
                    if (pnl == null || isNaN(Number(pnl))) {
                        pnlEl.textContent = '—';
                        pnlEl.style.color = '#e0e0e0';
                    } else {
                        var n = Number(pnl);
                        pnlEl.textContent = (n >= 0 ? '+' : '') + '¥' + n.toLocaleString('zh-CN', { maximumFractionDigits: 0 });
                        pnlEl.style.color = n >= 0 ? '#e74c3c' : '#2ecc71';
                    }
                }
            })
            .catch(function () {
                setText('trade-acct-user', uid);
            });
    }

    function syncFromRunningStrategy() {
        var t = transport();
        if (!t) return;
        t.get('/api/diagnostic/pipeline').then(function (data) {
            var strategies = data.strategies || [];
            for (var i = 0; i < strategies.length; i++) {
                if (strategies[i].state === 'running' && strategies[i].instrument_id) {
                    var current = resolveSelected();
                    if (!current || current === 'rb2610') {
                        selectContract(strategies[i].instrument_id);
                    }
                    break;
                }
            }
        }).catch(function () { /* ignore */ });
    }

    function init() {
        selectedContract = resolveSelected();
        var search = document.getElementById('trade-contract-search');
        if (search) {
            search.addEventListener('input', function () {
                searchFilter = search.value || '';
                renderContractTable(lastBoard);
            });
        }

        global.addEventListener('quant-sev-quote-board', function (ev) {
            renderContractTable((ev && ev.detail) || []);
        });

        global.addEventListener('quant-sev-chart-instrument', function (ev) {
            if (ev && ev.detail) {
                selectedContract = String(ev.detail).toLowerCase();
                renderContractTable(lastBoard);
            }
        });

        global.addEventListener('quant-sev-trade', function () {
            refreshAccount(false);
        });

        if (global.QuantSevBridge && QuantSevBridge.TradeView) {
            renderContractTable(QuantSevBridge.TradeView.getLastBoard());
        }

        refreshAccount(false);
        setInterval(function () { refreshAccount(false); }, 15000);
        syncFromRunningStrategy();
    }

    global.QuantSevBridge = global.QuantSevBridge || {};
    global.QuantSevBridge.TradeUi = {
        selectContract: selectContract,
        refreshAccount: refreshAccount,
        renderContractTable: renderContractTable,
        init: init
    };

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }
})(typeof window !== 'undefined' ? window : this);
