/** Strategies_Bridge.js — MT5 风格：多合约 / 分腿手数 / 分腿参数 */
(function (global) {
    'use strict';

    var META_KEYS = {
        id: 1, name: 1, period: 1, logic: 1, description: 1, state: 1, user_id: 1,
        instrument_id: 1, volume: 1, default_instrument_id: 1, default_volume: 1,
        min_interval_sec: 1, daily_limit: 1, orders_today: 1, legs: 1
    };

    var strategiesCache = [];
    var selectedId = null;
    var inputSpecsCache = {};
    var draftById = {};

    function transport() {
        return global.QuantSevBridge && QuantSevBridge.Logger && QuantSevBridge.Logger.Transport;
    }

    function resolveUserId() {
        try {
            var stored = global.localStorage && localStorage.getItem('quant_sev_user_id');
            if (stored) return stored;
        } catch (e) { /* ignore */ }
        return '';
    }

    function escapeHtml(text) {
        return String(text == null ? '' : text)
            .replace(/&/g, '&amp;')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;');
    }

    function defaultParamsFromStrategy(stg) {
        var params = {};
        if (!stg) return params;
        Object.keys(stg).forEach(function (key) {
            if (META_KEYS[key]) return;
            var val = stg[key];
            if (val != null && typeof val !== 'object') {
                params[key] = val;
            }
        });
        return params;
    }

    function normalizeLegs(stg) {
        if (stg.legs && stg.legs.length) {
            return stg.legs.map(function (leg) {
                return {
                    instrument_id: String(leg.instrument_id || '').toLowerCase(),
                    volume: Number(leg.volume) > 0 ? Number(leg.volume) : 1,
                    params: Object.assign({}, defaultParamsFromStrategy(stg), leg.params || {})
                };
            });
        }
        return [{
            instrument_id: String(stg.default_instrument_id || stg.instrument_id || 'rb2610').toLowerCase(),
            volume: Number(stg.default_volume != null ? stg.default_volume : stg.volume) || 1,
            params: defaultParamsFromStrategy(stg)
        }];
    }

    function getDraft(stg) {
        if (!stg) return null;
        if (draftById[stg.id]) return draftById[stg.id];
        draftById[stg.id] = {
            min_interval_sec: stg.min_interval_sec != null ? stg.min_interval_sec : 60,
            daily_limit: stg.daily_limit != null ? stg.daily_limit : 20,
            legs: normalizeLegs(stg)
        };
        return draftById[stg.id];
    }

    function findStrategy(id) {
        for (var i = 0; i < strategiesCache.length; i++) {
            if (strategiesCache[i].id === id) return strategiesCache[i];
        }
        return null;
    }

    function loadInputSpecs(logic) {
        var t = transport();
        if (!t || !logic) return Promise.resolve([]);
        if (inputSpecsCache[logic]) {
            return Promise.resolve(inputSpecsCache[logic]);
        }
        return t.get('/api/backtest/strategy_inputs?strategy=' + encodeURIComponent(logic)).then(function (data) {
            var inputs = (data && data.inputs) || [];
            inputSpecsCache[logic] = inputs;
            return inputs;
        }).catch(function () {
            inputSpecsCache[logic] = [];
            return [];
        });
    }

    function renderList() {
        var tbody = document.getElementById('stg-tbody');
        if (!tbody) return;
        if (!strategiesCache.length) {
            tbody.innerHTML = '<tr><td colspan="4">暂无策略，请检查 config/Strategy_list.json</td></tr>';
            return;
        }
        tbody.innerHTML = strategiesCache.map(function (stg) {
            var running = stg.state === 'running';
            var statusClass = running ? 'status-running' : 'status-stopped';
            var statusText = running ? '运行中' : '已停止';
            var legCount = (stg.legs && stg.legs.length) ||
                (stg.instrument_id ? 1 : (stg.default_instrument_id ? 1 : 0));
            var selected = stg.id === selectedId ? ' selected' : '';
            return '<tr class="stg-row' + selected + '" data-id="' + escapeHtml(stg.id) + '">' +
                '<td>' + escapeHtml(stg.name) + '</td>' +
                '<td>' + escapeHtml(stg.period) + '</td>' +
                '<td class="' + statusClass + '">' + statusText + '</td>' +
                '<td>' + legCount + ' 合约</td></tr>';
        }).join('');

        tbody.querySelectorAll('.stg-row').forEach(function (row) {
            row.addEventListener('click', function () {
                selectStrategy(row.getAttribute('data-id'));
            });
        });
    }

    function renderConfigPanel() {
        var panel = document.getElementById('stg-config-body');
        if (!panel) return;
        var stg = findStrategy(selectedId);
        if (!stg) {
            panel.innerHTML = '<div class="config-empty">← 选择左侧策略进行配置</div>';
            return;
        }

        var draft = getDraft(stg);
        var running = stg.state === 'running';
        var logic = stg.logic || 'dual_thrust';

        loadInputSpecs(logic).then(function (inputs) {
            var html = '';
            html += '<div class="config-head">';
            html += '<div><div class="config-name">' + escapeHtml(stg.name) + '</div>';
            html += '<div class="config-meta">' + escapeHtml(stg.period) + ' · ' + escapeHtml(logic) +
                (stg.description ? ' · ' + escapeHtml(stg.description) : '') + '</div></div>';
            html += '<div class="config-actions">';
            if (running) {
                html += '<button type="button" class="btn btn-stop" id="stg-stop-btn">停止</button>';
            } else {
                html += '<button type="button" class="btn btn-start" id="stg-start-btn">启动</button>';
            }
            html += '</div></div>';

            html += '<div class="config-section">';
            html += '<div class="section-title">全局风控</div>';
            html += '<div class="global-grid">';
            html += '<label>最小间隔(秒)<input type="number" min="0" id="cfg-min-interval" value="' +
                escapeHtml(draft.min_interval_sec) + '"' + (running ? ' disabled' : '') + '></label>';
            html += '<label>每日上限<input type="number" min="1" id="cfg-daily-limit" value="' +
                escapeHtml(draft.daily_limit) + '"' + (running ? ' disabled' : '') + '></label>';
            html += '<label>关联账户<span class="readonly-val">' + escapeHtml(resolveUserId() || '未选择') +
                '</span></label>';
            html += '</div></div>';

            html += '<div class="config-section">';
            html += '<div class="section-head"><span class="section-title">交易合约（每行独立手数与参数）</span>';
            if (!running) {
                html += '<button type="button" class="btn btn-add" id="stg-add-leg-btn">+ 添加合约</button>';
            }
            html += '</div>';

            html += '<div class="legs-wrap"><table class="legs-table"><thead><tr>';
            html += '<th>合约</th><th>手数</th>';
            inputs.forEach(function (spec) {
                html += '<th>' + escapeHtml(spec.label || spec.key) + '</th>';
            });
            html += '<th></th></tr></thead><tbody id="legs-tbody">';
            html += buildLegRowsHtml(draft.legs, inputs, running);
            html += '</tbody></table></div></div>';

            panel.innerHTML = html;
            bindConfigEvents(stg, draft, inputs, running);
        });
    }

    function buildLegRowsHtml(legs, inputs, disabled) {
        return legs.map(function (leg, idx) {
            var row = '<tr data-leg-idx="' + idx + '">';
            row += '<td><input class="leg-inst" type="text" value="' + escapeHtml(leg.instrument_id) +
                '" placeholder="rb2610"' + (disabled ? ' disabled' : '') + '></td>';
            row += '<td><input class="leg-vol" type="number" min="1" value="' + escapeHtml(leg.volume) + '"' +
                (disabled ? ' disabled' : '') + '></td>';
            inputs.forEach(function (spec) {
                var val = leg.params[spec.key];
                if (val == null) val = spec.default;
                var step = spec.type === 'int' ? '1' : '0.01';
                row += '<td><input class="leg-param" data-key="' + escapeHtml(spec.key) + '" type="number" step="' +
                    step + '" value="' + escapeHtml(val) + '"' + (disabled ? ' disabled' : '') + '></td>';
            });
            row += '<td>' + (disabled ? '' : '<button type="button" class="btn-link leg-del">删除</button>') + '</td>';
            row += '</tr>';
            return row;
        }).join('');
    }

    function readDraftFromDom(stg, draft, inputs) {
        var minEl = document.getElementById('cfg-min-interval');
        var dailyEl = document.getElementById('cfg-daily-limit');
        if (minEl) draft.min_interval_sec = Number(minEl.value) || 0;
        if (dailyEl) draft.daily_limit = Number(dailyEl.value) || 1;

        var tbody = document.getElementById('legs-tbody');
        if (!tbody) return draft;
        draft.legs = [];
        tbody.querySelectorAll('tr[data-leg-idx]').forEach(function (tr) {
            var inst = (tr.querySelector('.leg-inst') && tr.querySelector('.leg-inst').value || '').trim().toLowerCase();
            var vol = Number(tr.querySelector('.leg-vol') && tr.querySelector('.leg-vol').value) || 0;
            if (!inst || vol <= 0) return;
            var params = Object.assign({}, defaultParamsFromStrategy(stg));
            tr.querySelectorAll('.leg-param').forEach(function (inp) {
                var key = inp.getAttribute('data-key');
                if (!key) return;
                params[key] = inp.type === 'number' ? Number(inp.value) : inp.value;
            });
            draft.legs.push({ instrument_id: inst, volume: vol, params: params });
        });
        draftById[stg.id] = draft;
        return draft;
    }

    function bindConfigEvents(stg, draft, inputs, running) {
        if (running) return;

        var addBtn = document.getElementById('stg-add-leg-btn');
        if (addBtn) {
            addBtn.addEventListener('click', function () {
                readDraftFromDom(stg, draft, inputs);
                draft.legs.push({
                    instrument_id: stg.default_instrument_id || 'rb2610',
                    volume: stg.default_volume || 1,
                    params: defaultParamsFromStrategy(stg)
                });
                renderConfigPanel();
            });
        }

        var tbody = document.getElementById('legs-tbody');
        if (tbody) {
            tbody.querySelectorAll('.leg-del').forEach(function (btn) {
                btn.addEventListener('click', function () {
                    readDraftFromDom(stg, draft, inputs);
                    var tr = btn.closest('tr');
                    var idx = Number(tr.getAttribute('data-leg-idx'));
                    if (draft.legs.length <= 1) {
                        alert('至少保留一个合约');
                        return;
                    }
                    draft.legs.splice(idx, 1);
                    renderConfigPanel();
                });
            });
        }

        var startBtn = document.getElementById('stg-start-btn');
        if (startBtn) {
            startBtn.addEventListener('click', function () {
                Strategies.start(stg.id);
            });
        }
        var stopBtn = document.getElementById('stg-stop-btn');
        if (stopBtn) {
            stopBtn.addEventListener('click', function () {
                Strategies.stop(stg.id);
            });
        }
    }

    function selectStrategy(id) {
        selectedId = id;
        renderList();
        renderConfigPanel();
    }

    function refreshList() {
        var t = transport();
        if (!t) return Promise.resolve();
        return t.get('/api/strategies').then(function (data) {
            strategiesCache = (data && data.strategies) || [];
            if (selectedId && !findStrategy(selectedId) && strategiesCache.length) {
                selectedId = strategiesCache[0].id;
            }
            if (!selectedId && strategiesCache.length) {
                selectedId = strategiesCache[0].id;
            }
            strategiesCache.forEach(function (stg) {
                if (stg.state === 'running' && stg.legs && stg.legs.length) {
                    draftById[stg.id] = {
                        min_interval_sec: stg.min_interval_sec,
                        daily_limit: stg.daily_limit,
                        legs: normalizeLegs(stg)
                    };
                }
            });
            renderList();
            renderConfigPanel();
        }).catch(function (err) {
            var tbody = document.getElementById('stg-tbody');
            if (tbody) {
                tbody.innerHTML = '<tr><td colspan="4">加载失败: ' + escapeHtml(err.message) + '</td></tr>';
            }
        });
    }

    function start(strategyId) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var userId = resolveUserId();
        if (!userId) {
            alert('请先在账户页选择并连接账户');
            return Promise.reject(new Error('未选择账户'));
        }
        var stg = findStrategy(strategyId);
        if (!stg) return Promise.reject(new Error('策略不存在'));
        var draft = getDraft(stg);
        readDraftFromDom(stg, draft, inputSpecsCache[stg.logic] || []);

        if (!draft.legs.length) {
            alert('请至少配置一个合约');
            return Promise.reject(new Error('无合约 leg'));
        }

        var payload = {
            strategy_id: strategyId,
            user_id: userId,
            min_interval_sec: draft.min_interval_sec,
            daily_limit: draft.daily_limit,
            legs: draft.legs.map(function (leg) {
                return {
                    instrument_id: leg.instrument_id,
                    volume: leg.volume,
                    params: leg.params
                };
            })
        };

        return t.post('/api/strategy/start', payload).then(function () {
            if (global.QuantSevBridge.Logger) {
                QuantSevBridge.Logger.log('策略已启动: ' + strategyId + ' (' + draft.legs.length + ' 合约)');
            }
            if (global.QuantSevBridge.Diagnostic) {
                QuantSevBridge.Diagnostic.refreshPanel('diag-panel-body');
            }
            return refreshList();
        }).catch(function (err) {
            alert('启动失败: ' + (err.message || String(err)));
        });
    }

    function stop(strategyId) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.post('/api/strategy/stop', { strategy_id: strategyId }).then(function () {
            if (global.QuantSevBridge.Logger) {
                QuantSevBridge.Logger.log('策略已停止: ' + strategyId);
            }
            if (global.QuantSevBridge.Diagnostic) {
                QuantSevBridge.Diagnostic.refreshPanel('diag-panel-body');
            }
            return refreshList();
        }).catch(function (err) {
            alert('停止失败: ' + (err.message || String(err)));
        });
    }

    function sendSignal(payload) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var body = payload || {};
        if (!body.user_id) body.user_id = resolveUserId();
        return t.post('/api/strategy/signal', body);
    }

    var Strategies = {
        refreshList: refreshList,
        selectStrategy: selectStrategy,
        start: start,
        stop: stop,
        sendSignal: sendSignal
    };

    global.QuantSevBridge = global.QuantSevBridge || {};
    global.QuantSevBridge.Strategies = Strategies;

    document.addEventListener('DOMContentLoaded', function () {
        refreshList();
        setInterval(refreshList, 5000);
    });
})(typeof window !== 'undefined' ? window : this);
