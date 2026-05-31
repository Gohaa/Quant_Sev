/** rollover_Bridge.js — 换月检测 / 日终快照 / Symbol_list 应用 */
(function (global) {
    'use strict';

    function transport() {
        return global.QuantSevBridge && QuantSevBridge.Logger && QuantSevBridge.Logger.Transport;
    }

    function resolveUserId(explicit) {
        if (explicit) return explicit;
        try {
            var stored = global.localStorage && localStorage.getItem('quant_sev_user_id');
            if (stored) return stored;
        } catch (e) { /* ignore */ }
        return '';
    }

    function getSuggest() {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.get('/api/rollover/suggest');
    }

    function getDaily() {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.get('/api/rollover/daily');
    }

    function snapshot(tradingDay) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var body = {};
        if (tradingDay) body.trading_day = tradingDay;
        return t.post('/api/rollover/snapshot', body);
    }

    function apply(symbolId, newInstrumentId, userId) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.post('/api/rollover/apply', {
            symbol_id: symbolId,
            new_instrument_id: newInstrumentId,
            user_id: resolveUserId(userId),
            resubscribe: true
        });
    }

    function subscribeNext(instrumentId, userId) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.get('/api/rollover/next?instrument_id=' + encodeURIComponent(instrumentId))
            .then(function (res) {
                var nextId = res && res.next_instrument_id;
                if (!nextId) return Promise.reject(new Error('无法推算次月合约'));
                return t.post('/api/load/symbol', {
                    user_id: resolveUserId(userId),
                    instrument_ids: [nextId]
                });
            });
    }

    global.QuantSevBridge = global.QuantSevBridge || {};
    global.QuantSevBridge.Rollover = {
        getSuggest: getSuggest,
        getDaily: getDaily,
        snapshot: snapshot,
        apply: apply,
        subscribeNext: subscribeNext,
        resolveUserId: resolveUserId
    };
})(typeof window !== 'undefined' ? window : this);
