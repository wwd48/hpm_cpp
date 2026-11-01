#pragma once

#include "lfm_types.h"

#include <cstdint>
#include <limits>
#include <string>

struct LFM_API TriggerRecorder {
    StockId stock_id = 0;
    std::string univ_name;
    int32_t trigger_time = 0;
    uint32_t order_id = 0;
    uint32_t trade_id = 0;
    bool has_order_id = false;
    bool has_trade_id = false;
    // 触发已经决定收口，但因子要等这一笔真正算完再关闸门。
    bool close_gate = false;
    int64_t md_time = 0;
    int64_t after_trigger_time = 0;
    int64_t before_factor_time = 0;
    int64_t after_factor_time = 0;
    int64_t after_model_time = 0;
    int pred_bool = 0;
    float pred_value = std::numeric_limits<float>::quiet_NaN();
    // 已经写进因子的条数。委托是全部委托行，成交只计真实成交。没有反查另一路行情。
    int32_t order_data_idx = 0;
    int32_t trade_data_idx = 0;

    void set_time(int64_t md_time_, int64_t after_trigger_time_, int64_t before_factor_time_,
                  int64_t after_factor_time_, int64_t after_model_time_);
    void set_pred(int pred_bool_, float pred_value_);
};
