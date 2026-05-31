class MarketChart {
    constructor(containerId) {
        this.containerId = containerId;
        this.chart = null;
        this.data = null;
        this.option = null;
        this.barsMeta = [];
        this.instrumentId = '';
        this.period = 'm1';
        this.lastCumulativeVolume = null;
        this.overlayLayers = [];
    }

    static formatTradingDate(tradingDay) {
        if (!tradingDay || tradingDay.length !== 8) return tradingDay || '';
        return tradingDay.substr(0, 4) + '/' + tradingDay.substr(4, 2) + '/' + tradingDay.substr(6, 2);
    }

    static barDateTimeLabel(date, time) {
        return String(date || '').replace(/\//g, '-') + ' ' + (time || '');
    }

    static parseHms(time) {
        if (!time || time.length < 5) return null;
        const hour = parseInt(time.substr(0, 2), 10);
        const minute = parseInt(time.substr(3, 2), 10);
        const second = time.length >= 8 ? parseInt(time.substr(6, 2), 10) : 0;
        if (isNaN(hour) || isNaN(minute)) return null;
        return { hour, minute, second: isNaN(second) ? 0 : second };
    }

    static formatHms(hour, minute, second) {
        const pad = function (n) { return n < 10 ? '0' + n : String(n); };
        return pad(hour) + ':' + pad(minute) + ':' + pad(second || 0);
    }

    static m1TimeFloor(time) {
        if (!time || time.length < 5) return time || '00:00:00';
        return time.substr(0, 5) + ':00';
    }

    static m15BucketKey(date, time) {
        const hms = MarketChart.parseHms(time);
        if (!hms) return date + ' ' + time;
        const minute = Math.floor(hms.minute / 15) * 15;
        return date + ' ' + MarketChart.formatHms(hms.hour, minute, 0);
    }

    static h1LabelForM15(m15Time) {
        const hms = MarketChart.parseHms(m15Time);
        if (!hms) return m15Time;
        const minutes = hms.hour * 60 + hms.minute;
        const slots = [
            [21 * 60, 21 * 60 + 45, 21, 0],
            [22 * 60, 22 * 60 + 45, 22, 0],
            [23 * 60, 23 * 60 + 45, 23, 0],
            [0, 45, 0, 0],
            [1 * 60, 1 * 60 + 45, 1, 0],
            [2 * 60, 2 * 60 + 45, 2, 0],
            [9 * 60, 9 * 60 + 45, 9, 0],
            [10 * 60, 11 * 60, 10, 0],
            [11 * 60 + 15, 14 * 60, 11, 15],
            [14 * 60 + 15, 14 * 60 + 45, 14, 15]
        ];
        for (let i = 0; i < slots.length; i++) {
            const s = slots[i];
            if (minutes >= s[0] && minutes <= s[1]) {
                return MarketChart.formatHms(s[2], s[3], 0);
            }
        }
        return MarketChart.formatHms(Math.floor(minutes / 60), 0, 0);
    }

    static bucketFromTick(tick, period) {
        const date = MarketChart.formatTradingDate(tick.trading_day || '');
        const m1Time = MarketChart.m1TimeFloor(tick.update_time || '00:00:00');
        let time = m1Time;
        if (period === 'd1') {
            time = '00:00:00';
        } else if (period === 'm15') {
            time = MarketChart.m15BucketKey(date, m1Time).split(' ')[1];
        } else if (period === 'h1') {
            const m15Time = MarketChart.m15BucketKey(date, m1Time).split(' ')[1];
            time = MarketChart.h1LabelForM15(m15Time);
        }
        return {
            date: date,
            time: time,
            label: MarketChart.barDateTimeLabel(date, time)
        };
    }

    static bucketKey(meta, period) {
        if (!meta) return '';
        if (period === 'd1') return String(meta.date || '').replace(/\//g, '');
        return String(meta.date || '').replace(/\//g, '') + '|' + (meta.time || '');
    }

    static sortKey(meta, period) {
        return MarketChart.bucketKey(meta, period);
    }

    init() {
        this.chart = echarts.init(document.getElementById(this.containerId), 'dark');
        this.chart.group = 'quant-sev-market';
        this.bindEvents();
    }

    getDataZoomRange() {
        if (!this.chart) return null;
        const option = this.chart.getOption();
        const zooms = option.dataZoom || [];
        for (let i = 0; i < zooms.length; i++) {
            const dz = zooms[i];
            if (dz.start != null && dz.end != null) {
                return { start: dz.start, end: dz.end };
            }
        }
        return null;
    }

    setDataZoomRange(range) {
        if (!this.chart || !range) return;
        if (range.start == null || range.end == null || range.start >= range.end) return;
        this.chart.dispatchAction({
            type: 'dataZoom',
            start: range.start,
            end: range.end
        });
    }

    chartPatchOption(partial, replaceSeries) {
        if (!this.chart) return;
        const opts = replaceSeries ? { replaceMerge: ['series'] } : undefined;
        this.chart.setOption(partial, opts);
    }

    adjustYAxisToVisibleRange(startPct, endPct) {
        if (!this.chart || !this.data || !window.ChartAxisUtil) return;
        const util = window.ChartAxisUtil;
        const zoom = this.getDataZoomRange();
        const start = startPct != null ? startPct : (zoom ? zoom.start : 0);
        const end = endPct != null ? endPct : (zoom ? zoom.end : 100);
        const idx = util.visibleIndexRange(this.data.dates.length, start, end);

        let priceRange = null;
        for (let i = idx.start; i < idx.end; i++) {
            const k = this.data.klineData[i];
            if (!k) continue;
            priceRange = util.extendRange(priceRange, k[2]);
            priceRange = util.extendRange(priceRange, k[3]);
        }
        (this.overlayLayers || []).forEach(function (layer) {
            const outputs = layer.outputs || {};
            Object.keys(outputs).forEach(function (key) {
                const arr = outputs[key] || [];
                for (let j = idx.start; j < idx.end; j++) {
                    priceRange = util.extendRange(priceRange, arr[j]);
                }
            });
        });

        const padded = util.padRange(priceRange);
        if (!padded) return;

        let volMax = 0;
        for (let vi = idx.start; vi < idx.end; vi++) {
            const vol = this.data.volumes[vi];
            if (vol && vol[1] > volMax) volMax = vol[1];
        }

        this.chart.setOption({
            yAxis: [
                { min: padded.min, max: padded.max, scale: true },
                { min: 0, max: volMax > 0 ? volMax * 1.15 : 1, scale: false }
            ]
        });
    }

    applyBars(response, options) {
        const bars = (response && response.bars) || [];
        if (!bars.length) {
            this.data = null;
            this.barsMeta = [];
            return false;
        }

        const dates = [];
        const klineData = [];
        const volumes = [];
        const barsMeta = [];

        bars.forEach(function (bar, i) {
            const dt = String(bar.date || '').replace(/\//g, '-') + ' ' + (bar.time || '');
            dates.push(dt.trim());
            klineData.push([bar.open, bar.close, bar.low, bar.high]);
            const up = Number(bar.close) >= Number(bar.open);
            volumes.push([i, bar.volume || 0, up ? 1 : -1]);
            barsMeta.push({ date: bar.date, time: bar.time });
        });

        this.data = { dates: dates, klineData: klineData, volumes: volumes };
        this.barsMeta = barsMeta;
        this.instrumentId = (options && options.instrumentId) || '';
        this.period = (options && options.period) || 'm1';
        this.lastCumulativeVolume = null;
        if (!this.option) {
            this.setOption();
        } else {
            this.updateData();
        }
        return true;
    }

    applyTick(tick) {
        if (!this.data || !this.barsMeta.length || !tick) {
            return { updated: false };
        }
        const price = Number(tick.last_price);
        if (!price || isNaN(price)) {
            return { updated: false };
        }

        const tickBucket = MarketChart.bucketFromTick(tick, this.period);
        const lastIdx = this.barsMeta.length - 1;
        const lastMeta = this.barsMeta[lastIdx];
        const tickKey = MarketChart.bucketKey(tickBucket, this.period);
        const lastKey = MarketChart.bucketKey(lastMeta, this.period);

        if (tickKey === lastKey) {
            const k = this.data.klineData[lastIdx];
            k[1] = price;
            k[2] = Math.min(k[2], price);
            k[3] = Math.max(k[3], price);
            const cumVol = Number(tick.volume);
            if (!isNaN(cumVol)) {
                if (this.lastCumulativeVolume != null && cumVol >= this.lastCumulativeVolume) {
                    this.data.volumes[lastIdx][1] += cumVol - this.lastCumulativeVolume;
                }
                this.lastCumulativeVolume = cumVol;
            }
            this.data.volumes[lastIdx][2] = k[1] >= k[0] ? 1 : -1;
            this.updateData();
            return { updated: true, isNewBar: false, dateLabel: this.data.dates[lastIdx], close: price };
        }

        if (MarketChart.sortKey(tickBucket, this.period) <= MarketChart.sortKey(lastMeta, this.period)) {
            return { updated: false };
        }

        this.data.dates.push(tickBucket.label);
        this.data.klineData.push([price, price, price, price]);
        const idx = this.data.dates.length - 1;
        this.data.volumes.push([idx, 0, 1]);
        this.barsMeta.push({ date: tickBucket.date, time: tickBucket.time });
        this.lastCumulativeVolume = Number(tick.volume);
        if (isNaN(this.lastCumulativeVolume)) {
            this.lastCumulativeVolume = null;
        }

        if (this.data.dates.length > 600) {
            this.data.dates.shift();
            this.data.klineData.shift();
            this.data.volumes.shift();
            this.barsMeta.shift();
            this.data.volumes.forEach(function (v, i) { v[0] = i; });
        }
        this.updateData();
        return {
            updated: true,
            isNewBar: true,
            dateLabel: tickBucket.label,
            close: price
        };
    }

    getCloseSeries() {
        if (!this.data || !this.data.klineData) {
            return null;
        }
        return {
            dates: this.data.dates.slice(),
            closes: this.data.klineData.map(function (k) { return k[1]; })
        };
    }

    loadMockData() {
        const rawData = this.generateKlineData(300);
        this.data = rawData;
        if (!this.option) {
            this.setOption();
        } else {
            this.updateData();
        }
    }

    generateKlineData(count) {
        const dates = [];
        const klineData = [];
        const volumes = [];
        let basePrice = 3600;
        let date = new Date('2023-01-01');

        for (let i = 0; i < count; i++) {
            const open = basePrice + Math.random() * 30 - 15;
            const close = open + Math.random() * 20 - 10;
            const low = Math.min(open, close) - Math.random() * 8;
            const high = Math.max(open, close) + Math.random() * 8;
            const volume = Math.floor(Math.random() * 800 + 200);

            dates.push(date.toISOString().slice(0, 10));
            klineData.push([open, close, low, high]);
            volumes.push([i, volume, close > open ? 1 : -1]);

            basePrice = close;
            date.setDate(date.getDate() + 1);
        }

        return { dates, klineData, volumes };
    }

    calculateMA(dayCount) {
        const result = [];
        const data = this.data.klineData;

        for (let i = 0; i < data.length; i++) {
            if (i < dayCount) {
                result.push('-');
            } else {
                let sum = 0;
                for (let j = 0; j < dayCount; j++) {
                    sum += data[i - j][1];
                }
                result.push(+(sum / dayCount).toFixed(2));
            }
        }
        return result;
    }

    calculateEMA(period) {
        const result = [];
        const data = this.data.klineData;
        let ema = data[0][1];
        const multiplier = 2 / (period + 1);

        for (let i = 0; i < data.length; i++) {
            if (i === 0) {
                result.push(ema);
            } else {
                ema = (data[i][1] - ema) * multiplier + ema;
                result.push(+ema.toFixed(2));
            }
        }
        return result;
    }

    setOption() {
        if (!this.data) {
            return;
        }
        const { dates } = this.data;
        const coreSeries = this.buildCoreSeries();
        const legendNames = coreSeries.map(function (s) { return s.name; });

        this.option = {
            backgroundColor: '#1a1a1a',
            animation: false,
            legend: {
                data: legendNames,
                top: 10,
                left: 'center',
                textStyle: { color: '#95a5a6', fontSize: 11 }
            },
            tooltip: {
                trigger: 'axis',
                axisPointer: { type: 'cross' },
                backgroundColor: 'rgba(44, 62, 80, 0.95)',
                borderColor: '#3498db',
                textStyle: { color: '#e0e0e0', fontSize: 12 },
                formatter: function (params) {
                    const kline = params.find(function (p) { return p.seriesName === 'K线'; });
                    if (!kline) return '';
                    const data = kline.data;
                    return '时间: ' + kline.name + '<br/>' +
                        '开盘: ' + data[0] + '<br/>' +
                        '收盘: ' + data[1] + '<br/>' +
                        '最低: ' + data[2] + '<br/>' +
                        '最高: ' + data[3];
                }
            },
            axisPointer: {
                link: [{ xAxisIndex: 'all' }],
                label: { backgroundColor: '#3498db' }
            },
            grid: [
                { left: '10%', right: '8%', top: '15%', height: '55%' },
                { left: '10%', right: '8%', top: '75%', height: '15%' }
            ],
            xAxis: [
                {
                    type: 'category',
                    data: dates,
                    boundaryGap: false,
                    axisLine: { lineStyle: { color: '#3498db' } },
                    axisLabel: { color: '#95a5a6', fontSize: 10 },
                    splitLine: { show: false },
                    min: 'dataMin',
                    max: 'dataMax'
                },
                {
                    type: 'category',
                    gridIndex: 1,
                    data: dates,
                    boundaryGap: false,
                    axisLine: { lineStyle: { color: '#3498db' } },
                    axisLabel: { show: false },
                    splitLine: { show: false },
                    min: 'dataMin',
                    max: 'dataMax'
                }
            ],
            yAxis: [
                {
                    scale: true,
                    axisLine: { lineStyle: { color: '#3498db' } },
                    axisLabel: { color: '#95a5a6', fontSize: 10 },
                    splitLine: { lineStyle: { color: 'rgba(52, 152, 219, 0.1)' } }
                },
                {
                    scale: true,
                    gridIndex: 1,
                    splitNumber: 2,
                    axisLine: { lineStyle: { color: '#3498db' } },
                    axisLabel: { color: '#95a5a6', fontSize: 10 },
                    splitLine: { lineStyle: { color: 'rgba(52, 152, 219, 0.1)' } }
                }
            ],
            dataZoom: [
                {
                    id: 'syncZoomInside',
                    type: 'inside',
                    xAxisIndex: [0, 1],
                    start: 85,
                    end: 100,
                    filterMode: 'none'
                },
                {
                    id: 'syncZoomSlider',
                    show: true,
                    xAxisIndex: [0, 1],
                    type: 'slider',
                    bottom: '2%',
                    start: 85,
                    end: 100,
                    filterMode: 'none',
                    borderColor: '#3498db',
                    fillerColor: 'rgba(52, 152, 219, 0.2)',
                    handleStyle: { color: '#3498db' },
                    textStyle: { color: '#95a5a6' }
                }
            ],
            series: coreSeries
        };

        this.chart.setOption(this.option);
        this.adjustYAxisToVisibleRange();
    }

    bindEvents() {
        window.addEventListener('resize', () => {
            if (this.chart) this.chart.resize();
        });
    }

    updateData() {
        if (!this.data || !this.chart) return;
        const { dates } = this.data;
        this.chartPatchOption({
            xAxis: [{ data: dates }, { data: dates }],
            series: this.buildCoreSeries()
        }, true);
        this.adjustYAxisToVisibleRange();
    }

    buildOverlaySeries() {
        const catalog = window.IndicatorCatalog;
        const series = [];
        let colorIdx = 0;
        (this.overlayLayers || []).forEach(function (layer) {
            const outputs = layer.outputs || {};
            Object.keys(outputs).forEach(function (key) {
                const label = layer.name + '.' + key;
                const data = outputs[key] || [];
                const color = catalog ? catalog.seriesColor(colorIdx++) : '#f39c12';
                series.push({
                    name: label,
                    type: 'line',
                    data: data,
                    smooth: true,
                    lineStyle: { color: color, width: 1 },
                    symbol: 'none',
                    xAxisIndex: 0,
                    yAxisIndex: 0,
                    z: 8
                });
            });
        });
        return series;
    }

    buildCoreSeries() {
        if (!this.data) return [];
        const { klineData, volumes } = this.data;
        const overlaySeries = this.buildOverlaySeries();
        return [
            {
                name: 'K线',
                type: 'candlestick',
                xAxisIndex: 0,
                yAxisIndex: 0,
                data: klineData,
                z: 10,
                itemStyle: {
                    color: '#e74c3c',
                    color0: '#2ecc71',
                    borderColor: '#e74c3c',
                    borderColor0: '#2ecc71'
                }
            }
        ].concat(overlaySeries).concat([
            {
                name: '成交量',
                type: 'bar',
                xAxisIndex: 1,
                yAxisIndex: 1,
                z: 5,
                data: volumes,
                itemStyle: {
                    color: function (params) {
                        return params.data[2] > 0 ? '#e74c3c' : '#2ecc71';
                    }
                }
            }
        ]);
    }

    applyOverlays(layers) {
        this.overlayLayers = layers || [];
        if (!this.chart || !this.data) return;
        const coreSeries = this.buildCoreSeries();
        const legendNames = coreSeries.map(function (s) { return s.name; });
        this.chartPatchOption({
            legend: { data: legendNames },
            series: coreSeries
        }, true);
        this.adjustYAxisToVisibleRange();
    }

    setIndicator() {
        /* 兼容旧调用；叠加指标请用 applyOverlays */
    }

    getTimes() {
        return this.data ? this.data.dates.slice() : [];
    }

    resize() {
        if (this.chart) this.chart.resize();
    }

    dispose() {
        if (this.chart) {
            this.chart.dispose();
        }
    }
}

window.MarketChart = MarketChart;
