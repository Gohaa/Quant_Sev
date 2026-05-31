/** orderbook_Bridge.js — GET /api/orderbook */
(function (global) {
    'use strict';

    function transport() {
        return global.QuantSevBridge && QuantSevBridge.Logger && QuantSevBridge.Logger.Transport;
    }

    function getSnapshot(instrumentId, depth) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var d = depth || 5;
        var path = '/api/orderbook?instrument_id=' + encodeURIComponent(instrumentId) + '&depth=' + d;
        return t.get(path);
    }

    global.QuantSevBridge = global.QuantSevBridge || {};
    global.QuantSevBridge.OrderBook = {
        getSnapshot: getSnapshot
    };
})(typeof window !== 'undefined' ? window : this);
