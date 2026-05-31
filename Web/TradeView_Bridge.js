/** TradeView_Bridge.js — 合约看板轮询 + Tick 绑定 */
(function (global) {
    'use strict';

    var pollTimer = null;
    var lastBoard = [];

    function transport() {
        return global.QuantSevBridge && QuantSevBridge.Logger && QuantSevBridge.Logger.Transport;
    }

    function pollQuoteBoard() {
        if (!transport() || !transport().isHttp()) return;
        transport().get('/api/md_quote_board').then(function (data) {
            lastBoard = (data && data.quotes) || [];
            global.dispatchEvent(new CustomEvent('quant-sev-quote-board', { detail: lastBoard }));
        }).catch(function () {});
    }

    function subscribeSymbols(userId, instrumentIds) {
        if (!transport()) return Promise.reject(new Error('no transport'));
        return transport().post('/api/load/symbol', {
            user_id: userId || '',
            instrument_ids: instrumentIds || []
        });
    }

    function subscribeAll(userId) {
        if (!transport()) return Promise.reject(new Error('no transport'));
        return transport().post('/api/load/symbol', {
            user_id: userId || '',
            subscribe_all: true
        });
    }

    global.QuantSevBridge = global.QuantSevBridge || {};
    global.QuantSevBridge.TradeView = {
        init: function () {
            pollQuoteBoard();
            if (pollTimer) clearInterval(pollTimer);
            pollTimer = setInterval(pollQuoteBoard, 2000);
        },
        pollQuoteBoard: pollQuoteBoard,
        getLastBoard: function () { return lastBoard.slice(); },
        subscribeSymbols: subscribeSymbols,
        subscribeAll: subscribeAll,
        loadSymbols: function () {
            if (!transport()) return Promise.reject(new Error('no transport'));
            return transport().get('/api/symbols');
        },
        loadBars: function (instrumentId, period, limit) {
            if (!transport()) return Promise.reject(new Error('no transport'));
            var q = '/api/bars?instrument_id=' + encodeURIComponent(instrumentId || '');
            q += '&period=' + encodeURIComponent(period || 'm1');
            q += '&limit=' + encodeURIComponent(String(limit || 500));
            return transport().get(q);
        },
        loadIndicator: function (instrumentId, name, period, options, limit) {
            if (!transport()) return Promise.reject(new Error('no transport'));
            var q = '/api/indicator?instrument_id=' + encodeURIComponent(instrumentId || '');
            q += '&name=' + encodeURIComponent(name || '');
            q += '&period=' + encodeURIComponent(period || 'm1');
            q += '&limit=' + encodeURIComponent(String(limit || 500));
            if (options != null && options !== '') {
                var opt = Array.isArray(options) ? options.join(',') : String(options);
                q += '&options=' + encodeURIComponent(opt);
            }
            return transport().get(q);
        },
        listIndicators: function () {
            if (!transport()) return Promise.reject(new Error('no transport'));
            return transport().get('/api/indicators');
        },
    };

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', function () {
            if (global.QuantSevBridge.TradeView) QuantSevBridge.TradeView.init();
        });
    } else if (global.QuantSevBridge.TradeView) {
        QuantSevBridge.TradeView.init();
    }
})(typeof window !== 'undefined' ? window : this);
