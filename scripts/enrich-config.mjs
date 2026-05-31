import fs from 'fs';
import path from 'path';

const root = path.resolve(import.meta.dirname, '..');
const rulesPath = path.join(root, 'config', 'Contract_Rules.json');
const symPath = path.join(root, 'config', 'Symbol_list.json');

const rules = JSON.parse(fs.readFileSync(rulesPath, 'utf8'));
const sym = JSON.parse(fs.readFileSync(symPath, 'utf8'));

function parseInstrument(id) {
  const upper = id.toUpperCase();
  const lower = id.toLowerCase();
  const exchanges = [...rules.futureContracts.exchanges].map((ex) => ({
    ...ex,
    symbols: [...ex.symbols].sort((a, b) => b.length - a.length),
  }));

  for (const ex of exchanges) {
    for (const s of ex.symbols) {
      const isUpper = ex.symbolCase === 'upper';
      const prefix = isUpper ? s.toUpperCase() : s.toLowerCase();
      const hay = isUpper ? upper : lower;
      if (!hay.startsWith(prefix)) continue;
      const rest = id.slice(prefix.length);
      const yd = ex.yearSuffixDigits;
      const md = ex.deliveryMonthDigits;
      if (rest.length !== yd + md) continue;
      const year_suffix = rest.slice(0, yd);
      const delivery_month = rest.slice(yd);
      const product = isUpper ? prefix : prefix.toLowerCase();
      return {
        exchange: ex.exchange,
        product,
        year_suffix,
        delivery_month,
        month_slot: delivery_month,
      };
    }
  }
  return null;
}

const rolloverSection = {
  title: 'CTP 期货主力换月与 Storage 路径规则',
  description: '解析 instrument_id、映射 data/ 历史目录、指导 Symbol_list 主力合约更新',
  yearDecode: {
    CZCE: { digits: 1, decadeBase: 2020, note: '年份末1位，当前十年 2020-2029 映射为 0-9' },
    default: { digits: 2, note: '年份后2位，如 26 表示 2026' },
  },
  storageLayout: {
    historicalBars: {
      path: 'data/{exchange}/{product}/{month_slot}/{period}.csv',
      month_slot: {
        main: '主力连续（历史回测/展示用）',
        '01-12': '具体交割月序号目录，与 delivery_month 两位字符串一致',
      },
      periods: {
        m1: { minutes: 1, csvHeader: 'date,time,open,high,low,close,volume,turnover,open_interest' },
        m15: { minutes: 15 },
        h1: { minutes: 60 },
        d1: { minutes: 1440 },
      },
    },
    liveSession: {
      tickPath: 'data/{exchange}/{product}/{month_slot}/tick.csv',
      tickHeader: 'trading_day,update_time,update_millisec,last_price,volume,turnover,open_interest,bid_price1,bid_volume1,ask_price1,ask_volume1',
      barPath: 'data/{exchange}/{product}/{month_slot}/{period}.csv',
      barHeader: 'date,time,open,high,low,close,volume,turnover,open_interest',
      note: '实盘 tick/bar 与历史同目录 month_slot',
      flushIntervalMs: 1000,
    },
  },
  rolloverPolicy: {
    defaultMethod: 'volume_and_open_interest',
    description: '主力换月：次月合约成交量+持仓量连续2个交易日超过当前主力时切换',
    trigger: {
      metric: 'volume_plus_open_interest',
      consecutiveDays: 2,
      compareWith: 'next_nearby_contract',
    },
    actions: {
      updateSymbolList: '修改 Symbol_list.json 对应条目的 instrument_id',
      resubscribeQuote: 'Gateway POST /api/unsubscribe/symbol 旧合约 + /api/load/symbol 新合约',
      storageNote: '换月后 tick.csv / m1.csv 写入新 month_slot 目录',
    },
    exchangeRules: [
      {
        exchange: 'SHFE',
        nearMonthOffset: 1,
        avoidDeliveryMonthDays: 5,
        note: '交割月前约5个交易日开始降低主力权重；金属类常提前1-2月换月',
      },
      {
        exchange: 'DCE',
        nearMonthOffset: 1,
        avoidDeliveryMonthDays: 5,
        note: '农产品/化工多数按持仓量切换',
      },
      {
        exchange: 'CZCE',
        nearMonthOffset: 1,
        avoidDeliveryMonthDays: 5,
        note: '1位年份后缀，换月后须同步更新 Symbol_list instrument_id',
      },
      {
        exchange: 'INE',
        nearMonthOffset: 1,
        avoidDeliveryMonthDays: 5,
      },
      {
        exchange: 'GFEX',
        nearMonthOffset: 1,
        avoidDeliveryMonthDays: 5,
      },
      {
        exchange: 'CFFEX',
        method: 'calendar',
        rollDay: 'third_friday_of_previous_month',
        note: '股指/国债期货通常于交割月前一个月第三个周五附近换月',
      },
    ],
    buildNextContract: {
      description: '根据当前 instrument_id 生成候选次月合约代码',
      stepOrder: ['parse_product', 'increment_delivery_month', 'adjust_year_suffix', 'format_by_exchange_pattern'],
    },
  },
};

rules.rollover = rolloverSection;
fs.writeFileSync(rulesPath, JSON.stringify(rules, null, 2) + '\n', 'utf8');

sym.contractRulesRef = 'Contract_Rules.json';
sym.rolloverPolicyRef = 'rolloverPolicy in Contract_Rules.json';
sym.storageLayoutRef = 'rollover.storageLayout in Contract_Rules.json';
sym.notes = [
  'instrument_id 与 CTP 一致；exchange/product/month_slot 由 Contract_Rules.futureContracts 解析。',
  '换月：见 Contract_Rules.json rollover.rolloverPolicy；更新 instrument_id 后重新订阅行情。',
  '历史与实盘目录：data/{exchange}/{product}/{month_slot}/ 下 m1.csv、tick.csv 等。',
];
sym.symbols = sym.symbols.map((item) => {
  const parsed = parseInstrument(item.instrument_id);
  if (!parsed) return item;
  const { storage_live_tick: _legacy, ...rest } = item;
  return {
    ...rest,
    exchange: parsed.exchange,
    product: parsed.product,
    year_suffix: parsed.year_suffix,
    delivery_month: parsed.delivery_month,
    month_slot: parsed.month_slot,
    storage_bar_path: `data/${parsed.exchange}/${parsed.product}/${parsed.month_slot}/m1.csv`,
    storage_tick_path: `data/${parsed.exchange}/${parsed.product}/${parsed.month_slot}/tick.csv`,
  };
});

const missing = sym.symbols.filter((s) => !s.exchange);
if (missing.length) {
  console.warn('Unparsed symbols:', missing.map((s) => s.instrument_id).join(', '));
}

fs.writeFileSync(symPath, JSON.stringify(sym, null, 2) + '\n', 'utf8');
console.log('Updated Contract_Rules.json and Symbol_list.json, symbols:', sym.symbols.length);
