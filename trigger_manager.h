#pragma once

#include "lfm_types.h"
#include "market_data.h"
#include "trigger_recorder.h"

#include <string>
#include <vector>

class LFM_API TriggerManager {
public:
    void init(const std::vector<StockId>& stock_list, const std::vector<std::string>& univ_list,
              const std::vector<std::string>& pre_data_list, const std::string& daily_dir,
              const std::string& tradingday);

    std::vector<TriggerRecorder> trade_trigger(const MDUniTrans& trans);
    std::vector<TriggerRecorder> order_trigger(const MDUniOrder& order);
    // 时间戳前进时的钩子。tchta40、tchn20 已在触发当笔决定，这里不再结清它们。
    std::vector<TriggerRecorder> advance_time(StockId stock_id, int32_t ntp);
    std::vector<TriggerRecorder> finish_stock(StockId stock_id);
    bool still_open(StockId stock_id) const;
    void close_gate(StockId stock_id, const std::string& univ_name);

private:
    struct PreRow {
        float pre_close = 0;
        float limit_up = 0;
        float limit_down = 0;
    };

    void note_status(StockId stock_id, int univ_bit, bool stop);
    std::vector<TriggerRecorder> collect(StockId stock_id, int32_t trigger_time, uint32_t order_id, uint32_t trade_id,
                                         bool from_order, bool emit, bool stop, int univ_bit);
    void push_hit(std::vector<TriggerRecorder>& out, StockId stock_id, int univ_bit, int32_t trigger_time,
                  uint32_t order_id, uint32_t trade_id, bool from_order, bool emit, bool stop);

    struct Gate {
        bool alive = false;
        uint32_t open_mask = 0;
    };

    std::vector<std::string> univ_list_;
    std::vector<Gate> gates_;
    int n_univ_ = 0;
    int taaoc3_bit_ = -1;
    int tchbt2_bit_ = -1;
    int tchbt8_bit_ = -1;
    int tchn20_bit_ = -1;
    int tchno20_bit_ = -1;
    int tchta40_bit_ = -1;
    int cf7_bit_ = -1;
    int cf8_bit_ = -1;
    int brk1_bit_ = -1;
    int brk3_bit_ = -1;
    int brk7_bit_ = -1;
    int brk8_bit_ = -1;
    int brk16_bit_ = -1;
};
