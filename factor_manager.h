#pragma once

#include "lfm_types.h"
#include "market_data.h"

#include <cstdint>
#include <iosfwd>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// 编排盘前因子和 snap、trans、ot、otd、sot。公式在各自的 cpp 里，本文件只负责拼装和跳过重算。
class LFM_API FactorManager {
public:
    void init(const std::vector<StockId>& stock_list, const std::vector<std::string>& univ_list,
              const std::string& daily_dir, const std::string& tradingday);

    void updateTrade(const MDUniTrans& trans);
    void updateOrder(const MDUniOrder& order);
    void updateSnap(const MDStockSnap& snap);

    // 条数和触发时间都没变时，把上一个 univ 的当日因子拷到当前 univ 后返回。
    // 真正重算返回 true；事件没变、只复用上一行时返回 false。
    // otd 的滚动窗口对齐 trigger_time，不跟最后一笔快照的时间走。
    bool calculatefactor(StockId stock_id, const std::string& univ_name, int32_t trigger_time);
    // 这次触发算完后还要接着收后面的 univ，把截止时钟放开。
    void release_clock(StockId stock_id);
    void seal(StockId stock_id);
    bool sealed(StockId stock_id) const;

    void enable_calc_timing(bool on);
    void dump_calc_timing(std::ostream& out) const;
    void dump_mem(std::ostream& out) const;

    double* factor_row(const std::string& univ_name, StockId stock_id);
    const double* factor_row(const std::string& univ_name, StockId stock_id) const;

private:
    struct CalcRecord {
        bool has_last = false;
        uint64_t event_count = 0;
        int32_t trigger_time = 0;
        std::string univ_name;
    };

    std::vector<double>& ensure_row(std::unordered_map<StockId, std::vector<double>>& stock_map, StockId stock_id);

    std::unordered_map<std::string, std::unordered_map<StockId, std::vector<double>>> factor_uc_map_;
    std::unordered_map<StockId, std::vector<double>> last_prefix_;
    std::vector<uint64_t> event_count_;
    std::vector<int32_t> last_ntp_;
    std::vector<char> sealed_;
    struct CalcTime {
        int32_t ntp = 0;
        double us = 0;
        double snap_us = 0;
        double trans_us = 0;
        double ot_us = 0;
        double otd_us = 0;
        double sot_us = 0;
        bool skipped = false;
    };

    std::vector<CalcRecord> last_trigger_record_;
    std::vector<CalcTime> calc_times_;
    std::mutex time_mu_;
    bool time_calc_ = false;
};
