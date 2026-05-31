class ChipChart {
    constructor(containerId) {
        this.containerId = containerId;
        this.chart = null;
        this.data = null;
        this.option = null;
    }

    init() {
        this.chart = echarts.init(document.getElementById(this.containerId), 'dark');
        this.loadData();
        this.setOption();
        this.bindEvents();
    }

    loadData() {
        this.data = this.generateChipData(50);
    }

    generateChipData(count) {
        const chipData = [];
        let basePrice = 3600;

        for (let i = 0; i < count; i++) {
            const price = basePrice + i * 2;
            const volume = Math.floor(Math.random() * 1000 + 100);
            chipData.push({
                price: price.toFixed(0),
                volume: volume
            });
        }

        return chipData;
    }

    setOption() {
        const prices = this.data.map(d => d.price);
        const volumes = this.data.map(d => d.volume);
        const maxVolume = Math.max(...volumes);

        this.option = {
            backgroundColor: '#1a1a1a',
            animation: false,
            tooltip: {
                trigger: 'axis',
                backgroundColor: 'rgba(44, 62, 80, 0.95)',
                borderColor: '#3498db',
                textStyle: { color: '#e0e0e0', fontSize: 11 },
                formatter: function(params) {
                    return `价格: ${params[0].name}<br/>筹码: ${params[0].value}`;
                }
            },
            grid: {
                left: '15%',
                right: '10%',
                top: '10%',
                bottom: '15%'
            },
            xAxis: {
                type: 'value',
                name: '筹码',
                nameTextStyle: { color: '#95a5a6', fontSize: 10 },
                axisLine: { lineStyle: { color: '#3498db' } },
                axisLabel: { color: '#95a5a6', fontSize: 10 },
                splitLine: { lineStyle: { color: 'rgba(52, 152, 219, 0.1)' } }
            },
            yAxis: {
                type: 'category',
                data: prices,
                axisLine: { lineStyle: { color: '#3498db' } },
                axisLabel: { color: '#95a5a6', fontSize: 10 },
                splitLine: { show: false }
            },
            series: [
                {
                    type: 'bar',
                    data: volumes.map(v => ({
                        value: v,
                        itemStyle: {
                            color: `rgba(52, 152, 219, ${0.3 + (v / maxVolume) * 0.7})`
                        }
                    })),
                    barWidth: '80%',
                    label: {
                        show: false
                    }
                }
            ]
        };

        this.chart.setOption(this.option);
    }

    bindEvents() {
        window.addEventListener('resize', () => {
            this.chart.resize();
        });
    }

    refresh() {
        this.loadData();
        this.updateData();
    }

    updateData() {
        const prices = this.data.map(d => d.price);
        const volumes = this.data.map(d => d.volume);
        const maxVolume = Math.max(...volumes);

        this.chart.setOption({
            yAxis: { data: prices },
            series: [{
                data: volumes.map(v => ({
                    value: v,
                    itemStyle: {
                        color: `rgba(52, 152, 219, ${0.3 + (v / maxVolume) * 0.7})`
                    }
                }))
            }]
        });
    }

    resize() {
        this.chart.resize();
    }

    dispose() {
        if (this.chart) {
            this.chart.dispose();
        }
    }
}

window.ChipChart = ChipChart;