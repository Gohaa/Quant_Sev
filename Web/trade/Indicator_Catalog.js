/** Indicator_Catalog.js — Tulip 指标目录与默认参数 */
(function (global) {
    'use strict';

    var catalogCache = null;
    var palette = ['#f39c12', '#3498db', '#9b59b6', '#1abc9c', '#e67e22', '#e74c3c', '#2ecc71', '#95a5a6'];

    function guessDefaultOption(optionName, index) {
        var name = String(optionName || '').toLowerCase();
        if (name.indexOf('short') >= 0) return 12;
        if (name.indexOf('long') >= 0) return 26;
        if (name.indexOf('signal') >= 0) return 9;
        if (name.indexOf('period') >= 0 || name.indexOf('lookback') >= 0) return 14;
        if (name.indexOf('std') >= 0) return 2;
        if (name.indexOf('accel') >= 0) return 0.02;
        if (name.indexOf('maximum') >= 0 || name.indexOf('max') >= 0) return 0.2;
        if (name.indexOf('percent') >= 0) return 0.5;
        var fallbacks = [14, 20, 26, 9, 12, 2, 0.02, 0.2];
        return fallbacks[index % fallbacks.length];
    }

    function defaultOptionsString(indicator) {
        var names = (indicator && indicator.options) || [];
        if (!names.length) return '';
        return names.map(function (name, i) {
            return guessDefaultOption(name, i);
        }).join(',');
    }

    function loadCatalog(tradeView) {
        if (catalogCache) {
            return Promise.resolve(catalogCache);
        }
        if (!tradeView || !tradeView.listIndicators) {
            return Promise.reject(new Error('TradeView.listIndicators unavailable'));
        }
        return tradeView.listIndicators().then(function (data) {
            if (data && data.error) {
                throw new Error(data.error);
            }
            catalogCache = (data && data.indicators) || [];
            return catalogCache;
        });
    }

    function byType(type) {
        return (catalogCache || []).filter(function (ind) {
            return ind.type === type;
        });
    }

    function findByName(name) {
        var key = String(name || '').toLowerCase();
        return (catalogCache || []).find(function (ind) {
            return String(ind.name || '').toLowerCase() === key;
        }) || null;
    }

    function seriesColor(index) {
        return palette[index % palette.length];
    }

    function isBarOutput(name) {
        var n = String(name || '').toLowerCase();
        return n.indexOf('histogram') >= 0 || n.indexOf('hist') >= 0 || n.indexOf('bar') >= 0;
    }

    global.IndicatorCatalog = {
        load: loadCatalog,
        byType: byType,
        find: findByName,
        defaultOptions: defaultOptionsString,
        seriesColor: seriesColor,
        isBarOutput: isBarOutput,
        getAll: function () { return (catalogCache || []).slice(); }
    };
})(typeof window !== 'undefined' ? window : this);
