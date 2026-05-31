/** cta_Bridge.js — CTA 资管查询 HTTP → Gateway */
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

    function withUserQuery(path, userId) {
        var uid = resolveUserId(userId);
        if (!uid) return path;
        var sep = path.indexOf('?') >= 0 ? '&' : '?';
        return path + sep + 'user_id=' + encodeURIComponent(uid);
    }

    function getPositions(userId) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.get(withUserQuery('/api/cta/positions', userId));
    }

    function getOrders(userId, limit) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var q = withUserQuery('/api/cta/orders', userId);
        if (limit) q += (q.indexOf('?') >= 0 ? '&' : '?') + 'limit=' + encodeURIComponent(limit);
        return t.get(q);
    }

    function getAccount(userId) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.get(withUserQuery('/api/cta/account', userId));
    }

    global.QuantSevBridge = global.QuantSevBridge || {};
    global.QuantSevBridge.Cta = {
        getPositions: getPositions,
        getOrders: getOrders,
        getAccount: getAccount,
        resolveUserId: resolveUserId
    };
})(typeof window !== 'undefined' ? window : this);
