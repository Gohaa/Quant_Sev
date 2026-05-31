class PositionPanel {
    constructor(containerId) {
        this.containerId = containerId;
        this.data = null;
        this.updateInterval = null;
    }

    init() {
        this.loadData();
        this.render();
        this.startAutoUpdate();
    }

    loadData() {
        this.data = this.generatePositionData();
    }

    generatePositionData() {
        return {
            positions: [
                { contract: 'RB2410', long: 10, short: 0, avgPrice: 3650, profit: 6000 },
                { contract: 'HC2410', long: 0, short: 5, avgPrice: 3800, profit: -2500 },
                { contract: 'IF2406', long: 2, short: 0, avgPrice: 4095, profit: 9000 }
            ],
            summary: {
                totalContracts: 3,
                totalLong: 12,
                totalShort: 5,
                totalProfit: 12500
            }
        };
    }

    render() {
        const container = document.getElementById(this.containerId);
        if (!container) return;

        const { positions, summary } = this.data;

        let html = `
            <div class="position-summary">
                <div class="summary-item">
                    <span class="summary-label">总持仓:</span>
                    <span class="summary-value">${summary.totalContracts}合约</span>
                </div>
                <div class="summary-item">
                    <span class="summary-label">多头:</span>
                    <span class="summary-value position-long">${summary.totalLong}手</span>
                </div>
                <div class="summary-item">
                    <span class="summary-label">空头:</span>
                    <span class="summary-value position-short">${summary.totalShort}手</span>
                </div>
                <div class="summary-item">
                    <span class="summary-label">盈亏:</span>
                    <span class="summary-value ${summary.totalProfit >= 0 ? 'profit-up' : 'profit-down'}">${summary.totalProfit >= 0 ? '+' : ''}¥${summary.totalProfit}</span>
                </div>
            </div>
            <table class="position-table">
                <thead>
                    <tr>
                        <th>合约</th>
                        <th>多头</th>
                        <th>空头</th>
                        <th>均价</th>
                        <th>盈亏</th>
                    </tr>
                </thead>
                <tbody>
        `;

        positions.forEach(pos => {
            html += `
                <tr>
                    <td>${pos.contract}</td>
                    <td class="${pos.long > 0 ? 'position-long' : ''}">${pos.long}</td>
                    <td class="${pos.short > 0 ? 'position-short' : ''}">${pos.short}</td>
                    <td>${pos.avgPrice}</td>
                    <td class="${pos.profit >= 0 ? 'profit-up' : 'profit-down'}">${pos.profit >= 0 ? '+' : ''}¥${Math.abs(pos.profit)}</td>
                </tr>
            `;
        });

        html += `
                </tbody>
            </table>
        `;

        container.innerHTML = html;
    }

    startAutoUpdate() {
        this.updateInterval = setInterval(() => {
            this.updateData();
        }, 3000);
    }

    stopAutoUpdate() {
        if (this.updateInterval) {
            clearInterval(this.updateInterval);
            this.updateInterval = null;
        }
    }

    updateData() {
        const { positions, summary } = this.data;

        positions.forEach(pos => {
            const change = Math.floor(Math.random() * 200 - 100);
            pos.profit += change;
        });

        summary.totalProfit = positions.reduce((sum, pos) => sum + pos.profit, 0);

        this.render();
    }

    refresh() {
        this.loadData();
        this.render();
    }

    dispose() {
        this.stopAutoUpdate();
    }
}

window.PositionPanel = PositionPanel;