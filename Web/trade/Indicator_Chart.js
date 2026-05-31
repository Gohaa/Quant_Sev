class IndicatorSubChart {
    constructor(containerId) {
        this.containerId = containerId;
        this.chart = null;
        this.indicatorName = '';
        this.times = [];
        this.outputs = {};
        this.outputKeys = [];
    }

    init() {
        this.chart = echarts.init(document.getElementById(this.containerId), 'dark');
        this.chart.group = 'quant-sev-market';
        this.bindEvents();
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

    adjustYAxisToVisibleRange(startPct, endPct) {
        if (!this.chart || !this.times.length || !window.ChartAxisUtil) return;
        const util = window.ChartAxisUtil;
        const zoom = this.getDataZoomRange();
        const start = startPct != null ? startPct : (zoom ? zoom.start : 0);
        const end = endPct != null ? endPct : (zoom ? zoom.end : 100);
        const idx = util.visibleIndexRange(this.times.length, start, end);

        let valueRange = null;
        const self = this;
        this.outputKeys.forEach(function (key) {
            const arr = self.outputs[key] || [];
            for (let i = idx.start; i < idx.end; i++) {
                valueRange = util.extendRange(valueRange, arr[i]);
            }
        });

        const padded = util.padRange(valueRange);
        if (!padded) return;

        this.chart.setOption({
            yAxis: {
                min: padded.min,
                max: padded.max,
                scale: true
            }
        });
    }

    applyIndicator(response) {
        if (!response || response.error) {
            return;
        }
        this.indicatorName = response.indicator || response.full_name || 'indicator';
        this.times = (response.times || []).slice();
        this.outputs = response.outputs || {};
        this.outputKeys = Object.keys(this.outputs);
        this.render();
    }

    render() {
        if (!this.chart || !this.times.length || !this.outputKeys.length) {
            return;
        }

        var catalog = window.IndicatorCatalog;
        var legend = [];
        var series = [];
        var self = this;

        this.outputKeys.forEach(function (key, idx) {
            var label = self.indicatorName + '.' + key;
            legend.push(label);
            var data = self.outputs[key] || [];
            var color = catalog ? catalog.seriesColor(idx) : '#3498db';
            if (catalog && catalog.isBarOutput(key)) {
                series.push({
                    name: label,
                    type: 'bar',
                    data: data,
                    itemStyle: {
                        color: function (params) {
                            var v = params.value;
                            if (v == null || isNaN(v)) return '#95a5a6';
                            return v >= 0 ? '#e74c3c' : '#2ecc71';
                        }
                    }
                });
            } else {
                series.push({
                    name: label,
                    type: 'line',
                    data: data,
                    lineStyle: { color: color, width: 1.2 },
                    symbol: 'none',
                    smooth: true
                });
            }
        });

        var option = {
            backgroundColor: '#1a1a1a',
            animation: false,
            legend: {
                data: legend,
                top: 5,
                left: 'center',
                textStyle: { color: '#95a5a6', fontSize: 10 }
            },
            tooltip: { trigger: 'axis' },
            grid: { left: '10%', right: '8%', top: '20%', bottom: '15%' },
            xAxis: {
                type: 'category',
                data: this.times,
                boundaryGap: false,
                axisLine: { lineStyle: { color: '#3498db' } },
                axisLabel: { show: false },
                splitLine: { show: false },
                min: 'dataMin',
                max: 'dataMax'
            },
            yAxis: {
                scale: true,
                axisLine: { lineStyle: { color: '#3498db' } },
                axisLabel: { color: '#95a5a6', fontSize: 10 },
                splitLine: { lineStyle: { color: 'rgba(52, 152, 219, 0.1)' } }
            },
            dataZoom: [
                {
                    id: 'syncZoomInside',
                    type: 'inside',
                    start: 85,
                    end: 100,
                    filterMode: 'none'
                }
            ],
            series: series
        };

        this.chart.setOption(option, true);
        this.adjustYAxisToVisibleRange();
    }

    loadMockIndicator(name) {
        var mockTimes = [];
        var mockSeries = [];
        for (var i = 0; i < 120; i++) {
            mockTimes.push('2024-01-' + String((i % 28) + 1).padStart(2, '0'));
            mockSeries.push(+(Math.sin(i / 8) * 20 + 50 + Math.random()).toFixed(2));
        }
        this.indicatorName = name || 'demo';
        this.times = mockTimes;
        this.outputs = { value: mockSeries };
        this.outputKeys = ['value'];
        this.render();
    }

    attachLiveFromMain() {
        /* 通用 Tulip 指标由服务端计算，Tick 时不做客户端递推 */
    }

    updateLive() {
        return false;
    }

    bindEvents() {
        window.addEventListener('resize', () => {
            if (this.chart) this.chart.resize();
        });
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

window.IndicatorSubChart = IndicatorSubChart;
window.MACDChart = IndicatorSubChart;
