/** Backtest_Bridge.js — 回测 HTTP → Gateway → BacktestEngine */
(function (global) {
    'use strict';

    function transport() {
        return global.QuantSevBridge && QuantSevBridge.Logger && QuantSevBridge.Logger.Transport;
    }

    function fmtPct(v) {
        return (Number(v || 0) * 100).toFixed(2) + '%';
    }

    function fmtMoney(v) {
        return '¥' + Number(v || 0).toLocaleString('zh-CN', { maximumFractionDigits: 0 });
    }

    function val(id) {
        var el = document.getElementById(id);
        return el ? el.value : '';
    }

    function num(id, fallback) {
        var n = Number(val(id));
        return isNaN(n) ? fallback : n;
    }

    var ACTION_ZH = {
        buy: '买入开多',
        sell: '卖出开空',
        close_long: '平多',
        close_short: '平空'
    };

    var SIDE_ZH = {
        long: '多头',
        short: '空头'
    };

    function actionZh(action) {
        return ACTION_ZH[action] || action || '—';
    }

    function sideZh(side) {
        return SIDE_ZH[side] || side || '—';
    }

    function barTimeAt(result, idx) {
        var bars = (result && result.bars) || [];
        if (idx == null || idx < 0 || idx >= bars.length) return '—';
        return barTimeKey(bars[idx]);
    }

    function signalTime(result, signal) {
        if (signal && signal.time) return signal.time;
        return barTimeAt(result, signal && signal.bar);
    }

    function sumField(rows, field) {
        return (rows || []).reduce(function (acc, row) {
            return acc + Number(row[field] || 0);
        }, 0);
    }

    function winRate(rows) {
        if (!rows || !rows.length) return 0;
        var wins = rows.filter(function (r) { return Number(r.pnl) > 0; }).length;
        return wins / rows.length;
    }

    function downsampleCurve(curve, maxPoints) {
        if (!curve || !curve.length || curve.length <= maxPoints) return curve || [];
        var step = Math.ceil(curve.length / maxPoints);
        var out = [];
        for (var i = 0; i < curve.length; i += step) {
            out.push(curve[i]);
        }
        var last = curve[curve.length - 1];
        if (out[out.length - 1] !== last) out.push(last);
        return out;
    }

    function getChart(host) {
        if (!host || !global.echarts) return null;
        var chart = global.echarts.getInstanceByDom(host);
        if (!chart) chart = global.echarts.init(host, 'dark');
        return chart;
    }

    function resizeChartHost(host) {
        if (!host) return;
        var chart = global.echarts.getInstanceByDom(host);
        if (chart) chart.resize();
    }

    function scheduleChartResize(hostOrId) {
        var host = typeof hostOrId === 'string' ? document.getElementById(hostOrId) : hostOrId;
        if (!host) return;
        requestAnimationFrame(function () {
            resizeChartHost(host);
            setTimeout(function () { resizeChartHost(host); }, 60);
            setTimeout(function () { resizeChartHost(host); }, 250);
        });
    }

    function resizeChartsInPanel(panel) {
        if (!panel) return;
        scheduleChartResize(panel.querySelector('#bt-equity-chart'));
        scheduleChartResize(panel.querySelector('#bt-bar-chart'));
        scheduleChartResize(panel.querySelector('#bt-verify-chart'));
    }

    function pickEquityCurve(result) {
        if (!result) return [];
        if (result.equity_curve_bars && result.equity_curve_bars.length) {
            return result.equity_curve_bars;
        }
        return result.equity_curve || [];
    }

    function equityXLabel(point) {
        if (point.bar != null) return String(point.bar);
        if (point.index != null) return String(point.index);
        return '';
    }

    function renderComparison(result) {
        var grid = document.getElementById('bt-perf-grid');
        if (!grid) return;
        var cmp = result.comparison || {};
        var items = [
            ['Bar 收益率', fmtPct(cmp.bar_total_return)],
            ['Tick 收益率', fmtPct(cmp.tick_total_return)],
            ['收益差 (Tick-Bar)', fmtPct(cmp.return_diff)],
            ['Bar 交易次数', cmp.bar_trade_count],
            ['Tick 交易次数', cmp.tick_trade_count],
            ['Bar 最大回撤', fmtPct(cmp.bar_max_drawdown)],
            ['Tick 最大回撤', fmtPct(cmp.tick_max_drawdown)]
        ];
        grid.innerHTML = items.map(function (row) {
            return '<div class="metric-card"><div class="label">' + row[0] +
                '</div><div class="value">' + row[1] + '</div></div>';
        }).join('');
    }

    function renderMultiRanking(result) {
        var grid = document.getElementById('bt-perf-grid');
        if (!grid) return;
        var ranking = result.ranking || [];
        if (!ranking.length) {
            grid.innerHTML = '<div class="metric-card"><div class="label">无结果</div></div>';
            return;
        }
        grid.innerHTML = ranking.map(function (row, idx) {
            var title = row.strategy || row.instrument_id || ('#' + (idx + 1));
            return '<div class="metric-card"><div class="label">#' + (idx + 1) + ' ' + title +
                '</div><div class="value">' + fmtPct(row.total_return) + '</div>' +
                '<div class="label" style="margin-top:4px;">交易 ' + row.trade_count +
                ' · 回撤 ' + fmtPct(row.max_drawdown) + '</div></div>';
        }).join('');
    }

    function renderMultiSymbolDetail(result) {
        var body = document.getElementById('bt-data-body');
        if (!body) return;
        var rows = [['模式', '多合约对比'], ['合约数', (result.instrument_ids || []).length]];
        (result.ranking || []).forEach(function (row, idx) {
            rows.push(['#' + (idx + 1) + ' ' + row.instrument_id, fmtPct(row.total_return) + ' · ' + row.trade_count + ' 笔']);
        });
        body.innerHTML = rows.map(function (r) {
            return '<tr><td>' + r[0] + '</td><td>' + r[1] + '</td></tr>';
        }).join('');
    }

    var lastStrategyInputs = [];
    var OPT_RESULT_KEYS = {
        strategy: 1, total_return: 1, max_drawdown: 1, win_rate: 1,
        trade_count: 1, final_equity: 1, error: 1, params: 1
    };

    function formatOptParams(row) {
        if (row && row.params && typeof row.params === 'object') {
            row = row.params;
        }
        var parts = [];
        (lastStrategyInputs || []).forEach(function (inp) {
            if (row && row[inp.key] != null) {
                parts.push((inp.var_name || inp.key) + '=' + row[inp.key]);
            }
        });
        if (!parts.length && row) {
            Object.keys(row).forEach(function (k) {
                if (OPT_RESULT_KEYS[k] || row[k] == null) return;
                parts.push(k + '=' + row[k]);
            });
        }
        return parts.join(' · ') || '—';
    }

    var lastOptMultiResult = null;
    var optMultiSymbolIndex = 0;
    var btPageMode = 'run';

    function renderOptRankingTable(ranking, summaryEl, bodyEl, result) {
        var rankingList = ranking || [];
        var best = (rankingList.length ? rankingList[0] : {});
        if (summaryEl) {
            var prefix = result && result.instrument_id ? ('[' + result.instrument_id + '] ') : '';
            summaryEl.textContent = rankingList.length
                ? (prefix + '共 ' + ((result && result.total_combos) || rankingList.length) + ' 组 · 最优: ' +
                    formatOptParams(best) + ' · 收益 ' + fmtPct(best.total_return))
                : '无有效结果';
        }
        if (!bodyEl) return;
        if (!rankingList.length) {
            bodyEl.innerHTML = '<tr><td colspan="6">无结果</td></tr>';
            return;
        }
        bodyEl.innerHTML = rankingList.map(function (row, idx) {
            return '<tr><td>' + (idx + 1) + '</td><td>' + formatOptParams(row) + '</td><td>' +
                fmtPct(row.total_return) + '</td><td>' + fmtPct(row.win_rate) + '</td><td>' +
                fmtPct(row.max_drawdown) + '</td><td>' + (row.trade_count || 0) + '</td></tr>';
        }).join('');
    }

    function renderOptBestSummary(result) {
        var summary = document.getElementById('bt-opt-best-summary');
        var body = document.getElementById('bt-opt-best-body');
        if (!body) return;
        var rows = result.best_summary || [];
        if (!rows.length && result.best && result.instrument_id) {
            rows = [Object.assign({ instrument_id: result.instrument_id }, result.best)];
        }
        if (summary) {
            summary.textContent = rows.length
                ? ('共 ' + rows.length + ' 个合约 · 全局最优: ' +
                    (rows[0].instrument_id || '—') + ' ' + fmtPct(rows[0].total_return))
                : '无汇总结果';
        }
        if (!rows.length) {
            body.innerHTML = '<tr><td colspan="7">无结果</td></tr>';
            return;
        }
        body.innerHTML = rows.map(function (row, idx) {
            return '<tr><td>' + (idx + 1) + '</td><td>' + (row.instrument_id || '—') + '</td><td>' +
                formatOptParams(row) + '</td><td>' + fmtPct(row.total_return) + '</td><td>' +
                fmtPct(row.win_rate) + '</td><td>' + fmtPct(row.max_drawdown) + '</td><td>' +
                (row.trade_count || 0) + '</td></tr>';
        }).join('');
    }

    function updateOptSymbolPager() {
        var pager = document.getElementById('bt-opt-symbol-pager');
        var label = document.getElementById('bt-opt-symbol-label');
        var prev = document.getElementById('bt-opt-symbol-prev');
        var next = document.getElementById('bt-opt-symbol-next');
        if (!pager || !lastOptMultiResult || lastOptMultiResult.mode !== 'multi_optimize') {
            if (pager) pager.classList.remove('active');
            return;
        }
        var results = lastOptMultiResult.results || [];
        pager.classList.toggle('active', results.length > 1);
        if (!results.length) return;
        var cur = results[optMultiSymbolIndex] || results[0];
        if (label) {
            label.textContent = (optMultiSymbolIndex + 1) + ' / ' + results.length + ' · ' +
                (cur.instrument_id || '—');
        }
        if (prev) prev.disabled = optMultiSymbolIndex <= 0;
        if (next) next.disabled = optMultiSymbolIndex >= results.length - 1;
    }

    function showOptSymbolAt(index) {
        if (!lastOptMultiResult || lastOptMultiResult.mode !== 'multi_optimize') return;
        var results = lastOptMultiResult.results || [];
        if (!results.length) return;
        optMultiSymbolIndex = Math.max(0, Math.min(results.length - 1, index));
        var cur = results[optMultiSymbolIndex];
        renderOptRankingTable(
            cur.ranking || [],
            document.getElementById('bt-opt-summary'),
            document.getElementById('bt-opt-body'),
            cur
        );
        updateOptSymbolPager();
    }

    function bindOptSymbolPager() {
        var prev = document.getElementById('bt-opt-symbol-prev');
        var next = document.getElementById('bt-opt-symbol-next');
        if (prev) {
            prev.addEventListener('click', function () { showOptSymbolAt(optMultiSymbolIndex - 1); });
        }
        if (next) {
            next.addEventListener('click', function () { showOptSymbolAt(optMultiSymbolIndex + 1); });
        }
    }

    function renderOptimize(result) {
        if (!result) return;
        if (result.mode === 'multi_optimize') {
            lastOptMultiResult = result;
            optMultiSymbolIndex = 0;
            renderOptBestSummary(result);
            showOptSymbolAt(0);
            return;
        }
        lastOptMultiResult = null;
        updateOptSymbolPager();
        renderOptRankingTable(
            result.ranking || [],
            document.getElementById('bt-opt-summary'),
            document.getElementById('bt-opt-body'),
            result
        );
        renderOptBestSummary(result);
    }

    function renderSummary(summary) {
        var grid = document.getElementById('bt-perf-grid');
        if (!grid || !summary) return;
        var items = [
            ['总收益率', fmtPct(summary.total_return)],
            ['最终权益', fmtMoney(summary.final_equity)],
            ['总盈亏', fmtMoney(summary.total_pnl)],
            ['总手续费', fmtMoney(summary.total_fee)],
            ['交易次数', summary.trade_count],
            ['胜率', fmtPct(summary.win_rate)],
            ['最大回撤', fmtPct(summary.max_drawdown)],
            ['K线数', summary.bars_processed]
        ];
        if (summary.ticks_processed) {
            items.push(['Tick 数', summary.ticks_processed]);
        }
        if (summary.slippage_ticks != null) {
            items.push(['滑点(tick)', summary.slippage_ticks]);
        }
        if (summary.margin_rate != null) {
            items.push(['保证金比例', (Number(summary.margin_rate) * 100).toFixed(1) + '%']);
        }
        if (summary.peak_margin != null) {
            items.push(['峰值保证金', fmtMoney(summary.peak_margin)]);
        }
        if (summary.total_slippage_cost != null) {
            items.push(['滑点损耗', fmtMoney(summary.total_slippage_cost)]);
        }
        if (summary.margin_rejects) {
            items.push(['保证金拒单', summary.margin_rejects]);
        }
        grid.innerHTML = items.map(function (row) {
            return '<div class="metric-card"><div class="label">' + row[0] +
                '</div><div class="value">' + row[1] + '</div></div>';
        }).join('');
    }

    function renderEquityChart(result) {
        var host = document.getElementById('bt-equity-chart');
        if (!host) return;
        var chart = getChart(host);
        if (!chart) return;
        var curve = downsampleCurve(pickEquityCurve(result), 800);
        chart.setOption({
            backgroundColor: '#1a1a1a',
            grid: { left: 56, right: 20, top: 24, bottom: 36 },
            tooltip: {
                trigger: 'axis',
                formatter: function (params) {
                    var p = params && params[0];
                    if (!p) return '';
                    return 'Bar ' + p.name + '<br/>权益 ' + fmtMoney(p.value);
                }
            },
            xAxis: {
                type: 'category',
                data: curve.map(equityXLabel),
                axisLabel: { color: '#888', interval: 'auto' }
            },
            yAxis: { type: 'value', scale: true, axisLabel: { color: '#888' } },
            series: [{
                type: 'line',
                data: curve.map(function (p) { return p.equity; }),
                showSymbol: false,
                lineStyle: { color: '#3498db', width: 1.5 }
            }]
        }, true);
        scheduleChartResize(host);
    }

    function renderSignals(result) {
        var tbody = document.getElementById('bt-signals-body');
        if (!tbody) return;
        var signals = (result && result.signals) || [];
        if (!signals.length) {
            tbody.innerHTML = '<tr><td colspan="5">无信号</td></tr>';
            return;
        }
        tbody.innerHTML = signals.map(function (s) {
            var inst = s.instrument_id || (result && result.instrument_id) || '—';
            return '<tr><td>' + inst + '</td><td>' + signalTime(result, s) + '</td><td>' + actionZh(s.action) +
                '</td><td>' + (s.label || '—') + '</td><td>' + s.price + '</td></tr>';
        }).join('');
    }

    function renderDataDetail(result) {
        var tbody = document.getElementById('bt-data-body');
        var files = document.getElementById('bt-files-body');
        if (!tbody) return;
        var summary = result.summary || {};
        var spec = result.contract_spec || {};
        var rows = [
            ['标的', result.instrument_id],
            ['模式', result.mode || 'bar'],
            ['撮合', result.match_style || (result.mode === 'tick' ? 'intra_bar' : 'bar_close')],
            ['周期', result.period],
            ['策略', result.strategy],
            ['合约乘数', spec.multiplier != null ? spec.multiplier : summary.contract_multiplier],
            ['最小变动', spec.tick != null ? spec.tick : summary.tick_size],
            ['滑点(tick)', spec.slippage_ticks != null ? spec.slippage_ticks : summary.slippage_ticks],
            ['手续费率', spec.fee_rate != null ? spec.fee_rate : summary.fee_rate],
            ['固定手续费', spec.fee_per_lot != null ? spec.fee_per_lot : summary.fee_per_lot],
            ['保证金比例', spec.margin_rate != null ? spec.margin_rate : summary.margin_rate],
            ['峰值保证金', summary.peak_margin != null ? fmtMoney(summary.peak_margin) : '—'],
            ['K线数', summary.bars_processed],
            ['Tick 数', summary.ticks_processed || '—'],
            ['信号数', (result.signals || []).length],
            ['成交数', summary.trade_count]
        ];
        var ds = result.data_source || {};
        if (ds.start_date || ds.end_date) {
            rows.push(['回测区间', (ds.start_date || '…') + ' ~ ' + (ds.end_date || '…')]);
        }
        if (ds.storage_path) {
            rows.push(['Storage 路径', ds.storage_path]);
            rows.push(['回测载入', (ds.backtest_bar_count != null ? ds.backtest_bar_count : '—') + ' / 文件 ' + (ds.file_bar_count != null ? ds.file_bar_count : '—')]);
            rows.push(['末Bar对齐', ds.aligned === true ? '是' : (ds.aligned === false ? '否' : '—')]);
        }
        tbody.innerHTML = rows.map(function (row) {
            return '<tr><td>' + row[0] + '</td><td>' + row[1] + '</td></tr>';
        }).join('');
        if (files) {
            files.innerHTML = '<tr><td>closes</td><td>平仓明细</td><td>MatchEngine</td></tr>' +
                '<tr><td>equity_curve</td><td>权益曲线</td><td>MatchEngine</td></tr>';
        }
    }

    function renderMultiDetail(result) {
        var tbody = document.getElementById('bt-data-body');
        if (!tbody) return;
        var rows = [['多策略模式', 'Bar 并行对比']];
        (result.ranking || []).forEach(function (r) {
            rows.push([r.strategy, fmtPct(r.total_return) + ' / ' + r.trade_count + ' 笔']);
        });
        tbody.innerHTML = rows.map(function (row) {
            return '<tr><td>' + row[0] + '</td><td>' + row[1] + '</td></tr>';
        }).join('');
    }

    function computeTradeStats(closes) {
        var longRows = closes.filter(function (t) { return t.side === 'long'; });
        var shortRows = closes.filter(function (t) { return t.side === 'short'; });
        return {
            trade_count: closes.length,
            long_count: longRows.length,
            short_count: shortRows.length,
            win_rate: winRate(closes),
            long_win_rate: winRate(longRows),
            short_win_rate: winRate(shortRows),
            total_fee: sumField(closes, 'fee'),
            total_slippage_cost: sumField(closes, 'slippage_cost')
        };
    }

    function renderTradeStatsRow(symbol, stats, isSummary) {
        var cls = isSummary ? ' class="trade-stats-summary"' : '';
        return '<tr' + cls + '><td>' + symbol + '</td><td>' + stats.trade_count +
            '</td><td>' + stats.long_count + '</td><td>' + stats.short_count +
            '</td><td>' + fmtPct(stats.win_rate) + '</td><td>' + fmtPct(stats.long_win_rate) +
            '</td><td>' + fmtPct(stats.short_win_rate) + '</td><td>' + fmtMoney(stats.total_fee) +
            '</td><td>' + fmtMoney(stats.total_slippage_cost) + '</td></tr>';
    }

    function renderTrades(result) {
        var statsBody = document.getElementById('bt-trade-stats-body');
        var closesBody = document.getElementById('bt-closes-body');
        var closes = result.closes || result.all_closes || [];
        var rows = [];

        if (result.results && result.results.length) {
            result.results.forEach(function (r) {
                var symCloses = r.closes || [];
                rows.push(renderTradeStatsRow(r.instrument_id || '—', computeTradeStats(symCloses), false));
            });
            rows.push(renderTradeStatsRow('汇总', computeTradeStats(closes), true));
        } else {
            var summary = result.summary || {};
            var stats = computeTradeStats(closes);
            stats.trade_count = summary.trade_count != null ? summary.trade_count : stats.trade_count;
            stats.long_count = summary.long_count != null ? summary.long_count : stats.long_count;
            stats.short_count = summary.short_count != null ? summary.short_count : stats.short_count;
            stats.win_rate = summary.win_rate != null ? summary.win_rate : stats.win_rate;
            stats.total_fee = summary.total_fee != null ? summary.total_fee : stats.total_fee;
            stats.total_slippage_cost = summary.total_slippage_cost != null ? summary.total_slippage_cost : stats.total_slippage_cost;
            rows.push(renderTradeStatsRow(result.instrument_id || '—', stats, false));
        }

        if (statsBody) {
            statsBody.innerHTML = rows.length
                ? rows.join('')
                : '<tr><td colspan="9">无交易统计</td></tr>';
        }
        if (!closesBody) return;
        if (!closes.length) {
            closesBody.innerHTML = '<tr><td colspan="10">无平仓记录</td></tr>';
            return;
        }
        closesBody.innerHTML = closes.map(function (t) {
            var cls = t.pnl >= 0 ? 'pos' : 'neg';
            var openTime = t.open_time || barTimeAt(result, t.open_bar);
            var closeTime = t.close_time || barTimeAt(result, t.close_bar);
            var inst = t.instrument_id || result.instrument_id || '—';
            return '<tr><td>' + inst + '</td><td>' + sideZh(t.side) + '</td><td>' + openTime + '</td><td>' + closeTime +
                '</td><td>' + t.bars_held + '</td><td>' + t.open_price + '</td><td>' + t.close_price +
                '</td><td class="' + cls + '">' + fmtMoney(t.pnl) + '</td><td>' + fmtMoney(t.fee || 0) +
                '</td><td>' + fmtMoney(t.slippage_cost || 0) + '</td></tr>';
        }).join('');
    }

    function renderCompareDetail(result) {
        var tbody = document.getElementById('bt-data-body');
        if (!tbody) return;
        var cmp = result.comparison || {};
        tbody.innerHTML = [
            ['对比模式', 'Bar CSV vs Tick intra-bar'],
            ['Bar 最终权益', fmtMoney(cmp.bar_final_equity)],
            ['Tick 最终权益', fmtMoney(cmp.tick_final_equity)],
            ['收益差', fmtPct(cmp.return_diff)],
            ['交易次数差', cmp.trade_count_diff]
        ].map(function (row) {
            return '<tr><td>' + row[0] + '</td><td>' + row[1] + '</td></tr>';
        }).join('');
    }

    function bindTabs() {
        document.querySelectorAll('.view-tab-btn').forEach(function (btn) {
            btn.addEventListener('click', function () {
                document.querySelectorAll('.view-tab-btn').forEach(function (b) { b.classList.remove('active'); });
                document.querySelectorAll('.view-panel').forEach(function (p) {
                    p.classList.remove('active');
                    p.style.display = 'none';
                });
                btn.classList.add('active');
                var panel = document.getElementById('bt-view-' + btn.dataset.view);
                if (panel) {
                    panel.classList.add('active');
                    panel.style.display = 'flex';
                    resizeChartsInPanel(panel);
                }
            });
        });
    }

    function resolveInstrumentId() {
        var ids = resolveInstrumentIds();
        return ids.length ? ids[0] : 'rb2610';
    }

    var lastMultiSymbolResult = null;
    var multiSymbolIndex = 0;


    function updateSymbolPager() {
        var pager = document.getElementById('bt-symbol-pager');
        var label = document.getElementById('bt-symbol-label');
        var prev = document.getElementById('bt-symbol-prev');
        var next = document.getElementById('bt-symbol-next');
        var results = (lastMultiSymbolResult && lastMultiSymbolResult.results) || [];
        if (!pager) return;
        if (results.length <= 1) {
            pager.classList.remove('active');
            return;
        }
        pager.classList.add('active');
        var one = results[multiSymbolIndex] || {};
        var sym = one.instrument_id || ((lastMultiSymbolResult.instrument_ids || [])[multiSymbolIndex]) || '—';
        if (label) {
            label.textContent = sym + ' (' + (multiSymbolIndex + 1) + '/' + results.length + ')';
        }
        if (prev) prev.disabled = multiSymbolIndex <= 0;
        if (next) next.disabled = multiSymbolIndex >= results.length - 1;
    }

    function showMultiSymbolAt(index) {
        if (!lastMultiSymbolResult || !lastMultiSymbolResult.results) return;
        var results = lastMultiSymbolResult.results;
        if (!results.length) return;
        multiSymbolIndex = Math.max(0, Math.min(index, results.length - 1));
        var one = results[multiSymbolIndex];
        if (!one || one.error) {
            updateSymbolPager();
            return;
        }
        renderSignals(one);
        renderSignalChart(one);
        updateSymbolPager();
        scheduleChartResize('bt-bar-chart');
    }

    function bindSymbolPager() {
        var prev = document.getElementById('bt-symbol-prev');
        var next = document.getElementById('bt-symbol-next');
        if (prev) {
            prev.addEventListener('click', function () {
                showMultiSymbolAt(multiSymbolIndex - 1);
            });
        }
        if (next) {
            next.addEventListener('click', function () {
                showMultiSymbolAt(multiSymbolIndex + 1);
            });
        }
    }


    function renderMultiSymbolView(result) {
        lastMultiSymbolResult = result;
        multiSymbolIndex = 0;
        renderMultiRanking(result);
        renderMultiSymbolDetail(result);
        renderTrades(result);
        var topId = result.ranking && result.ranking[0] && result.ranking[0].instrument_id;
        var topIdx = (result.results || []).findIndex(function (r) { return r.instrument_id === topId; });
        showMultiSymbolAt(topIdx >= 0 ? topIdx : 0);
        var best = (result.results || [])[multiSymbolIndex] || {};
        renderEquityChart(best);
    }

    function resolveInstrumentIds() {
        var raw = (val('bt-symbol') || 'rb2610').trim();
        if (!raw) return ['rb2610'];
        var parts = raw.split(/[,，;]+/);
        var out = [];
        parts.forEach(function (part) {
            part.split(/\s+/).forEach(function (s) {
                s = s.trim();
                if (!s) return;
                if (s.indexOf('.') >= 0) s = s.split('.').pop();
                out.push(s.toLowerCase());
            });
        });
        return out.length ? out : ['rb2610'];
    }

    function showProgress(show) {
        var wrap = document.getElementById('bt-progress-wrap');
        if (wrap) wrap.classList.toggle('active', !!show);
    }

    function updateProgressUI(prog) {
        var fill = document.getElementById('bt-progress-fill');
        var text = document.getElementById('bt-progress-text');
        if (!prog) return;
        var pct = Math.max(0, Math.min(100, Number(prog.percent || 0)));
        if (fill) fill.style.width = pct + '%';
        if (text) {
            var msg = prog.message || '处理中…';
            if (prog.total > 0) {
                msg += ' (' + prog.current + '/' + prog.total + ' · ' + pct + '%)';
            }
            if (prog.instrument_id) msg = '[' + prog.instrument_id + '] ' + msg;
            text.textContent = msg;
        }
    }

    function pollProgress(intervalMs) {
        var t = transport();
        if (!t) return Promise.resolve();
        return t.get('/api/backtest/progress').then(function (prog) {
            updateProgressUI(prog);
            return prog;
        }).catch(function () { return { running: false }; });
    }

    function waitForBacktest(onProgress) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var delay = 400;
        function step() {
            return pollProgress().then(function (prog) {
                if (onProgress) onProgress(prog);
                if (prog && prog.running) {
                    return new Promise(function (resolve) {
                        setTimeout(function () { resolve(step()); }, delay);
                    });
                }
                return t.get('/api/backtest/result');
            });
        }
        return step();
    }

    function startAsyncJob(url, payload) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        showProgress(true);
        updateProgressUI({ percent: 0, message: '提交任务…' });
        return t.post(url, payload).then(function (ack) {
            if (!ack || !ack.started) return ack;
            return waitForBacktest(updateProgressUI);
        }).finally(function () {
            showProgress(false);
        });
    }

    function periodToApi(uiPeriod) {
        var map = {
            '1分钟': 'm1',
            '5分钟': 'm5',
            '15分钟': 'm15',
            '1小时': 'h1',
            '日线': 'd1'
        };
        return map[uiPeriod] || uiPeriod || 'm15';
    }

    function barTimeKey(bar) {
        return String(bar.date || '') + ' ' + String(bar.time || '');
    }

    function formatMonthDay(text) {
        var m = String(text || '').match(/(\d{4})[\/\-](\d{1,2})[\/\-](\d{1,2})/);
        if (!m) return text;
        var mm = m[2].length === 1 ? '0' + m[2] : m[2];
        var dd = m[3].length === 1 ? '0' + m[3] : m[3];
        return mm + '-' + dd;
    }

    function axisLabelStep(count, target) {
        return Math.max(1, Math.ceil((count || 1) / (target || 20)));
    }

    function barRangeOffset(bar) {
        var span = Number(bar.high) - Number(bar.low);
        if (span > 0) return span * 0.35;
        return Math.max(Number(bar.close) * 0.002, 1);
    }

    function buildSignalMarkers(signals, bars) {
        var buyMarks = [];
        var sellMarks = [];
        (signals || []).forEach(function (s) {
            var idx = s.bar;
            if (idx == null || idx < 0 || idx >= bars.length) return;
            var bar = bars[idx];
            var offset = barRangeOffset(bar);
            if (s.action === 'buy') {
                buyMarks.push({
                    name: s.label || '买入',
                    xAxis: idx,
                    yAxis: Number(bar.low) - offset,
                    symbol: 'arrow',
                    symbolSize: 16,
                    symbolRotate: 0,
                    itemStyle: { color: '#e74c3c' }
                });
            } else if (s.action === 'sell') {
                sellMarks.push({
                    name: s.label || '卖出',
                    xAxis: idx,
                    yAxis: Number(bar.high) + offset,
                    symbol: 'arrow',
                    symbolSize: 16,
                    symbolRotate: 180,
                    itemStyle: { color: '#2ecc71' }
                });
            }
        });
        return buyMarks.concat(sellMarks);
    }

    function buildTradeLinkLines(closes) {
        return (closes || []).map(function (t) {
            var openIdx = t.open_bar;
            var closeIdx = t.close_bar;
            if (openIdx == null || closeIdx == null) return null;
            var color = t.side === 'long' ? 'rgba(231,76,60,0.55)' : 'rgba(46,204,113,0.55)';
            return [
                { xAxis: openIdx, yAxis: t.open_price, symbol: 'none' },
                { xAxis: closeIdx, yAxis: t.close_price, symbol: 'none', lineStyle: { color: color } }
            ];
        }).filter(Boolean);
    }

    function indicatorSeries(chartIndicators) {
        var ind = chartIndicators || {};
        var out = [];
        if (ind.dual_upper && ind.dual_upper.length) {
            out.push({
                name: '上轨',
                type: 'line',
                data: ind.dual_upper,
                showSymbol: false,
                lineStyle: { color: '#e67e22', width: 1.2, opacity: 0.9 },
                z: 3
            });
        }
        if (ind.dual_lower && ind.dual_lower.length) {
            out.push({
                name: '下轨',
                type: 'line',
                data: ind.dual_lower,
                showSymbol: false,
                lineStyle: { color: '#1abc9c', width: 1.2, opacity: 0.9 },
                z: 3
            });
        }
        if (ind.ma_fast && ind.ma_fast.length) {
            out.push({
                name: 'MA快',
                type: 'line',
                data: ind.ma_fast,
                showSymbol: false,
                lineStyle: { color: '#f1c40f', width: 1 },
                z: 3
            });
        }
        if (ind.ma_slow && ind.ma_slow.length) {
            out.push({
                name: 'MA慢',
                type: 'line',
                data: ind.ma_slow,
                showSymbol: false,
                lineStyle: { color: '#9b59b6', width: 1 },
                z: 3
            });
        }
        return out;
    }

    function renderSignalChart(result) {
        var host = document.getElementById('bt-bar-chart');
        if (!host) return;
        var chart = getChart(host);
        if (!chart) return;
        var bars = (result && result.bars) || [];
        if (!bars.length) {
            chart.clear();
            return;
        }
        var categories = bars.map(function (b) { return barTimeKey(b); });
        var ohlc = bars.map(function (b) { return [b.open, b.close, b.low, b.high]; });
        var labelStep = axisLabelStep(categories.length, 20);
        var signalMarks = buildSignalMarkers(result.signals, bars);
        var tradeLines = buildTradeLinkLines(result.closes);
        var indSeries = indicatorSeries(result.chart_indicators);
        var legendNames = ['回测K线'].concat(indSeries.map(function (s) { return s.name; }));
        if (signalMarks.length) legendNames.push('买卖点');
        if (tradeLines.length) legendNames.push('持仓连线');

        chart.setOption({
            backgroundColor: '#1a1a1a',
            legend: { data: legendNames, textStyle: { color: '#888' }, top: 0, type: 'scroll' },
            grid: { left: 52, right: 18, top: 42, bottom: 72 },
            tooltip: { trigger: 'axis', axisPointer: { type: 'cross' } },
            xAxis: {
                type: 'category',
                data: categories,
                boundaryGap: true,
                axisLabel: {
                    color: '#888',
                    fontSize: 10,
                    interval: 0,
                    formatter: function (value, idx) {
                        if (idx % labelStep !== 0) return '';
                        return formatMonthDay(value);
                    }
                }
            },
            yAxis: { scale: true, axisLabel: { color: '#888' } },
            dataZoom: [
                { type: 'inside', xAxisIndex: 0 },
                { type: 'slider', xAxisIndex: 0, height: 22, bottom: 6, filterMode: 'filter' }
            ],
            series: [{
                name: '回测K线',
                type: 'candlestick',
                data: ohlc,
                itemStyle: {
                    color: '#e74c3c',
                    color0: '#2ecc71',
                    borderColor: '#e74c3c',
                    borderColor0: '#2ecc71'
                },
                markPoint: signalMarks.length ? {
                    data: signalMarks,
                    z: 20,
                    label: { show: false }
                } : undefined,
                markLine: tradeLines.length ? {
                    symbol: ['none', 'none'],
                    silent: true,
                    lineStyle: { type: 'dashed', width: 1, color: 'rgba(160,160,160,0.65)' },
                    data: tradeLines,
                    z: 15
                } : undefined,
                z: 5
            }].concat(indSeries)
        }, true);
        scheduleChartResize(host);
    }

    function renderVerifyBarChart(result, storageBars) {
        var host = document.getElementById('bt-verify-chart');
        if (!host) return;
        var chart = getChart(host);
        if (!chart) return;
        var bars = (result && result.bars) || [];
        var tail = bars.slice(-80);
        if (!tail.length) {
            chart.clear();
            return;
        }
        var startIdx = bars.length - tail.length;
        var categories = tail.map(function (b) { return barTimeKey(b); });
        var ohlc = tail.map(function (b) { return [b.open, b.close, b.low, b.high]; });
        var storageMap = {};
        (storageBars || []).forEach(function (b) {
            storageMap[barTimeKey(b)] = b.close;
        });
        var storageLine = tail.map(function (b) {
            var key = barTimeKey(b);
            return storageMap[key] != null ? storageMap[key] : null;
        });
        var signals = (result && result.signals) || [];
        var signalMarks = buildSignalMarkers(
            signals.filter(function (s) {
                return s.bar != null && s.bar >= startIdx && s.bar < bars.length;
            }).map(function (s) { return Object.assign({}, s, { bar: s.bar - startIdx }); }),
            tail
        );
        var tradeLines = buildTradeLinkLines((result.closes || []).filter(function (t) {
            return t.close_bar >= startIdx && t.open_bar >= startIdx;
        }).map(function (t) {
            return {
                side: t.side,
                open_bar: t.open_bar - startIdx,
                close_bar: t.close_bar - startIdx,
                open_price: t.open_price,
                close_price: t.close_price
            };
        }));
        var labelStep = axisLabelStep(categories.length, 20);
        chart.setOption({
            backgroundColor: '#1a1a1a',
            legend: { data: ['回测K线', 'Storage收盘'], textStyle: { color: '#888' }, top: 0 },
            grid: { left: 50, right: 20, top: 36, bottom: 60 },
            tooltip: { trigger: 'axis' },
            xAxis: {
                type: 'category',
                data: categories,
                axisLabel: {
                    color: '#888',
                    fontSize: 9,
                    interval: 0,
                    formatter: function (value, idx) {
                        if (idx % labelStep !== 0) return '';
                        return formatMonthDay(value);
                    }
                }
            },
            yAxis: { scale: true, axisLabel: { color: '#888' } },
            dataZoom: [{ type: 'inside' }, { type: 'slider', height: 18, bottom: 4 }],
            series: [
                {
                    name: '回测K线',
                    type: 'candlestick',
                    data: ohlc,
                    itemStyle: { color: '#e74c3c', color0: '#2ecc71', borderColor: '#e74c3c', borderColor0: '#2ecc71' },
                    markPoint: signalMarks.length ? { data: signalMarks, label: { show: false } } : undefined,
                    markLine: tradeLines.length ? {
                        symbol: ['none', 'none'],
                        silent: true,
                        lineStyle: { type: 'dashed', width: 1, color: 'rgba(160,160,160,0.65)' },
                        data: tradeLines
                    } : undefined
                },
                {
                    name: 'Storage收盘',
                    type: 'line',
                    data: storageLine,
                    showSymbol: false,
                    lineStyle: { color: '#f39c12', width: 1, type: 'dashed' }
                }
            ]
        }, true);
        scheduleChartResize(host);
    }

    function renderVerifyTable(btBars, storageBars) {
        var tbody = document.getElementById('bt-verify-body');
        if (!tbody) return;
        var storageMap = {};
        (storageBars || []).forEach(function (b) {
            storageMap[barTimeKey(b)] = b;
        });
        var rows = (btBars || []).slice(-20);
        if (!rows.length) {
            tbody.innerHTML = '<tr><td colspan="5">无回测 Bar</td></tr>';
            return;
        }
        var matchCount = 0;
        tbody.innerHTML = rows.map(function (b, i) {
            var key = barTimeKey(b);
            var st = storageMap[key];
            var ok = st && st.close === b.close;
            if (ok) matchCount += 1;
            return '<tr><td>' + i + '</td><td>' + key + '</td><td>' + b.close +
                '</td><td>' + (st ? st.close : '—') + '</td><td class="' + (ok ? 'pos' : 'neg') +
                '">' + (ok ? 'OK' : '差异') + '</td></tr>';
        }).join('');
        var summary = document.getElementById('bt-verify-summary');
        if (summary && rows.length) {
            var extra = ' · 末20根匹配 ' + matchCount + '/' + rows.length;
            if (summary.innerHTML.indexOf('末20根匹配') < 0) summary.innerHTML += extra;
        }
    }

    function renderVerify(result) {
        if (!result || result.mode === 'compare' || result.mode === 'multi') {
            return Promise.resolve();
        }
        var ds = result.data_source || {};
        var summary = document.getElementById('bt-verify-summary');
        if (summary) {
            summary.innerHTML = '数据源: ' + (ds.storage_path || '—') + '<br>周期 ' +
                (ds.requested_period || result.period || '—') + ' → ' + (ds.resolved_period || result.period || '—') +
                (ds.fallback_m15 ? ' <span class="neg">(回退 m15)</span>' : '') +
                ' · Storage ' + (ds.file_bar_count != null ? ds.file_bar_count : '—') + ' 条 / 回测 ' +
                (ds.backtest_bar_count != null ? ds.backtest_bar_count : (result.summary && result.summary.bars_processed) || '—') + ' 条' +
                (ds.aligned === true ? ' <span class="pos">末Bar对齐</span>' :
                    (ds.aligned === false ? ' <span class="neg">末Bar不一致</span>' : ''));
        }
        var inst = result.instrument_id || resolveInstrumentId();
        var period = ds.resolved_period || result.period || periodToApi(val('bt-period'));
        var btBars = result.bars || [];
        var t = transport();
        if (!t || !inst) return Promise.resolve();
        return t.get('/api/bars?instrument_id=' + encodeURIComponent(inst) + '&period=' +
            encodeURIComponent(period) + '&limit=80').then(function (apiRes) {
            var storageBars = (apiRes && apiRes.bars) || [];
            renderVerifyTable(btBars.slice(-20), storageBars);
            renderVerifyBarChart(result, storageBars);
        }).catch(function () {
            renderVerifyTable(btBars.slice(-20), []);
            renderVerifyBarChart(result, []);
        });
    }

    function verifyStorageOnly() {
        var inst = resolveInstrumentId();
        var period = periodToApi(val('bt-period') || '15分钟');
        if (!global.QuantSevBridge.Diagnostic) {
            alert('Diagnostic Bridge 未加载');
            return Promise.resolve();
        }
        return QuantSevBridge.Diagnostic.verifyBacktestBars(inst, period).then(function (res) {
            var summary = document.getElementById('bt-verify-summary');
            if (summary) {
                var cls = res.aligned ? 'pos' : 'neg';
                summary.innerHTML = '<span class="' + cls + '">' + (res.aligned ? 'Storage/回测路径对齐 OK' : '未对齐') +
                    '</span><br>路径: ' + (res.storage_path || '—') + '<br>Storage ' + (res.file_bar_count || 0) +
                    ' 条 / 回测载入 ' + (res.backtest_bar_count || 0) + ' 条';
            }
            document.querySelectorAll('.view-tab-btn').forEach(function (b) { b.classList.remove('active'); });
            document.querySelectorAll('.view-panel').forEach(function (p) {
                p.classList.remove('active');
                p.style.display = 'none';
            });
            var tab = document.querySelector('.view-tab-btn[data-view="verify"]');
            var panel = document.getElementById('bt-view-verify');
            if (tab) tab.classList.add('active');
            if (panel) {
                panel.classList.add('active');
                panel.style.display = 'flex';
                resizeChartsInPanel(panel);
            }
        }).catch(function (err) {
            alert('核对失败: ' + err.message);
        });
    }

    function openInTradePage() {
        var inst = resolveInstrumentId();
        try {
            localStorage.setItem('quant_sev_chart_instrument', inst);
            localStorage.setItem('quant_sev_last_contract', inst);
        } catch (e) { /* ignore */ }
        try {
            if (window.parent && window.parent !== window) {
                var nav = window.parent.document.querySelector('[data-page="Trade_ui.html"]');
                if (nav && typeof window.parent.navigateTo === 'function') {
                    window.parent.navigateTo(nav);
                    setTimeout(function () {
                        window.parent.dispatchEvent(new CustomEvent('quant-sev-chart-instrument', { detail: inst }));
                    }, 400);
                    return;
                }
            }
        } catch (e) { /* ignore */ }
        alert('合约 ' + inst + ' 已写入 localStorage，请打开「交易界面」页查看 K 线/DOM');
    }

    function loadLastResult() {
        var t = transport();
        if (!t) return Promise.resolve();
        return t.get('/api/backtest/result').then(function (result) {
            if (!result || !result.ok) return;
            if (btPageMode === 'optimize') {
                if (result.mode === 'optimize' || result.mode === 'multi_optimize') {
                    renderOptimize(result);
                }
                return;
            }
            if (result.mode === 'optimize' || result.mode === 'multi_optimize') return;
            renderResult(result);
        }).catch(function () { /* ignore */ });
    }

    function collectStrategyParamsFromUi() {
        var params = {};
        document.querySelectorAll('[data-strategy-param]').forEach(function (el) {
            var key = el.getAttribute('data-strategy-param');
            if (!key) return;
            var raw = el.value;
            if (raw === '' || raw == null) return;
            var n = Number(raw);
            if (!isNaN(n)) {
                params[key] = n;
            }
        });
        return params;
    }

    function renderRunStrategyInputs(inputs) {
        var wrap = document.getElementById('bt-strategy-inputs');
        if (!wrap) return;
        if (!inputs || !inputs.length) {
            wrap.innerHTML = '<p class="hint">该策略无 input 参数</p>';
            return;
        }
        wrap.innerHTML = '<div class="strategy-inputs-grid">' + inputs.map(function (inp) {
            var isInt = inp.type === 'int';
            var step = isInt ? 1 : 0.1;
            var label = (inp.var_name || inp.key) + (inp.label ? ' · ' + inp.label : '');
            return '<div class="form-group" data-input-key="' + inp.key + '">' +
                '<label class="form-label">' + label + '</label>' +
                '<input type="number" class="form-input" data-strategy-param="' + inp.key + '" ' +
                'value="' + inp.default + '" min="' + inp.min + '" max="' + inp.max + '" step="' + step + '">' +
                '</div>';
        }).join('') + '</div>';
    }

    function applyStrategyInputs(data) {
        if (data && data.error) {
            lastStrategyInputs = [];
            renderOptParamsTable([]);
            renderRunStrategyInputs([]);
            return data;
        }
        lastStrategyInputs = (data && data.inputs) || [];
        renderOptParamsTable(lastStrategyInputs);
        renderRunStrategyInputs(lastStrategyInputs);
        return data;
    }

    function collectPayload() {
        var strategy = val('bt-strategy') || 'dual_thrust';
        var modeEl = document.getElementById('bt-mode');
        var mode = modeEl ? modeEl.value : 'bar';
        var ids = resolveInstrumentIds();
        if (ids.length > 1) {
            mode = 'multi_symbol';
        }
        var strategyParams = collectStrategyParamsFromUi();
        var payload = {
            instrument_id: ids.join(','),
            instrument_ids: ids,
            mode: mode,
            period: val('bt-period') || '15分钟',
            initial_capital: num('bt-capital', 1000000),
            start_date: val('bt-start-date') || '',
            end_date: val('bt-end-date') || '',
            tick_limit: num('bt-max-ticks', 200000),
            slippage_ticks: num('bt-slippage', 0),
            fee_rate: num('bt-fee-rate', 0.0001),
            fee_per_lot: num('bt-fee-lot', 0),
            margin_rate: num('bt-margin-rate', 0.1),
            strategy: strategy
        };
        Object.keys(strategyParams).forEach(function (k) {
            payload[k] = strategyParams[k];
        });
        (lastStrategyInputs || []).forEach(function (inp) {
            if (payload[inp.key] == null && inp.default != null) {
                payload[inp.key] = inp.default;
            }
        });
        var mult = num('bt-multiplier', 0);
        var tick = num('bt-tick', 0);
        if (mult > 0) payload.contract_multiplier = mult;
        if (tick > 0) payload.tick_size = tick;
        if (!payload.start_date && !payload.end_date) {
            payload.max_bars = 50000;
        }
        return payload;
    }

    function renderOptParamsTable(inputs) {
        var tbody = document.getElementById('bt-opt-params-body');
        if (!tbody) return;
        if (!inputs || !inputs.length) {
            tbody.innerHTML = '<tr><td colspan="5">该策略无可优化 input</td></tr>';
            return;
        }
        tbody.innerHTML = inputs.map(function (inp) {
            var opt = inp.optimize || {};
            var isInt = inp.type === 'int';
            var step = isInt ? 1 : 0.1;
            var label = (inp.var_name || inp.key) + (inp.label ? ' · ' + inp.label : '');
            return '<tr data-opt-key="' + inp.key + '">' +
                '<td>' + label + '</td>' +
                '<td><input type="number" data-role="start" value="' + opt.start + '" min="' + inp.min +
                '" max="' + inp.max + '" step="' + step + '"></td>' +
                '<td><input type="number" data-role="step" value="' + opt.step + '" step="' + step + '"></td>' +
                '<td><input type="number" data-role="stop" value="' + opt.stop + '" min="' + inp.min +
                '" max="' + inp.max + '" step="' + step + '"></td>' +
                '<td><input type="checkbox" data-role="enabled" checked></td>' +
                '</tr>';
        }).join('');
    }

    function loadBacktestStrategies() {
        var t = transport();
        if (!t) return Promise.resolve([]);
        return t.get('/api/backtest/strategies').then(function (data) {
            var logics = (data && data.logics) || [];
            function optionHtml(l, idx) {
                return '<option value="' + l.id + '"' + (idx === 0 ? ' selected' : '') + '>' +
                    (l.name || l.id) + '</option>';
            }
            var runSel = document.getElementById('bt-strategy');
            if (runSel) {
                runSel.innerHTML = logics.length
                    ? logics.map(optionHtml).join('')
                    : '<option value="">无可用策略</option>';
            }
            var optSel = document.getElementById('bt-opt-strategy');
            if (optSel) {
                optSel.innerHTML = logics.length
                    ? logics.map(optionHtml).join('')
                    : '<option value="">无可用策略</option>';
            }
            return logics;
        }).catch(function () {
            return [];
        });
    }

    function loadStrategyInputs(strategyId) {
        var t = transport();
        if (!t) return Promise.resolve(null);
        if (!strategyId) {
            return Promise.resolve(applyStrategyInputs({ inputs: [] }));
        }
        return t.get('/api/backtest/strategy_inputs?strategy=' + encodeURIComponent(strategyId)).then(function (data) {
            return applyStrategyInputs(data);
        }).catch(function () {
            return applyStrategyInputs(null);
        });
    }

    function syncRunStrategyUi() {
        var strategy = val('bt-strategy') || '';
        return loadStrategyInputs(strategy);
    }

    function buildOptimizeGrid() {
        var grid = {};
        document.querySelectorAll('#bt-opt-params-body tr[data-opt-key]').forEach(function (tr) {
            var key = tr.getAttribute('data-opt-key');
            var startEl = tr.querySelector('[data-role="start"]');
            var stepEl = tr.querySelector('[data-role="step"]');
            var stopEl = tr.querySelector('[data-role="stop"]');
            var enEl = tr.querySelector('[data-role="enabled"]');
            grid[key] = {
                start: startEl ? Number(startEl.value) : 0,
                step: stepEl ? Number(stepEl.value) : 1,
                stop: stopEl ? Number(stopEl.value) : 0,
                enabled: !enEl || enEl.checked
            };
        });
        return grid;
    }

    function collectOptimizePayload() {
        var payload = collectPayload();
        payload.mode = 'optimize';
        payload.optimize_metric = val('bt-opt-metric') || 'total_return';
        var optStrategy = val('bt-opt-strategy') || val('bt-strategy') || 'dual_thrust';
        payload.optimize_strategy = optStrategy;
        payload.strategy = optStrategy;
        payload.optimize_grid = buildOptimizeGrid();
        var ids = resolveInstrumentIds();
        payload.instrument_id = ids.join(',');
        payload.instrument_ids = ids;
        return payload;
    }

    function syncOptStrategyUi() {
        var strategy = val('bt-opt-strategy') || val('bt-strategy') || '';
        return loadStrategyInputs(strategy);
    }

    function syncModeUi() {
        var mode = document.getElementById('bt-mode');
        var tickGroup = document.getElementById('bt-tick-limit-group');
        if (mode && tickGroup) {
            tickGroup.style.display = (mode.value === 'tick' || mode.value === 'compare') ? 'block' : 'none';
        }
    }

    function loadContractSpec() {
        var t = transport();
        if (!t) return Promise.resolve();
        var instrument = resolveInstrumentId();
        return t.get('/api/backtest/contract_spec?instrument_id=' + encodeURIComponent(instrument)).then(function (spec) {
            var multEl = document.getElementById('bt-multiplier');
            var tickEl = document.getElementById('bt-tick');
            if (!spec || !spec.found) return spec;
            if (multEl && !multEl.dataset.userEdited) {
                multEl.placeholder = String(spec.multiplier);
                multEl.value = '';
            }
            if (tickEl && !tickEl.dataset.userEdited) {
                tickEl.placeholder = String(spec.tick);
                tickEl.value = '';
            }
            return spec;
        }).catch(function () { /* ignore */ });
    }

    function renderResult(result) {
        if (!result) return;
        if (result.error) {
            alert('回测失败: ' + result.error);
            return;
        }
        if (result.mode === 'compare') {
            renderComparison(result);
            renderEquityChart(result.tick || {});
            renderSignals(result.tick || {});
            renderCompareDetail(result);
            renderTrades(result.tick || {});
            return;
        }
        if (result.mode === 'multi') {
            renderMultiRanking(result);
            var best = (result.results && result.results[0]) || {};
            renderEquityChart(best);
            renderSignals(best);
            renderMultiDetail(result);
            renderTrades(best);
            return;
        }
        if (result.mode === 'multi_symbol') {
            renderMultiSymbolView(result);
            if (result.errors && result.errors.length && global.QuantSevBridge.Logger) {
                result.errors.forEach(function (e) {
                    QuantSevBridge.Logger.warn('合约 ' + e.instrument_id + ' 回测失败: ' + e.error);
                });
            }
            return;
        }
        if (result.mode === 'optimize' || result.mode === 'multi_optimize') {
            renderOptimize(result);
            return;
        }
        renderSummary(result.summary);
        renderEquityChart(result);
        lastMultiSymbolResult = null;
        updateSymbolPager();
        renderSignals(result);
        renderDataDetail(result);
        renderTrades(result);
        renderVerify(result);
        renderSignalChart(result);
        scheduleChartResize('bt-equity-chart');
    }

    function run() {
        var btn = document.getElementById('bt-run-btn');
        if (btn) {
            btn.disabled = true;
            btn.textContent = '回测中…';
        }
        return startAsyncJob('/api/backtest/run', collectPayload()).then(function (result) {
            if (result && result.error) {
                throw new Error(result.error);
            }
            renderResult(result || {});
            if (global.QuantSevBridge.Logger) {
                var msg;
                if (result.mode === 'compare') {
                    msg = '对比完成: Bar ' + ((result.comparison && result.comparison.bar_trade_count) || 0) +
                        ' / Tick ' + ((result.comparison && result.comparison.tick_trade_count) || 0) + ' 笔';
                } else if (result.mode === 'multi') {
                    msg = '多策略完成: ' + ((result.ranking && result.ranking.length) || 0) + ' 个策略';
                } else if (result.mode === 'multi_symbol') {
                    msg = '多合约完成: ' + ((result.instrument_ids && result.instrument_ids.length) || 0) + ' 个标的';
                } else {
                    var ds = result.data_source || {};
                    var range = (ds.start_date || ds.end_date)
                        ? (' 区间 ' + (ds.start_date || '…') + ' ~ ' + (ds.end_date || '…'))
                        : '';
                    msg = '回测完成: ' + ((result.summary && result.summary.trade_count) || 0) + ' 笔' +
                        range + '，K线 ' + ((result.summary && result.summary.bars_processed) || 0) + ' 根';
                }
                QuantSevBridge.Logger.log(msg);
            }
            return result;
        }).catch(function (err) {
            var msg = err.message || String(err);
            if (msg.indexOf('not found') >= 0 || msg.indexOf('404') >= 0) {
                msg += '\n\n请关闭旧进程并重启最新版 quant_sev_host.exe（需含 /api/backtest/run）';
            }
            if (msg.indexOf('already running') >= 0) {
                msg = '已有回测任务在运行，请稍候';
            }
            alert('回测失败: ' + msg);
        }).finally(function () {
            if (btn) {
                btn.disabled = false;
                btn.textContent = '▶️ 开始回测';
            }
        });
    }

    function runOptimize() {
        var btn = document.getElementById('bt-opt-run-btn');
        if (btn) {
            btn.disabled = true;
            btn.textContent = '优化中…';
        }
        document.querySelectorAll('.view-tab-btn').forEach(function (b) { b.classList.remove('active'); });
        document.querySelectorAll('.view-panel').forEach(function (p) {
            p.classList.remove('active');
            p.style.display = 'none';
        });
        var tab = document.querySelector('.view-tab-btn[data-view="opt-symbol"]') ||
            document.querySelector('.view-tab-btn[data-view="optimize"]');
        var panel = document.getElementById('bt-view-opt-symbol') || document.getElementById('bt-view-optimize');
        if (tab) tab.classList.add('active');
        if (panel) {
            panel.classList.add('active');
            panel.style.display = 'flex';
        }
        return startAsyncJob('/api/backtest/optimize', collectOptimizePayload()).then(function (result) {
            if (result && result.error) throw new Error(result.error);
            renderOptimize(result);
            if (global.QuantSevBridge.Logger) {
                var msg = result.mode === 'multi_optimize'
                    ? ('多合约优化完成: ' + ((result.instrument_ids && result.instrument_ids.length) || 0) + ' 个标的')
                    : ('参数优化完成: ' + (result.total_combos || 0) + ' 组，最优收益 ' +
                        fmtPct((result.best && result.best.total_return) || 0));
                QuantSevBridge.Logger.log(msg);
            }
            return result;
        }).catch(function (err) {
            alert('参数优化失败: ' + (err.message || err));
        }).finally(function () {
            if (btn) {
                btn.disabled = false;
                btn.textContent = '🚀 开始参数优化';
            }
        });
    }

    function initPage(page) {
        btPageMode = page || (document.body && document.body.getAttribute('data-bt-page')) || 'run';
        bindTabs();
        bindSymbolPager();
        bindOptSymbolPager();
        syncModeUi();
        loadContractSpec();
        loadLastResult();

        var symbol = document.getElementById('bt-symbol');
        if (symbol) {
            symbol.addEventListener('change', loadContractSpec);
            symbol.addEventListener('keydown', function (e) {
                if (e.key === 'Enter') loadContractSpec();
            });
        }
        ['bt-multiplier', 'bt-tick'].forEach(function (id) {
            var el = document.getElementById(id);
            if (el) {
                el.addEventListener('input', function () { el.dataset.userEdited = '1'; });
            }
        });

        if (btPageMode === 'run') {
            var mode = document.getElementById('bt-mode');
            if (mode) mode.addEventListener('change', syncModeUi);
            var strategy = document.getElementById('bt-strategy');
            if (strategy) strategy.addEventListener('change', syncRunStrategyUi);
            loadBacktestStrategies().then(function () {
                syncRunStrategyUi();
            });
            var btn = document.getElementById('bt-run-btn');
            if (btn) btn.addEventListener('click', run);
            var verifyBtn = document.getElementById('bt-verify-btn');
            if (verifyBtn) verifyBtn.addEventListener('click', verifyStorageOnly);
            var tradeBtn = document.getElementById('bt-trade-btn');
            if (tradeBtn) tradeBtn.addEventListener('click', openInTradePage);
        }

        if (btPageMode === 'optimize') {
            loadBacktestStrategies().then(function () {
                syncOptStrategyUi();
            });
            var optStrategy = document.getElementById('bt-opt-strategy');
            if (optStrategy) optStrategy.addEventListener('change', syncOptStrategyUi);
            var optBtn = document.getElementById('bt-opt-run-btn');
            if (optBtn) optBtn.addEventListener('click', runOptimize);
        }

        global.addEventListener('resize', function () {
            ['bt-equity-chart', 'bt-bar-chart', 'bt-verify-chart'].forEach(function (id) {
                scheduleChartResize(id);
            });
        });
    }

    global.QuantSevBridge = global.QuantSevBridge || {};
    global.QuantSevBridge.Backtest = {
        run: run,
        runOptimize: runOptimize,
        renderResult: renderResult,
        renderOptimize: renderOptimize,
        initPage: initPage,
        loadContractSpec: loadContractSpec,
        verifyStorage: verifyStorageOnly,
        openInTradePage: openInTradePage,
        loadLastResult: loadLastResult
    };

    document.addEventListener('DOMContentLoaded', function () {
        if (document.body && document.body.getAttribute('data-bt-page')) {
            return;
        }
        initPage('run');
    });
})(typeof window !== 'undefined' ? window : this);
