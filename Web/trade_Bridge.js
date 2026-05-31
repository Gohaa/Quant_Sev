/** trade_Bridge.js — 人工报单 / 撤单 HTTP → Gateway → Trade */
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

    function sendOrder(order) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var payload = {
            user_id: resolveUserId(order && order.user_id),
            instrument_id: (order.instrument_id || order.contract || '').trim(),
            direction: order.direction || 'buy',
            offset: (order.offset || 'open').replace(/-/g, '_'),
            price_type: order.price_type || order.orderType || 'limit',
            price: order.price,
            volume: order.volume
        };
        if (!payload.user_id) {
            return Promise.reject(new Error('请先在账户页选择并连接账户'));
        }
        if (!payload.instrument_id) {
            return Promise.reject(new Error('请选择合约'));
        }
        return t.post('/api/order', payload);
    }

    function cancelOrder(payload) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var req = payload || {};
        var body = {
            user_id: resolveUserId(req.user_id),
            instrument_id: (req.instrument_id || req.contract || '').trim(),
            order_ref: (req.order_ref || '').trim(),
            order_sys_id: (req.order_sys_id || '').trim(),
            exchange_id: (req.exchange_id || '').trim()
        };
        if (!body.user_id) {
            return Promise.reject(new Error('请先在账户页选择并连接账户'));
        }
        if (!body.order_ref && !body.order_sys_id) {
            return Promise.reject(new Error('order_ref 或 order_sys_id 必填'));
        }
        return t.post('/api/cancel_order', body);
    }

    function listTradeOrders(query) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var q = query || {};
        var params = {
            user_id: resolveUserId(q.user_id),
            limit: q.limit || 100
        };
        return t.get('/api/trade/orders', params);
    }

    function listTradeTrades(query) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var q = query || {};
        var params = {
            user_id: resolveUserId(q.user_id),
            limit: q.limit || 100
        };
        return t.get('/api/trade/trades', params);
    }

    function queryHistory(query) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var q = query || {};
        var params = {
            user_id: resolveUserId(q.user_id),
            type: q.type || 'trades',
            date_from: q.date_from || '',
            date_to: q.date_to || '',
            instrument_id: q.instrument_id || '',
            refresh: q.refresh ? '1' : '0',
            limit: q.limit || 500
        };
        return t.get('/api/trade/history', params);
    }

    function queryTradingAccount(query) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var q = query || {};
        var params = {
            user_id: resolveUserId(q.user_id),
            refresh: q.refresh ? '1' : '0'
        };
        return t.get('/api/trade/account', params);
    }

    function queryPositions(query) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var q = query || {};
        var params = {
            user_id: resolveUserId(q.user_id),
            refresh: q.refresh ? '1' : '0'
        };
        return t.get('/api/trade/positions', params);
    }

    global.QuantSevBridge = global.QuantSevBridge || {};
    global.QuantSevBridge.Trade = {
        sendOrder: sendOrder,
        cancelOrder: cancelOrder,
        listTradeOrders: listTradeOrders,
        listTradeTrades: listTradeTrades,
        queryHistory: queryHistory,
        queryTradingAccount: queryTradingAccount,
        queryPositions: queryPositions,
        resolveUserId: resolveUserId
    };
})(typeof window !== 'undefined' ? window : this);
