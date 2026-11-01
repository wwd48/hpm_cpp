#pragma once

#include "lfm_types.h"
#include "market_data.h"

#include <cmath>
#include <cstdint>

struct UnivHit {
    bool emit = false;
    bool stop = false;
    int32_t trigger_time = 0;
    uint32_t order_id = 0;
    uint32_t trade_id = 0;
};

inline bool pre_ok_limits(float pre_close, float limit_up, float limit_down) {
    return std::isfinite(pre_close) && std::isfinite(limit_up) && std::isfinite(limit_down) && limit_up > 0 &&
           limit_down > 0;
}

inline int32_t raw_px(double yuan) {
    return static_cast<int32_t>(std::nearbyint(yuan * 10000.0));
}

inline int32_t raw_px_fen(double yuan) {
    return static_cast<int32_t>(std::nearbyint(yuan * 100.0)) * 100;
}

inline double yuan_amt(int32_t px, int32_t vol) {
    return std::nearbyint(static_cast<double>(px) * static_cast<double>(vol) * 0.01) / 100.0;
}

inline bool normal_order(int tick) {
    return tick == SZ_MARKET || tick == SZ_LIMIT || tick == SZ_BEST || tick == SH_ADD || tick == SH_COMBINE;
}

inline bool fill_row(const MDUniTrans& trans) {
    return trans.tickType == SZ_FILL && trans.tradePrice != 0 && trans.bidId != 0 && trans.askId != 0;
}
