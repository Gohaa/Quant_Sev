class DOMPanel {
    constructor(containerId) {
        this.containerId = containerId;
        this.instrumentId = '';
        this.data = {
            asks: [],
            bids: [],
            spread: 0,
            midPrice: 0,
            lastPrice: 0,
            imbalance: 0,
            bidDepth: 0,
            askDepth: 0,
            updateTime: ''
        };
        this.pollTimer = null;
        this.tickUnsub = null;
    }

    bridge() {
        try {
            if (window.parent && window.parent.QuantSevBridge) {
                return window.parent.QuantSevBridge;
            }
        } catch (e) { /* cross-origin */ }
        return window.QuantSevBridge || null;
    }

    resolveInstrument() {
        try {
            var stored = localStorage.getItem('quant_sev_chart_instrument') ||
                localStorage.getItem('quant_sev_last_contract');
            if (stored) return String(stored).toLowerCase();
        } catch (e) { /* ignore */ }
        return 'rb2610';
    }

    init() {
        this.instrumentId = this.resolveInstrument();
        this.render();
        this.initTickStream();
        this.initInstrumentSync();
        this.fetchSnapshot();
        this.pollTimer = setInterval(() => this.fetchSnapshot(), 1500);
        var self = this;
        window.addEventListener('storage', function (e) {
            if (e.key === 'quant_sev_chart_instrument' || e.key === 'quant_sev_last_contract') {
                self.setInstrument(self.resolveInstrument());
            }
        });
    }

    initInstrumentSync() {
        var self = this;
        try {
            if (window.parent && window.parent !== window) {
                window.parent.addEventListener('quant-sev-chart-instrument', function (ev) {
                    if (ev && ev.detail) self.setInstrument(ev.detail);
                });
            }
        } catch (e) { /* ignore */ }
        window.addEventListener('message', function (ev) {
            var data = ev.data;
            if (data && data.type === 'quant-sev-set-contract' && data.instrument_id) {
                self.setInstrument(data.instrument_id);
            }
        });
    }

    setInstrument(instrumentId) {
        if (!instrumentId) return;
        var next = String(instrumentId).toLowerCase();
        if (next === this.instrumentId) return;
        this.instrumentId = next;
        this.render();
        this.fetchSnapshot();
    }

    initTickStream() {
        var self = this;
        var b = this.bridge();
        if (!b || !b.Tick || !b.Tick.onTick) {
            return;
        }
        if (this.tickUnsub) {
            this.tickUnsub();
        }
        this.tickUnsub = b.Tick.onTick(function (tick) {
            if (!tick || !tick.instrument_id) return;
            if (String(tick.instrument_id).toLowerCase() !== self.instrumentId) return;
            self.applyTick(tick);
        });
    }

    fetchSnapshot() {
        var self = this;
        var b = this.bridge();
        if (!b || !b.OrderBook || !this.instrumentId) return;
        b.OrderBook.getSnapshot(this.instrumentId, 5).then(function (snap) {
            if (snap && !snap.error) {
                self.applyApiSnapshot(snap);
            }
        }).catch(function () { /* ignore */ });
    }

    applyApiSnapshot(snap) {
        var asks = [];
        var bids = [];
        var i;
        var apiAsks = snap.asks || [];
        var apiBids = snap.bids || [];
        for (i = apiAsks.length - 1; i >= 0; i--) {
            asks.push({
                level: '卖' + (i + 1),
                price: apiAsks[i].price,
                volume: apiAsks[i].volume
            });
        }
        for (i = 0; i < apiBids.length; i++) {
            bids.push({
                level: '买' + (i + 1),
                price: apiBids[i].price,
                volume: apiBids[i].volume
            });
        }
        this.data = {
            asks: asks,
            bids: bids,
            spread: snap.spread || 0,
            midPrice: snap.mid_price || 0,
            lastPrice: snap.last_price || 0,
            imbalance: snap.imbalance || 0,
            bidDepth: snap.bid_depth || 0,
            askDepth: snap.ask_depth || 0,
            updateTime: snap.update_time || ''
        };
        this.render();
    }

    applyTick(tick) {
        var asks = [];
        var bids = [];
        var i;
        for (i = 5; i >= 1; i--) {
            asks.push({
                level: '卖' + i,
                price: tick['ask_price' + i] || 0,
                volume: tick['ask_volume' + i] || 0
            });
        }
        for (i = 1; i <= 5; i++) {
            bids.push({
                level: '买' + i,
                price: tick['bid_price' + i] || 0,
                volume: tick['bid_volume' + i] || 0
            });
        }
        var bid1 = tick.bid_price1 || 0;
        var ask1 = tick.ask_price1 || 0;
        var spread = (bid1 > 0 && ask1 > 0) ? ask1 - bid1 : 0;
        var mid = (bid1 > 0 && ask1 > 0) ? (bid1 + ask1) * 0.5 : (tick.last_price || 0);
        var bidDepth = 0;
        var askDepth = 0;
        for (i = 1; i <= 5; i++) {
            bidDepth += tick['bid_volume' + i] || 0;
            askDepth += tick['ask_volume' + i] || 0;
        }
        var total = bidDepth + askDepth;
        var imbalance = total > 0 ? (bidDepth - askDepth) / total : 0;
        this.data = {
            asks: asks,
            bids: bids,
            spread: spread,
            midPrice: mid,
            lastPrice: tick.last_price || 0,
            imbalance: imbalance,
            bidDepth: bidDepth,
            askDepth: askDepth,
            updateTime: tick.update_time || ''
        };
        this.render();
    }

    formatPrice(v) {
        if (!v || v <= 0) return '—';
        return Number(v).toFixed(2);
    }

    render() {
        var container = document.getElementById(this.containerId);
        if (!container) return;

        var asks = this.data.asks || [];
        var bids = this.data.bids || [];
        var maxVolume = 1;
        asks.forEach(function (a) { if (a.volume > maxVolume) maxVolume = a.volume; });
        bids.forEach(function (b) { if (b.volume > maxVolume) maxVolume = b.volume; });

        var html = '<div class="dom-header">' +
            '<span class="dom-inst">' + (this.instrumentId || '—') + '</span>' +
            '<span class="dom-last">最新 ' + this.formatPrice(this.data.lastPrice) + '</span>' +
            '</div><table class="dom-table"><thead><tr>' +
            '<th>档位</th><th>价格</th><th>数量</th></tr></thead><tbody>';

        asks.forEach(function (ask) {
            var barWidth = maxVolume > 0 ? (ask.volume / maxVolume) * 60 : 0;
            html += '<tr class="dom-ask"><td>' + ask.level + '</td><td>' + this.formatPrice(ask.price) +
                '</td><td>' + (ask.volume || 0) +
                '<span class="dom-bar dom-bar-ask" style="width:' + barWidth + 'px;"></span></td></tr>';
        });

        html += '<tr class="dom-spread-row"><td colspan="3">价差 ' + this.formatPrice(this.data.spread) +
            ' · 中间价 ' + this.formatPrice(this.data.midPrice) +
            ' · 失衡 ' + (this.data.imbalance * 100).toFixed(1) + '%' +
            (this.data.updateTime ? ' · ' + this.data.updateTime : '') + '</td></tr>';

        bids.forEach(function (bid) {
            var barWidth = maxVolume > 0 ? (bid.volume / maxVolume) * 60 : 0;
            html += '<tr class="dom-bid"><td>' + bid.level + '</td><td>' + this.formatPrice(bid.price) +
                '</td><td>' + (bid.volume || 0) +
                '<span class="dom-bar dom-bar-bid" style="width:' + barWidth + 'px;"></span></td></tr>';
        });

        html += '</tbody></table>';
        html += '<div class="dom-footer">买深 ' + this.data.bidDepth + ' · 卖深 ' + this.data.askDepth + '</div>';
        container.innerHTML = html;
    }

    refresh() {
        this.fetchSnapshot();
    }

    dispose() {
        if (this.pollTimer) {
            clearInterval(this.pollTimer);
            this.pollTimer = null;
        }
        if (this.tickUnsub) {
            this.tickUnsub();
            this.tickUnsub = null;
        }
    }
}

window.DOMPanel = DOMPanel;
