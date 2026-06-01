/** trade_Bridge.js — 人工报单 / 撤单 HTTP → Gateway → Trade */
(function (global) {
    'use strict';

    function transport() {
        return global.QuantSevBridge && QuantSevBridge.Logger && QuantSevBridge.Logger.Transport;
    }

    function resolveUserId(explicit) {
        return resolveAccountContext(explicit).user_id;
    }

    function resolveAccountContext(explicit) {
        var ctx = { user_id: '', name: '', md_front: '', td_front: '' };
        if (explicit && typeof explicit === 'object') {
            ctx.user_id = explicit.user_id || '';
            ctx.name = explicit.name || '';
            ctx.md_front = explicit.md_front || '';
            ctx.td_front = explicit.td_front || explicit.trader_front || '';
        } else if (explicit) {
            ctx.user_id = explicit;
        }
        try {
            if (global.localStorage) {
                if (!ctx.user_id) ctx.user_id = localStorage.getItem('quant_sev_user_id') || '';
                if (!ctx.name) ctx.name = localStorage.getItem('quant_sev_account_name') || '';
                if (!ctx.md_front) ctx.md_front = localStorage.getItem('quant_sev_md_front') || '';
                if (!ctx.td_front) ctx.td_front = localStorage.getItem('quant_sev_td_front') || '';
            }
        } catch (e) { /* ignore */ }
        return ctx;
    }

    function withAccountParams(query) {
        var q = query || {};
        var ctx = resolveAccountContext(q);
        var params = {
            user_id: ctx.user_id,
            name: ctx.name,
            md_front: ctx.md_front,
            td_front: ctx.td_front,
            trader_front: ctx.td_front
        };
        Object.keys(q).forEach(function (key) {
            if (q[key] !== undefined && q[key] !== null && q[key] !== '') {
                params[key] = q[key];
            }
        });
        return params;
    }

    function sendOrder(order) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var ctx = resolveAccountContext(order);
        var payload = {
            name: ctx.name,
            user_id: ctx.user_id,
            md_front: ctx.md_front,
            td_front: ctx.td_front,
            trader_front: ctx.td_front,
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
        var ctx = resolveAccountContext(req);
        var body = {
            name: ctx.name,
            user_id: ctx.user_id,
            td_front: ctx.td_front,
            trader_front: ctx.td_front,
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
        var params = withAccountParams(Object.assign({ limit: q.limit || 100 }, q));
        return t.get('/api/trade/orders', params);
    }

    function listTradeTrades(query) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var q = query || {};
        var params = withAccountParams(Object.assign({ limit: q.limit || 100 }, q));
        return t.get('/api/trade/trades', params);
    }

    function queryHistory(query) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var q = query || {};
        var params = withAccountParams({
            type: q.type || 'trades',
            date_from: q.date_from || '',
            date_to: q.date_to || '',
            instrument_id: q.instrument_id || '',
            refresh: q.refresh ? '1' : '0',
            limit: q.limit || 500,
            user_id: q.user_id
        });
        return t.get('/api/trade/history', params);
    }

    function queryTradingAccount(query) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var q = query || {};
        var params = withAccountParams({
            refresh: q.refresh ? '1' : '0',
            user_id: q.user_id
        });
        return t.get('/api/trade/account', params);
    }

    function queryPositions(query) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var q = query || {};
        var params = withAccountParams({
            refresh: q.refresh ? '1' : '0',
            user_id: q.user_id
        });
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
        resolveUserId: resolveUserId,
        resolveAccountContext: resolveAccountContext
    };
})(typeof window !== 'undefined' ? window : this);
