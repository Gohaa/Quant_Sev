#pragma once

#include <string>

namespace quant_sev::core {

struct OrderRequest {
    std::string user_id;
    std::string instrument_id;
    std::string direction;    // buy | sell
    std::string offset;       // open | close | close_today
    std::string price_type;   // limit | market | opponent
    double price{0};
    int volume{0};
};

struct OrderResult {
    bool ok{false};
    std::string message;
    std::string order_ref;
    int error_id{0};
};

struct CancelOrderRequest {
    std::string user_id;
    std::string instrument_id;
    std::string order_ref;
    std::string order_sys_id;
    std::string exchange_id;
};

struct CancelResult {
    bool ok{false};
    std::string message;
    std::string order_ref;
    int error_id{0};
};

struct OrderUpdateRecord {
    std::string user_id;
    std::string instrument_id;
    std::string order_ref;
    std::string order_sys_id;
    std::string direction;
    std::string offset;
    std::string status;
    double limit_price{0};
    int volume_total{0};
    int volume_traded{0};
    int volume_left{0};
    std::string insert_date;
    std::string insert_time;
    std::string update_time;
    std::string status_msg;
};

struct TradeUpdateRecord {
    std::string user_id;
    std::string trade_id;
    std::string order_ref;
    std::string order_sys_id;
    std::string instrument_id;
    std::string exchange_id;
    std::string direction;
    std::string offset;
    double price{0};
    int volume{0};
    std::string trade_date;
    std::string trade_time;
};

struct TradingAccountSnapshot {
    std::string user_id;
    std::string broker_id;
    std::string account_id;
    std::string trading_day;
    std::string currency_id;
    double balance{0};
    double available{0};
    double curr_margin{0};
    double frozen_margin{0};
    double frozen_cash{0};
    double frozen_commission{0};
    double commission{0};
    double close_profit{0};
    double position_profit{0};
    double withdraw_quota{0};
    double deposit{0};
    double withdraw{0};
    double pre_balance{0};
    double exchange_margin{0};
};

struct InvestorPositionSnapshot {
    std::string instrument_id;
    int long_volume{0};
    int short_volume{0};
    double avg_long_price{0};
    double avg_short_price{0};
    double long_profit{0};
    double short_profit{0};
    double position_profit{0};
};

}  // namespace quant_sev::core
