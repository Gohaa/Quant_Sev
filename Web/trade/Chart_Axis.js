/** Chart_Axis.js — 可见区间索引与 Y 轴范围计算 */
(function (global) {
    'use strict';

    function visibleIndexRange(length, startPct, endPct) {
        if (!length) {
            return { start: 0, end: 0 };
        }
        var start = Math.max(0, Math.floor(length * startPct / 100));
        var end = Math.min(length, Math.max(start + 1, Math.ceil(length * endPct / 100)));
        return { start: start, end: end };
    }

    function extendRange(range, value) {
        if (value == null || value === '' || isNaN(value)) {
            return range;
        }
        var v = Number(value);
        if (!range) {
            return { min: v, max: v };
        }
        if (v < range.min) range.min = v;
        if (v > range.max) range.max = v;
        return range;
    }

    function padRange(range, ratio) {
        if (!range || !isFinite(range.min) || !isFinite(range.max)) {
            return null;
        }
        ratio = ratio == null ? 0.06 : ratio;
        if (range.min === range.max) {
            var delta = Math.abs(range.min) * 0.02 || 1;
            return { min: range.min - delta, max: range.max + delta };
        }
        var pad = (range.max - range.min) * ratio;
        return { min: range.min - pad, max: range.max + pad };
    }

    global.ChartAxisUtil = {
        visibleIndexRange: visibleIndexRange,
        extendRange: extendRange,
        padRange: padRange
    };
})(typeof window !== 'undefined' ? window : this);
