/** diagnostic_Bridge.js — SimNow 联调诊断 → Gateway */
(function (global) {
    'use strict';

    function transport() {
        return global.QuantSevBridge && QuantSevBridge.Logger && QuantSevBridge.Logger.Transport;
    }

    function escapeHtml(text) {
        return String(text == null ? '' : text)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;');
    }

    function badge(ok, okText, failText) {
        var cls = ok ? 'diag-ok' : 'diag-fail';
        return '<span class="diag-badge ' + cls + '">' + escapeHtml(ok ? okText : failText) + '</span>';
    }

    function getPipeline() {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.get('/api/diagnostic/pipeline');
    }

    function verifyBars(instrumentId, period, tail) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var q = '/api/diagnostic/bars?instrument_id=' + encodeURIComponent(instrumentId) +
            '&period=' + encodeURIComponent(period || 'm1');
        if (tail) q += '&tail=' + encodeURIComponent(tail);
        return t.get(q);
    }

    function verifyBacktestBars(instrumentId, period, limit) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var q = '/api/diagnostic/backtest-bars?instrument_id=' + encodeURIComponent(instrumentId) +
            '&period=' + encodeURIComponent(period || 'm1');
        if (limit) q += '&limit=' + encodeURIComponent(limit);
        return t.get(q);
    }

    function renderPanel(containerId, data) {
        var el = document.getElementById(containerId);
        if (!el || !data) return;

        var summary = data.cta_position_summary || {};
        var barChecks = data.bar_checks || [];
        var reconnect = data.reconnect || {};

        var html = '<div class="diag-grid">' +
            '<div class="diag-item">' + badge(data.md_ok, 'Md 已连', 'Md 未连') + '</div>' +
            '<div class="diag-item">' + badge(data.td_ok, 'Td 已连', 'Td 未连') + '</div>' +
            '<div class="diag-item">' + badge(data.in_session, '交易时段', '非交易时段') +
            ' <span class="diag-muted">' + escapeHtml(data.session_phase || '') + '</span></div>' +
            '<div class="diag-item">订阅 <b>' + escapeHtml(data.subscribed_count) + '</b> 合约</div>' +
            '<div class="diag-item">策略 <b>' + escapeHtml(data.strategies_running) + '</b>/' +
            escapeHtml(data.strategies_total) + ' 运行</div>' +
            '<div class="diag-item">CTA 持仓 <b>' + escapeHtml(summary.total_contracts || 0) + '</b> 合约 ' +
            '(多' + escapeHtml(summary.total_long || 0) + '/空' + escapeHtml(summary.total_short || 0) + ')</div>' +
            '</div>';

        if (data.halt_all_orders || data.halt_all_strategies) {
            html += '<div class="diag-alert">风控暂停: ' +
                (data.halt_all_orders ? '停单 ' : '') +
                (data.halt_all_strategies ? '停策略' : '') + '</div>';
        }

        if (reconnect.md_in_progress || reconnect.td_in_progress) {
            html += '<div class="diag-muted">重连: Md=' + (reconnect.md_in_progress ? '进行中' : '—') +
                ' Td=' + (reconnect.td_in_progress ? '进行中' : '—') + '</div>';
        }

        if (barChecks.length) {
            html += '<table class="diag-table"><thead><tr>' +
                '<th>策略</th><th>合约</th><th>周期</th><th>Bar数</th><th>末Bar</th><th>对齐</th></tr></thead><tbody>';
            barChecks.forEach(function (row) {
                var last = row.file_last || {};
                var lastText = last.date ? (last.date + ' ' + (last.time || '') + ' @' + last.close) : '—';
                html += '<tr><td>' + escapeHtml(row.strategy_id) + '</td>' +
                    '<td>' + escapeHtml(row.instrument_id) + '</td>' +
                    '<td>' + escapeHtml(row.period) + '</td>' +
                    '<td>' + escapeHtml(row.file_bar_count) + '</td>' +
                    '<td class="diag-muted">' + escapeHtml(lastText) + '</td>' +
                    '<td>' + badge(row.aligned, 'OK', row.file_exists ? '不一致' : '无文件') + '</td></tr>';
            });
            html += '</tbody></table>';
        } else if ((data.strategies_running || 0) > 0) {
            html += '<p class="diag-muted">运行中策略暂无 Bar 检查项</p>';
        } else {
            html += '<p class="diag-muted">无运行中策略 — 启动 demo_m15_dual / demo_m1_ma 后可验证 Bar 对齐</p>';
        }

        var positions = data.cta_positions || [];
        if (positions.length) {
            html += '<div class="diag-subtitle">CTA 持仓明细</div><table class="diag-table"><thead><tr>' +
                '<th>合约</th><th>多</th><th>空</th><th>多均价</th><th>空均价</th></tr></thead><tbody>';
            positions.forEach(function (p) {
                html += '<tr><td>' + escapeHtml(p.instrument_id) + '</td>' +
                    '<td>' + escapeHtml(p.long || 0) + '</td><td>' + escapeHtml(p.short || 0) + '</td>' +
                    '<td>' + escapeHtml(p.avg_long_price ? Number(p.avg_long_price).toFixed(0) : '—') + '</td>' +
                    '<td>' + escapeHtml(p.avg_short_price ? Number(p.avg_short_price).toFixed(0) : '—') + '</td></tr>';
            });
            html += '</tbody></table>';
        }

        el.innerHTML = html;
    }

    function refreshPanel(containerId) {
        return getPipeline().then(function (data) {
            renderPanel(containerId, data);
            return data;
        }).catch(function (err) {
            var el = document.getElementById(containerId);
            if (el) el.innerHTML = '<p class="diag-fail">诊断加载失败: ' + escapeHtml(err.message) + '</p>';
        });
    }

    global.QuantSevBridge = global.QuantSevBridge || {};
    global.QuantSevBridge.Diagnostic = {
        getPipeline: getPipeline,
        verifyBars: verifyBars,
        verifyBacktestBars: verifyBacktestBars,
        renderPanel: renderPanel,
        refreshPanel: refreshPanel
    };
})(typeof window !== 'undefined' ? window : this);
