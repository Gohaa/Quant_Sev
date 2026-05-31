/** Data_Bridge.js — 逐级浏览 data/ 交易所→品种→月份→周期 + 查看/导入/删除 */
(function (global) {
    'use strict';

    var navState = { exchange: null, product: null, month_slot: null };
    var currentFiles = [];
    var selectedFileIdx = -1;
    var pendingImportRow = null;
    var filterKeyword = '';

    var PERIOD_LABELS = {
        tick: 'Tick 逐笔',
        m1: 'M1 一分',
        m15: 'M15 十五分',
        h1: 'H1 小时',
        d1: 'D1 日线'
    };
    var EXCHANGE_LABELS = {
        CFFEX: 'CFFEX 中金所',
        CZCE: 'CZCE 郑商所',
        DCE: 'DCE 大商所',
        GFEX: 'GFEX 广期所',
        INE: 'INE 能源中心',
        SHFE: 'SHFE 上期所'
    };

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

    function exchangeLabel(code) {
        return EXCHANGE_LABELS[code] || code || '—';
    }

    function periodLabel(code) {
        return PERIOD_LABELS[code] || code || '—';
    }

    function monthLabel(slot) {
        if (!slot) return '—';
        if (slot === 'main') return 'main 主力连续';
        return slot + ' 月';
    }

    function formatSize(bytes) {
        var n = Number(bytes) || 0;
        if (n <= 0) return '—';
        if (n < 1024) return n + ' B';
        if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
        return (n / (1024 * 1024)).toFixed(1) + ' MB';
    }

    function browse(params) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.get('/api/data/browse', params || {});
    }

    function isBrowseNotFound(err) {
        var msg = (err && err.message) || '';
        return /not found|404/i.test(msg) && /browse/i.test(msg);
    }

    function formatBrowseError(err) {
        if (isBrowseNotFound(err)) {
            return '后端未提供 /api/data/browse — 请关闭旧进程并启动新版 build-msvc\\bin\\Debug\\quant_sev_host.exe，再通过 http://127.0.0.1:8080/Mainwindow.html 打开';
        }
        return (err && err.message) || String(err);
    }

    function previewFile(row, limit) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        if (!row) return Promise.reject(new Error('未选择文件'));
        var q = {
            limit: limit || 100,
            exchange: row.exchange || navState.exchange || '',
            product: row.product || navState.product || '',
            month_slot: row.month_slot || navState.month_slot || '',
            period: row.period || '',
            instrument_id: row.instrument_id || ''
        };
        if (row.path) q.path = row.path;
        if (!q.period) return Promise.reject(new Error('缺少周期参数'));
        return t.get('/api/data/preview', q);
    }

    function deleteFile(path) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.post('/api/data/delete', { path: path });
    }

    function importCsv(row, csvContent, mode) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.post('/api/data/import', {
            exchange: row.exchange,
            product: row.product,
            month_slot: row.month_slot,
            period: row.period,
            csv_content: csvContent,
            mode: mode || 'replace'
        });
    }

    function subscribeSymbols(instrumentIds) {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        var uid = resolveUserId();
        if (!uid) return Promise.reject(new Error('请先在账户页连接并选择 user_id'));
        var ids = Array.isArray(instrumentIds) ? instrumentIds : [instrumentIds];
        return t.post('/api/load/symbol', { user_id: uid, instrument_ids: ids });
    }

    function getStatus() {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.get('/api/status');
    }

    function getSubscribed() {
        var t = transport();
        if (!t) return Promise.reject(new Error('Transport 不可用'));
        return t.get('/api/subscribed_symbols');
    }

    function matchesKeywordName(name) {
        var kw = (filterKeyword || '').trim().toUpperCase();
        if (!kw) return true;
        return String(name || '').toUpperCase().indexOf(kw) >= 0;
    }

    function setColLoading(colId, text) {
        var el = document.getElementById(colId);
        if (el) el.innerHTML = '<div class="tree-empty">' + escapeHtml(text || '加载中…') + '</div>';
    }

    function renderNameColumn(colId, items, selected, onSelect, labelFn) {
        var el = document.getElementById(colId);
        if (!el) return;
        var filtered = (items || []).filter(function (it) {
            return matchesKeywordName(it.name || it);
        });
        if (!filtered.length) {
            el.innerHTML = '<div class="tree-empty">无匹配项</div>';
            return;
        }
        el.innerHTML = filtered.map(function (it) {
            var name = it.name || it;
            var cls = name === selected ? 'tree-item active' : 'tree-item';
            var label = labelFn ? labelFn(name) : name;
            return '<button type="button" class="' + cls + '" data-key="' + escapeHtml(name) + '">' +
                escapeHtml(label) + '</button>';
        }).join('');
        el.querySelectorAll('.tree-item').forEach(function (btn) {
            btn.addEventListener('click', function () {
                onSelect(btn.getAttribute('data-key'));
            });
        });
    }

    function renderBreadcrumb() {
        var el = document.getElementById('data-breadcrumb');
        if (!el) return;
        if (!navState.exchange) {
            el.textContent = '请选择交易所';
            return;
        }
        var parts = [exchangeLabel(navState.exchange)];
        if (navState.product) parts.push(navState.product);
        if (navState.month_slot) parts.push(monthLabel(navState.month_slot));
        el.textContent = parts.join('  ›  ');
    }

    function renderSummary(fileCount) {
        var el = document.getElementById('data-summary-bar');
        if (!el) return;
        el.innerHTML =
            '<span>当前路径文件 <b>' + (fileCount || 0) + '</b></span>' +
            '<span class="status-muted">落盘规则 data/{交易所}/{品种}/{月份}/{周期}.csv</span>';
    }

    function renderStatusStrip() {
        var el = document.getElementById('data-status-bar');
        if (!el) return Promise.resolve();
        return Promise.allSettled([getStatus(), getSubscribed()]).then(function (res) {
            var st = res[0].status === 'fulfilled' ? res[0].value : null;
            var sub = res[1].status === 'fulfilled' ? res[1].value : null;
            var mdOk = st && st.md_ok;
            var tdOk = st && st.td_ok;
            var subCount = (sub && sub.instruments && sub.instruments.length) || 0;
            var phase = (st && st.phase) || '?';
            el.innerHTML =
                '<span class="' + (mdOk ? 'status-ok' : 'status-off') + '">Md ' + (mdOk ? '已连' : '未连') + '</span>' +
                '<span class="' + (tdOk ? 'status-ok' : 'status-off') + '">Td ' + (tdOk ? '已连' : '未连') + '</span>' +
                '<span>订阅 <b>' + subCount + '</b> 合约</span>' +
                '<span class="status-muted">逐级扫描，不重复全量遍历</span>' +
                '<span class="status-muted">API phase ' + escapeHtml(phase) + '</span>';
        });
    }

    function renderBarDetail(bars, meta) {
        var tbody = document.getElementById('data-detail-body');
        var title = document.getElementById('data-detail-title');
        if (!tbody) return;
        var label = [meta && meta.exchange, meta && meta.product, meta && meta.month_slot, meta && meta.period]
            .filter(Boolean).join(' / ');
        if (title) {
            title.textContent = '📊 数据详情 · ' + label + ' · 末 ' + Math.min(30, (bars || []).length) + ' 根';
        }
        if (!bars || !bars.length) {
            tbody.innerHTML = '<tr><td colspan="8">无 Bar 数据（文件可能为空或路径无效）</td></tr>';
            return;
        }
        var tail = bars.slice(-30);
        tbody.innerHTML = tail.map(function (b) {
            return '<tr><td>' + escapeHtml((b.date || '') + ' ' + (b.time || '')) + '</td>' +
                '<td>' + escapeHtml(b.open) + '</td><td>' + escapeHtml(b.high) + '</td>' +
                '<td>' + escapeHtml(b.low) + '</td><td>' + escapeHtml(b.close) + '</td>' +
                '<td>' + escapeHtml(b.volume) + '</td><td>' + escapeHtml(b.turnover) + '</td>' +
                '<td>' + escapeHtml(b.open_interest) + '</td></tr>';
        }).join('');
    }

    function renderTickDetail(ticks, meta) {
        var tbody = document.getElementById('data-detail-body');
        var title = document.getElementById('data-detail-title');
        if (!tbody) return;
        var label = [meta && meta.exchange, meta && meta.product, meta && meta.month_slot, 'tick']
            .filter(Boolean).join(' / ');
        if (title) {
            title.textContent = '📊 Tick 详情 · ' + label + ' · 末 ' + Math.min(30, (ticks || []).length) + ' 条';
        }
        if (!ticks || !ticks.length) {
            tbody.innerHTML = '<tr><td colspan="8">无 Tick 数据（文件可能为空或路径无效）</td></tr>';
            return;
        }
        var tail = ticks.slice(-30);
        tbody.innerHTML = tail.map(function (t) {
            return '<tr><td>' + escapeHtml((t.trading_day || '') + ' ' + (t.update_time || '') +
                (t.update_millisec ? '.' + t.update_millisec : '')) + '</td>' +
                '<td colspan="3">' + escapeHtml(t.last_price) + '</td>' +
                '<td>' + escapeHtml(t.volume) + '</td>' +
                '<td>' + escapeHtml(t.turnover) + '</td>' +
                '<td>' + escapeHtml(t.open_interest) + '</td>' +
                '<td>—</td></tr>';
        }).join('');
    }

    function getSelectedFile() {
        if (selectedFileIdx < 0 || selectedFileIdx >= currentFiles.length) return null;
        return currentFiles[selectedFileIdx];
    }

    function updateFileHeaderActions() {
        var row = getSelectedFile();
        var disabled = !row;
        ['data-file-view-btn', 'data-file-import-btn', 'data-file-delete-btn'].forEach(function (id) {
            var btn = document.getElementById(id);
            if (btn) btn.disabled = disabled;
        });
    }

    function selectFileIdx(idx) {
        selectedFileIdx = idx;
        var row = getSelectedFile();
        var pathEl = document.getElementById('data-selected-path');
        if (pathEl) pathEl.textContent = (row && row.path) ? row.path : '选中周期文件后显示 Storage 路径';
        renderFilesPanel();
    }

    function renderFilesPanel() {
        var el = document.getElementById('data-col-files');
        if (!el) return;
        if (!navState.exchange || !navState.product || !navState.month_slot) {
            el.innerHTML = '<div class="tree-empty">先选交易所、品种、月份</div>';
            selectedFileIdx = -1;
            updateFileHeaderActions();
            renderSummary(0);
            return;
        }
        if (!currentFiles.length) {
            el.innerHTML = '<div class="tree-empty">该月份目录下无 CSV 文件</div>';
            selectedFileIdx = -1;
            updateFileHeaderActions();
            renderSummary(0);
            return;
        }
        if (selectedFileIdx >= currentFiles.length) selectedFileIdx = -1;

        el.innerHTML = currentFiles.map(function (row, idx) {
            var cls = idx === selectedFileIdx ? 'tree-item file-row active' : 'tree-item file-row';
            return '<button type="button" class="' + cls + '" data-file-idx="' + idx + '">' +
                escapeHtml(periodLabel(row.period)) +
                '<span class="file-meta">' + escapeHtml(formatSize(row.size_bytes)) + '</span>' +
                '</button>';
        }).join('');

        el.querySelectorAll('.file-row').forEach(function (btn) {
            btn.addEventListener('click', function () {
                selectFileIdx(Number(btn.getAttribute('data-file-idx')));
            });
            btn.addEventListener('dblclick', function () {
                var idx = Number(btn.getAttribute('data-file-idx'));
                selectFileIdx(idx);
                viewFile(getSelectedFile());
            });
        });
        updateFileHeaderActions();
        renderSummary(currentFiles.length);
    }

    function viewFile(row) {
        if (!row) return;
        var pathEl = document.getElementById('data-selected-path');
        if (pathEl) pathEl.textContent = row.path || '—';

        var tbody = document.getElementById('data-detail-body');
        if (tbody) tbody.innerHTML = '<tr><td colspan="8">加载预览中…</td></tr>';

        previewFile(row, 100).then(function (res) {
            if (res && res.file_type === 'tick') {
                renderTickDetail((res && res.ticks) || [], res);
            } else {
                renderBarDetail((res && res.bars) || [], res);
            }
        }).catch(function (e) {
            if (tbody) tbody.innerHTML = '<tr><td colspan="8">预览失败: ' + escapeHtml(e.message) + '</td></tr>';
        });

        var detail = document.getElementById('data-detail-section');
        if (detail && detail.scrollIntoView) detail.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
    }

    function loadExchanges() {
        setColLoading('data-col-exchange', '扫描 data/ 交易所…');
        setColLoading('data-col-product', '—');
        setColLoading('data-col-month', '—');
        setColLoading('data-col-files', '—');
        return browse({}).then(function (data) {
            var items = (data && data.items) || [];
            renderNameColumn('data-col-exchange', items, navState.exchange, function (key) {
                navState.exchange = key;
                navState.product = null;
                navState.month_slot = null;
                currentFiles = [];
                selectedFileIdx = -1;
                loadProducts();
            }, exchangeLabel);
            renderBreadcrumb();
            var countEl = document.getElementById('data-file-count');
            if (countEl) countEl.textContent = items.length + ' 家交易所';
        }).catch(function (e) {
            setColLoading('data-col-exchange', formatBrowseError(e));
        });
    }

    function loadProducts() {
        if (!navState.exchange) return Promise.resolve();
        setColLoading('data-col-product', '扫描品种…');
        setColLoading('data-col-month', '—');
        setColLoading('data-col-files', '—');
        return browse({ exchange: navState.exchange }).then(function (data) {
            var items = (data && data.items) || [];
            renderNameColumn('data-col-product', items, navState.product, function (key) {
                navState.product = key;
                navState.month_slot = null;
                currentFiles = [];
                selectedFileIdx = -1;
                loadMonths();
            });
            renderBreadcrumb();
        }).catch(function (e) {
            setColLoading('data-col-product', formatBrowseError(e));
        });
    }

    function loadMonths() {
        if (!navState.exchange || !navState.product) return Promise.resolve();
        setColLoading('data-col-month', '扫描月份…');
        setColLoading('data-col-files', '—');
        return browse({ exchange: navState.exchange, product: navState.product }).then(function (data) {
            var items = (data && data.items) || [];
            renderNameColumn('data-col-month', items, navState.month_slot, function (key) {
                navState.month_slot = key;
                loadFiles();
            }, monthLabel);
            renderBreadcrumb();
        }).catch(function (e) {
            setColLoading('data-col-month', formatBrowseError(e));
        });
    }

    function loadFiles() {
        if (!navState.exchange || !navState.product || !navState.month_slot) return Promise.resolve();
        setColLoading('data-col-files', '读取周期文件…');
        return browse({
            exchange: navState.exchange,
            product: navState.product,
            month_slot: navState.month_slot
        }).then(function (data) {
            currentFiles = (data && data.items) || [];
            selectedFileIdx = currentFiles.length ? 0 : -1;
            renderFilesPanel();
            renderBreadcrumb();
        }).catch(function (e) {
            setColLoading('data-col-files', formatBrowseError(e));
        });
    }

    function confirmDelete(row) {
        if (!row || !row.path) return;
        var label = [row.exchange, row.product, row.month_slot, row.period].join('/');
        if (!confirm('确认删除 data/' + label + ' ？\n' + row.path)) return;
        deleteFile(row.path).then(function () {
            alert('已删除');
            loadFiles();
        }).catch(function (e) { alert('删除失败: ' + e.message); });
    }

    function promptImport(row) {
        pendingImportRow = row;
        var input = document.getElementById('data-import-file');
        if (input) {
            input.value = '';
            input.click();
        }
    }

    function handleImportFileSelected(ev) {
        var file = ev.target.files && ev.target.files[0];
        if (!file || !pendingImportRow) return;
        var row = pendingImportRow;
        pendingImportRow = null;
        var mode = 'replace';
        if (Number(row.size_bytes) > 0) {
            var choice = confirm('文件已有数据 (' + formatSize(row.size_bytes) + ')。\n确定=覆盖\n取消=追加');
            mode = choice ? 'replace' : 'append';
        }
        var reader = new FileReader();
        reader.onload = function () {
            importCsv(row, String(reader.result || ''), mode).then(function (res) {
                alert((res.message || '导入成功') + '\n行数: ' + (res.row_count || 0));
                loadFiles().then(function () {
                    var idx = currentFiles.findIndex(function (f) { return f.period === row.period; });
                    if (idx >= 0) {
                        selectFileIdx(idx);
                        viewFile(currentFiles[idx]);
                    }
                });
            }).catch(function (e) { alert('导入失败: ' + e.message); });
        };
        reader.onerror = function () { alert('读取文件失败'); };
        reader.readAsText(file, 'UTF-8');
    }

    function refreshCurrentBranch() {
        var chain = loadExchanges();
        if (navState.exchange) chain = chain.then(loadProducts);
        if (navState.product) chain = chain.then(loadMonths);
        if (navState.month_slot) chain = chain.then(loadFiles);
        return chain;
    }

    function showAccessHint() {
        var el = document.getElementById('data-access-hint');
        if (!el) return;
        var viaHost = false;
        try {
            viaHost = global.location && global.location.protocol && global.location.protocol.indexOf('http') === 0;
        } catch (e) { /* ignore */ }
        if (viaHost) {
            el.textContent = '落盘规则：data/{交易所}/{品种}/{月份}/{周期}.csv — 选中周期后点标题栏 查看/导入/删除（双击行可查看）';
            return;
        }
        el.textContent = '⚠ 请运行 quant_sev_host.exe 后访问 http://127.0.0.1:8080/Mainwindow.html';
    }

    function init() {
        showAccessHint();
        var kwEl = document.getElementById('data-filter-keyword');
        if (kwEl) {
            kwEl.addEventListener('input', function () {
                filterKeyword = kwEl.value;
                refreshCurrentBranch();
            });
        }
        var refreshBtn = document.getElementById('data-refresh-btn');
        if (refreshBtn) refreshBtn.addEventListener('click', function () {
            renderStatusStrip();
            refreshCurrentBranch();
        });
        var subBtn = document.getElementById('data-subscribe-btn');
        var subInput = document.getElementById('data-subscribe-input');
        if (subBtn && subInput) {
            subBtn.addEventListener('click', function () {
                var raw = (subInput.value || '').trim();
                if (!raw) return;
                var ids = raw.split(/[,;\s]+/).map(function (s) { return s.trim().toLowerCase(); }).filter(Boolean);
                subscribeSymbols(ids).then(function () {
                    alert('已订阅 ' + ids.length + ' 个合约');
                    renderStatusStrip();
                }).catch(function (e) { alert(e.message); });
            });
        }
        var importInput = document.getElementById('data-import-file');
        if (importInput) importInput.addEventListener('change', handleImportFileSelected);

        var viewBtn = document.getElementById('data-file-view-btn');
        if (viewBtn) viewBtn.addEventListener('click', function () {
            var row = getSelectedFile();
            if (row) viewFile(row);
        });
        var importBtn = document.getElementById('data-file-import-btn');
        if (importBtn) importBtn.addEventListener('click', function () {
            var row = getSelectedFile();
            if (row) promptImport(row);
        });
        var deleteBtn = document.getElementById('data-file-delete-btn');
        if (deleteBtn) deleteBtn.addEventListener('click', function () {
            var row = getSelectedFile();
            if (row) confirmDelete(row);
        });

        renderStatusStrip().then(function () {
            return getStatus();
        }).then(function (st) {
            if (st && st.data_browse === false) {
                setColLoading('data-col-exchange', '后端版本过旧(data_browse=false)，请重启新版 quant_sev_host.exe');
            }
        }).catch(function () { /* ignore */ });
        loadExchanges();
        setInterval(renderStatusStrip, 8000);
    }

    global.QuantSevBridge = global.QuantSevBridge || {};
    global.QuantSevBridge.Data = {
        browse: browse,
        previewFile: previewFile,
        deleteFile: deleteFile,
        importCsv: importCsv,
        loadExchanges: loadExchanges,
        viewFile: viewFile,
        init: init
    };

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }
})(typeof window !== 'undefined' ? window : this);
