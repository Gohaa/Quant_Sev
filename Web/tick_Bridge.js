/** tick_Bridge.js — WebSocket Tick / 委托回报订阅 */
(function (global) {
    'use strict';

    var ws = null;
    var wsPort = 8081;
    var reconnectTimer = null;
    var tickListeners = [];
    var orderListeners = [];
    var tradeListeners = [];

    function wsUrl() {
        var proto = (location.protocol === 'https:') ? 'wss:' : 'ws:';
        var host = location.hostname || '127.0.0.1';
        return proto + '//' + host + ':' + wsPort + '/ws/tick';
    }

    function dispatchTick(data) {
        tickListeners.forEach(function (fn) {
            try { fn(data); } catch (e) { console.error('tick listener', e); }
        });
        try {
            global.dispatchEvent(new CustomEvent('quant-sev-tick', { detail: data }));
        } catch (e) { /* ignore */ }
    }

    function dispatchOrder(data) {
        orderListeners.forEach(function (fn) {
            try { fn(data); } catch (e) { console.error('order listener', e); }
        });
        try {
            global.dispatchEvent(new CustomEvent('quant-sev-order', { detail: data }));
        } catch (e) { /* ignore */ }
    }

    function dispatchTrade(data) {
        tradeListeners.forEach(function (fn) {
            try { fn(data); } catch (e) { console.error('trade listener', e); }
        });
        try {
            global.dispatchEvent(new CustomEvent('quant-sev-trade', { detail: data }));
        } catch (e) { /* ignore */ }
    }

    function connect() {
        if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
            return;
        }
        try {
            ws = new WebSocket(wsUrl());
        } catch (e) {
            scheduleReconnect();
            return;
        }
        ws.onopen = function () {
            if (reconnectTimer) {
                clearTimeout(reconnectTimer);
                reconnectTimer = null;
            }
        };
        ws.onmessage = function (ev) {
            try {
                var msg = JSON.parse(ev.data);
                if (!msg || !msg.type) return;
                if (msg.type === 'tick' && msg.data) {
                    dispatchTick(msg.data);
                } else if (msg.type === 'order' && msg.data) {
                    dispatchOrder(msg.data);
                } else if (msg.type === 'trade' && msg.data) {
                    dispatchTrade(msg.data);
                }
            } catch (e) {
                console.warn('ws parse', e);
            }
        };
        ws.onclose = function () {
            ws = null;
            scheduleReconnect();
        };
        ws.onerror = function () {
            /* onclose handles reconnect */
        };
    }

    function scheduleReconnect() {
        if (reconnectTimer) return;
        reconnectTimer = setTimeout(function () {
            reconnectTimer = null;
            connect();
        }, 3000);
    }

    function loadWsPort() {
        if (!global.QuantSevBridge || !QuantSevBridge.Logger || !QuantSevBridge.Logger.Transport) return;
        QuantSevBridge.Logger.Transport.get('/api/status').then(function (st) {
            if (st && st.ws_port) wsPort = st.ws_port;
            connect();
        }).catch(function () { connect(); });
    }

    global.QuantSevBridge = global.QuantSevBridge || {};
    global.QuantSevBridge.Tick = {
        connect: connect,
        disconnect: function () {
            if (reconnectTimer) clearTimeout(reconnectTimer);
            reconnectTimer = null;
            if (ws) ws.close();
            ws = null;
        },
        onTick: function (fn) {
            if (typeof fn === 'function') tickListeners.push(fn);
        },
        offTick: function (fn) {
            tickListeners = tickListeners.filter(function (f) { return f !== fn; });
        },
        onOrder: function (fn) {
            if (typeof fn === 'function') orderListeners.push(fn);
        },
        offOrder: function (fn) {
            orderListeners = orderListeners.filter(function (f) { return f !== fn; });
        },
        onTrade: function (fn) {
            if (typeof fn === 'function') tradeListeners.push(fn);
        },
        offTrade: function (fn) {
            tradeListeners = tradeListeners.filter(function (f) { return f !== fn; });
        }
    };

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', loadWsPort);
    } else {
        loadWsPort();
    }
})(typeof window !== 'undefined' ? window : this);
