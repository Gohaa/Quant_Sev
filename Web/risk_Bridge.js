/** risk_Bridge.js — 风控 HTTP → Gateway → Risk / TimeCheck */
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

    function getStatus(userId) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.get(withUserQuery('/api/risk/status', userId));
    }

    function setHalt(halt, stopStrategies) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.post('/api/risk/halt', {
            halt: !!halt,
            halt_all_orders: !!halt,
            stop_strategies: !!stopStrategies
        });
    }

    function triggerEmergency(options) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var opt = options || {};
        var body = {
            user_id: resolveUserId(opt.user_id),
            halt: opt.halt !== false,
            stop_strategies: opt.stop_strategies !== false,
            flatten: !!opt.flatten,
            reset: !!opt.reset
        };
        return t.post('/api/risk/emergency', body);
    }

    function saveConfig(patch) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.post('/api/risk/config', patch || {});
    }

    function clearLatencyPause() {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.post('/api/risk/latency/clear', {});
    }

    global.QuantSevBridge = global.QuantSevBridge || {};
    global.QuantSevBridge.Risk = {
        getStatus: getStatus,
        setHalt: setHalt,
        triggerEmergency: triggerEmergency,
        saveConfig: saveConfig,
        clearLatencyPause: clearLatencyPause,
        resolveUserId: resolveUserId
    };
})(typeof window !== 'undefined' ? window : this);
