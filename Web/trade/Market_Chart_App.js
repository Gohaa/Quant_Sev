class MarketChartApp {
    constructor() {
        this.mainChart = null;
        this.macdChart = null;
        this.chipChart = null;
        this.domPanel = null;
        this.instrumentId = 'rb2610';
        this.period = 'm15';
        this.activeSubIndicator = 'macd';
        this.useApi = false;
        this.liveTickCount = 0;
        this.indicatorCatalogReady = false;

        this.subpageConfig = {
            main: { id: 'main-chart-panel', visible: true, height: 50 },
            macd: { id: 'sub-chart-macd', visible: true, height: 20 },
            dom: { id: 'dom-panel', visible: false, height: 15 },
            chip: { id: 'chip-panel', visible: false, height: 15 }
        };
    }

    bridge() {
        try { return window.parent.QuantSevBridge; } catch (e) { return null; }
    }

    tradeView() {
        const b = this.bridge();
        return b && b.TradeView ? b.TradeView : null;
    }

    setStatus(text, isError) {
        const el = document.getElementById('chart-status');
        if (!el) return;
        el.textContent = text || '';
        el.style.color = isError ? '#e74c3c' : '#95a5a6';
    }

    resolveInstrumentId() {
        const input = document.getElementById('instrument-input');
        if (input && input.value.trim()) {
            return input.value.trim().toLowerCase();
        }
        try {
            const stored = localStorage.getItem('quant_sev_chart_instrument') ||
                localStorage.getItem('quant_sev_last_contract');
            if (stored) return stored.toLowerCase();
        } catch (e) { /* ignore */ }
        return 'rb2610';
    }

    init() {
        this.instrumentId = this.resolveInstrumentId();
        const input = document.getElementById('instrument-input');
        if (input) input.value = this.instrumentId;

        this.period = document.getElementById('timeframe-select').value || 'm15';

        this.initCharts();
        this.initIndicatorCatalog().then(() => {
            this.initToolbar();
            this.initIndicatorPickers();
        });
        this.initContextMenu();
        this.initResizeHandles();
        this.initSubpageActions();
        this.initInstrumentSync();
        this.initTickStream();
        this.reloadMarketData();
    }

    initIndicatorCatalog() {
        const self = this;
        const tv = this.tradeView();
        if (!tv || !window.IndicatorCatalog) {
            return Promise.resolve();
        }
        return IndicatorCatalog.load(tv).then(function () {
            self.populateIndicatorSelects();
            self.indicatorCatalogReady = true;
        }).catch(function (err) {
            self.setStatus('指标目录加载失败: ' + err.message, true);
        });
    }

    populateIndicatorSelects() {
        this.populateIndicatorPickers();
    }

    populateIndicatorPickers() {
        const overlayList = document.getElementById('overlay-picker-list');
        const subList = document.getElementById('sub-picker-list');
        if (!overlayList || !subList || !window.IndicatorCatalog) return;

        overlayList.innerHTML = '';
        subList.innerHTML = '';

        IndicatorCatalog.byType('overlay').forEach(function (ind) {
            const label = document.createElement('label');
            label.className = 'indicator-picker-item';
            label.innerHTML =
                '<input type="checkbox" value="' + ind.name + '">' +
                '<span class="indicator-name">' + (ind.full_name || ind.name) + '</span>' +
                '<span class="indicator-id">' + ind.name + '</span>';
            overlayList.appendChild(label);
        });

        const subTypes = ['indicator', 'math', 'simple', 'comparative'];
        IndicatorCatalog.getAll().filter(function (ind) {
            return subTypes.indexOf(ind.type) >= 0;
        }).forEach(function (ind) {
            const item = document.createElement('div');
            item.className = 'indicator-picker-item';
            item.dataset.value = ind.name;
            item.innerHTML =
                '<span class="indicator-name">' + (ind.full_name || ind.name) + '</span>' +
                '<span class="indicator-id">' + ind.name + '</span>';
            subList.appendChild(item);
        });

        const defaults = ['sma', 'ema', 'bbands', 'psar'];
        defaults.forEach(function (name) {
            const cb = overlayList.querySelector('input[value="' + name + '"]');
            if (cb) cb.checked = true;
        });

        const macdItem = subList.querySelector('[data-value="macd"]');
        if (macdItem) {
            this.activeSubIndicator = 'macd';
            macdItem.classList.add('active');
        } else if (subList.firstElementChild) {
            this.activeSubIndicator = subList.firstElementChild.dataset.value;
            subList.firstElementChild.classList.add('active');
        }

        this.updateOverlayPickerLabel();
        this.updateSubPickerLabel();
    }

    updateOverlayPickerLabel() {
        const btn = document.getElementById('overlay-picker-btn');
        if (!btn) return;
        const names = this.selectedOverlayNames();
        if (!names.length) {
            btn.textContent = '选择指标';
            return;
        }
        if (names.length === 1) {
            const ind = window.IndicatorCatalog ? IndicatorCatalog.find(names[0]) : null;
            btn.textContent = ind ? (ind.full_name || ind.name) : names[0];
            return;
        }
        btn.textContent = names.length + ' 个指标';
    }

    updateSubPickerLabel() {
        const btn = document.getElementById('sub-picker-btn');
        if (!btn) return;
        const ind = window.IndicatorCatalog ? IndicatorCatalog.find(this.activeSubIndicator) : null;
        btn.textContent = ind ? (ind.full_name || ind.name) : (this.activeSubIndicator || '副图指标');
    }

    closeIndicatorPickers(exceptId) {
        document.querySelectorAll('.indicator-picker-panel.open').forEach(function (panel) {
            if (!exceptId || panel.id !== exceptId) {
                panel.classList.remove('open');
            }
        });
    }

    initIndicatorPickers() {
        const self = this;
        const overlayBtn = document.getElementById('overlay-picker-btn');
        const overlayPanel = document.getElementById('overlay-picker-panel');
        const overlayList = document.getElementById('overlay-picker-list');
        const subBtn = document.getElementById('sub-picker-btn');
        const subPanel = document.getElementById('sub-picker-panel');
        const subList = document.getElementById('sub-picker-list');

        if (overlayBtn && overlayPanel) {
            overlayBtn.addEventListener('click', function (e) {
                e.stopPropagation();
                const open = overlayPanel.classList.contains('open');
                self.closeIndicatorPickers();
                if (!open) overlayPanel.classList.add('open');
            });
        }

        if (overlayList) {
            overlayList.addEventListener('change', function () {
                self.updateOverlayPickerLabel();
                self.reloadOverlays();
            });
            overlayList.addEventListener('click', function (e) {
                e.stopPropagation();
            });
        }

        if (subBtn && subPanel) {
            subBtn.addEventListener('click', function (e) {
                e.stopPropagation();
                const open = subPanel.classList.contains('open');
                self.closeIndicatorPickers();
                if (!open) subPanel.classList.add('open');
            });
        }

        if (subList) {
            subList.addEventListener('click', function (e) {
                e.stopPropagation();
                const item = e.target.closest('.indicator-picker-item');
                if (!item || !item.dataset.value) return;
                subList.querySelectorAll('.indicator-picker-item').forEach(function (el) {
                    el.classList.remove('active');
                });
                item.classList.add('active');
                self.activeSubIndicator = item.dataset.value;
                self.updateSubPickerLabel();
                self.updateSubChartTitle();
                self.closeIndicatorPickers();
                self.reloadSubIndicator();
            });
        }

        document.addEventListener('click', function () {
            self.closeIndicatorPickers();
        });
    }

    initTickStream() {
        const self = this;
        const b = this.bridge();
        if (!b || !b.Tick || !b.Tick.onTick) {
            return;
        }
        b.Tick.onTick(function (tick) {
            if (!self.useApi || !tick || !self.mainChart) {
                return;
            }
            const inst = String(tick.instrument_id || '').toLowerCase();
            if (inst !== self.instrumentId.toLowerCase()) {
                return;
            }
            const tickResult = self.mainChart.applyTick(tick);
            if (tickResult && tickResult.updated) {
                self.liveTickCount += 1;
                self.setStatus(self.instrumentId + ' · ' + self.period + ' · 实时 Tick', false);
            }
        });
    }

    initCharts() {
        this.mainChart = new MarketChart('main-chart');
        this.mainChart.init();

        this.macdChart = new IndicatorSubChart('macd-chart');
        this.macdChart.init();

        this.setupChartZoomLink();
    }

    setupChartZoomLink() {
        const main = this.mainChart && this.mainChart.chart;
        const sub = this.macdChart && this.macdChart.chart;
        if (!main || !sub) return;

        if (this._zoomSyncBound) return;
        this._zoomSyncBound = true;
        this._syncingZoom = false;

        const self = this;
        const bindZoom = function (source, target) {
            source.on('datazoom', function (ev) {
                if (self._syncingZoom) return;
                const payload = (ev.batch && ev.batch.length) ? ev.batch[0] : ev;
                if (payload.start == null || payload.end == null) return;
                if (payload.start >= payload.end) return;
                self._syncingZoom = true;
                target.dispatchAction({
                    type: 'dataZoom',
                    start: payload.start,
                    end: payload.end
                });
                self._syncingZoom = false;
                self.applyVisibleYAxis(payload.start, payload.end);
            });
        };
        bindZoom(main, sub);
        bindZoom(sub, main);
    }

    applyVisibleYAxis(startPct, endPct) {
        if (this.mainChart) {
            this.mainChart.adjustYAxisToVisibleRange(startPct, endPct);
        }
        if (this.macdChart) {
            this.macdChart.adjustYAxisToVisibleRange(startPct, endPct);
        }
    }

    syncChartsZoom() {
        if (!this.mainChart || !this.macdChart) return;
        const range = this.mainChart.getDataZoomRange();
        if (!range) return;
        this._syncingZoom = true;
        this.macdChart.setDataZoomRange(range);
        this._syncingZoom = false;
        this.applyVisibleYAxis(range.start, range.end);
    }

    initInstrumentSync() {
        const self = this;
        try {
            window.parent.addEventListener('quant-sev-chart-instrument', function (ev) {
                if (ev.detail) self.setInstrument(ev.detail);
            });
        } catch (e) { /* ignore */ }
    }

    setInstrument(instrumentId) {
        if (!instrumentId) return;
        this.instrumentId = String(instrumentId).toLowerCase();
        const input = document.getElementById('instrument-input');
        if (input) input.value = this.instrumentId;
        try {
            localStorage.setItem('quant_sev_chart_instrument', this.instrumentId);
        } catch (e) { /* ignore */ }
        if (this.domPanel) {
            this.domPanel.setInstrument(this.instrumentId);
        }
        this.reloadMarketData();
    }

    initToolbar() {
        const self = this;

        document.getElementById('timeframe-select').addEventListener('change', function (e) {
            self.period = e.target.value;
            self.reloadMarketData();
        });

        document.getElementById('instrument-input').addEventListener('change', function () {
            self.setInstrument(self.resolveInstrumentId());
        });
        document.getElementById('instrument-input').addEventListener('keydown', function (e) {
            if (e.key === 'Enter') self.setInstrument(self.resolveInstrumentId());
        });

        document.querySelectorAll('[data-sub]').forEach(function (btn) {
            btn.addEventListener('click', function (e) {
                e.target.classList.toggle('active');
                self.toggleSubpage(e.target.dataset.sub);
            });
        });

        document.getElementById('add-subpage-btn').addEventListener('click', function (e) {
            self.showContextMenu(e.target);
        });

        document.getElementById('reset-layout-btn').addEventListener('click', function () {
            self.resetLayout();
        });
    }

    selectedOverlayNames() {
        const overlayList = document.getElementById('overlay-picker-list');
        if (!overlayList) return [];
        return Array.from(overlayList.querySelectorAll('input[type="checkbox"]:checked')).map(function (cb) {
            return cb.value;
        });
    }

    indicatorOptionsString(name) {
        if (!window.IndicatorCatalog) return '';
        const ind = IndicatorCatalog.find(name);
        return ind ? IndicatorCatalog.defaultOptions(ind) : '';
    }

    updateSubChartTitle() {
        const title = document.getElementById('sub-chart-title');
        if (!title) return;
        const ind = window.IndicatorCatalog ? IndicatorCatalog.find(this.activeSubIndicator) : null;
        const label = ind ? (ind.full_name || ind.name) : this.activeSubIndicator;
        title.textContent = '📊 副图 - ' + label;
    }

    reloadOverlays() {
        const self = this;
        const tv = this.tradeView();
        if (!tv || !tv.loadIndicator || !this.useApi || !this.mainChart) {
            return Promise.resolve();
        }
        const names = this.selectedOverlayNames();
        if (!names.length) {
            this.mainChart.applyOverlays([]);
            return Promise.resolve();
        }
        const tasks = names.map(function (name) {
            const options = self.indicatorOptionsString(name);
            return tv.loadIndicator(self.instrumentId, name, self.period, options, 500).then(function (res) {
                if (res && res.error) {
                    throw new Error(name + ': ' + res.error);
                }
                return { name: name, outputs: (res && res.outputs) || {} };
            });
        });
        return Promise.allSettled(tasks).then(function (results) {
            const layers = [];
            const errors = [];
            results.forEach(function (r, i) {
                if (r.status === 'fulfilled') {
                    layers.push(r.value);
                } else {
                    errors.push(names[i] + ': ' + (r.reason && r.reason.message ? r.reason.message : r.reason));
                }
            });
            self.mainChart.applyOverlays(layers);
            if (errors.length) {
                self.setStatus('部分叠加失败: ' + errors.join('; '), true);
            }
        });
    }

    reloadMarketData() {
        const self = this;
        const tv = this.tradeView();
        this.instrumentId = this.resolveInstrumentId();
        this.period = document.getElementById('timeframe-select').value || this.period;

        if (!tv || !tv.loadBars) {
            this.useApi = false;
            this.setStatus('演示数据（Bridge 未加载）', true);
            this.mainChart.loadMockData();
            this.macdChart.loadMockIndicator(this.activeSubIndicator || 'macd');
            return;
        }

        this.setStatus('加载 K 线…', false);
        this.liveTickCount = 0;
        tv.loadBars(this.instrumentId, this.period, 500).then(function (barsRes) {
            if (barsRes && barsRes.error) {
                throw new Error(barsRes.error);
            }
            const ok = self.mainChart.applyBars(barsRes, {
                instrumentId: self.instrumentId,
                period: self.period
            });
            if (!ok) {
                throw new Error('无 Bar 数据: ' + self.instrumentId + ' ' + self.period);
            }
            self.useApi = true;
            self.setStatus(self.instrumentId + ' · ' + self.period + ' · Storage + WS', false);
            return Promise.all([
                self.reloadOverlays(),
                self.reloadSubIndicator()
            ]).then(function () {
                requestAnimationFrame(function () {
                    self.syncChartsZoom();
                });
            });
        }).catch(function (err) {
            self.useApi = false;
            self.setStatus('API 失败: ' + err.message + '，使用演示数据', true);
            self.mainChart.loadMockData();
            self.macdChart.loadMockIndicator(self.activeSubIndicator || 'macd');
        });
    }

    reloadSubIndicator() {
        const self = this;
        const tv = this.tradeView();
        if (!this.activeSubIndicator) {
            this.activeSubIndicator = 'macd';
        }
        if (!tv || !tv.loadIndicator || !this.useApi) {
            return Promise.resolve();
        }

        const name = this.activeSubIndicator || 'macd';
        const options = this.indicatorOptionsString(name);
        this.updateSubChartTitle();
        this.setStatus('加载 ' + name + '…', false);

        return tv.loadIndicator(this.instrumentId, name, this.period, options, 500).then(function (indRes) {
            if (indRes && indRes.error) {
                throw new Error(indRes.error);
            }
            self.macdChart.applyIndicator(indRes);
            self.syncChartsZoom();
            self.setStatus(self.instrumentId + ' · ' + self.period + ' · ' + name, false);
        }).catch(function (err) {
            self.setStatus('指标失败: ' + err.message, true);
            self.macdChart.loadMockIndicator(name);
            self.syncChartsZoom();
        });
    }

    toggleSubpage(type) {
        const config = this.subpageConfig[type];
        if (!config) return;

        config.visible = !config.visible;
        const panel = document.getElementById(config.id);

        if (panel) {
            panel.style.display = config.visible ? 'flex' : 'none';
            if (config.visible) {
                panel.style.flex = config.height + ' 1 0';
            }
        }

        if (type === 'chip' && config.visible && !this.chipChart) {
            this.chipChart = new ChipChart('chip-chart');
            this.chipChart.init();
        }

        if (type === 'dom' && config.visible && !this.domPanel) {
            this.domPanel = new DOMPanel('dom-content');
            this.domPanel.init();
        }

        this.resizeCharts();
    }

    initContextMenu() {
        const self = this;
        const menu = document.getElementById('context-menu');

        document.addEventListener('contextmenu', function (e) {
            e.preventDefault();
            menu.style.display = 'block';
            menu.style.left = e.pageX + 'px';
            menu.style.top = e.pageY + 'px';
        });

        document.addEventListener('click', function () {
            menu.style.display = 'none';
        });

        menu.querySelectorAll('.context-menu-item').forEach(function (item) {
            item.addEventListener('click', function (e) {
                self.handleContextMenuAction(e.target.dataset.action);
                menu.style.display = 'none';
            });
        });
    }

    handleContextMenuAction(action) {
        switch (action) {
            case 'add-dom':
                this.subpageConfig.dom.visible = true;
                document.getElementById('dom-panel').style.display = 'flex';
                document.getElementById('dom-panel').style.flex = '15 1 0';
                if (!this.domPanel) {
                    this.domPanel = new DOMPanel('dom-content');
                    this.domPanel.init();
                }
                this.resizeCharts();
                break;

            case 'add-chip':
                this.subpageConfig.chip.visible = true;
                document.getElementById('chip-panel').style.display = 'flex';
                document.getElementById('chip-panel').style.flex = '15 1 0';
                if (!this.chipChart) {
                    this.chipChart = new ChipChart('chip-chart');
                    this.chipChart.init();
                }
                this.resizeCharts();
                break;

            case 'reset':
                this.resetLayout();
                break;
        }
    }

    showContextMenu(element) {
        const menu = document.getElementById('context-menu');
        const rect = element.getBoundingClientRect();
        menu.style.display = 'block';
        menu.style.left = rect.left + 'px';
        menu.style.top = rect.bottom + 'px';
    }

    resetLayout() {
        Object.values(this.subpageConfig).forEach(function (config) {
            config.visible = config.id === 'main-chart-panel' || config.id === 'sub-chart-macd';
            const panel = document.getElementById(config.id);

            if (panel) {
                panel.style.display = config.visible ? 'flex' : 'none';
                if (config.visible) {
                    panel.style.flex = config.height + ' 1 0';
                }
            }
        });

        this.resizeCharts();
    }

    initResizeHandles() {
        const self = this;
        document.querySelectorAll('.resize-handle').forEach(function (handle) {
            let startY, startHeight, panel;

            handle.addEventListener('mousedown', function (e) {
                e.preventDefault();
                startY = e.clientY;
                panel = document.getElementById(handle.dataset.panel);
                startHeight = panel.offsetHeight;

                function onMouseMove(ev) {
                    const delta = ev.clientY - startY;
                    const newHeight = Math.max(50, startHeight + delta);
                    panel.style.flex = newHeight + 'px 0 0';
                    self.resizeCharts();
                }

                function onMouseUp() {
                    document.removeEventListener('mousemove', onMouseMove);
                    document.removeEventListener('mouseup', onMouseUp);
                }

                document.addEventListener('mousemove', onMouseMove);
                document.addEventListener('mouseup', onMouseUp);
            });
        });
    }

    initSubpageActions() {
        const self = this;
        document.querySelectorAll('.subpage-action-btn').forEach(function (btn) {
            btn.addEventListener('click', function (e) {
                const action = e.target.dataset.action;
                const panel = e.target.closest('.subpage-panel');
                const panelId = panel.id;

                switch (action) {
                    case 'close':
                        panel.style.display = 'none';
                        self.resizeCharts();
                        break;

                    case 'maximize':
                        document.querySelectorAll('.subpage-panel').forEach(function (p) {
                            if (p.id !== panelId) p.style.display = 'none';
                        });
                        panel.style.flex = '1 1 auto';
                        self.resizeCharts();
                        break;
                }
            });
        });
    }

    resizeCharts() {
        if (this.mainChart) this.mainChart.resize();
        if (this.macdChart) this.macdChart.resize();
        if (this.chipChart) this.chipChart.resize();
    }

    dispose() {
        if (this.mainChart) this.mainChart.dispose();
        if (this.macdChart) this.macdChart.dispose();
        if (this.chipChart) this.chipChart.dispose();
        if (this.domPanel) this.domPanel.dispose();
    }
}

document.addEventListener('DOMContentLoaded', function () {
    const app = new MarketChartApp();
    app.init();
    window.marketChartApp = app;

    window.addEventListener('resize', function () {
        app.resizeCharts();
    });
});
