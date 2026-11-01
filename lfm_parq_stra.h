#pragma once

#include "factor_manager.h"
#include "model_manager.h"
#include "sim_parquet_stra.h"
#include "trigger_manager.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class lfm_parq_stra : public sim_parq_stra {
public:
    void init_lfm(const std::vector<StockId>& stock_list, const std::string& daily_dir,
                  const std::string& output_dir, const std::string& lfm_dir, const std::string& tradingday,
                  const std::vector<std::string>& books);

    void on_snap_md(const MDStockSnap& snap) override;
    void on_order_md(const MDUniOrder& order) override;
    void on_trans_md(const MDUniTrans& trans) override;

    int64_t trigger_count() const;
    void enable_calc_timing(bool on);
    void dump_calc_timing(std::ostream& out) const;
    void dump_hit_timing(std::ostream& out) const;
    void dump_event_timing(std::ostream& out) const;
    void set_log_hits(bool on);
    // 实盘快照 turnover 先除这个数，得到和 HDF TotalAmount 一样的单位。HDF 回放设为 1。
    void set_turnover_divisor(int64_t divisor);
    // 每个线程在自己的股票走完后调用，把还挂着的触发算完。
    void flush_end();

private:
    void before_event(StockId stock, int32_t ntp);
    void defer_hits(const std::vector<TriggerRecorder>& hits);
    void settle(StockId stock, std::vector<TriggerRecorder> due, int32_t actual_time);
    void handle_hits(std::vector<TriggerRecorder>& hits, int32_t actual_time);
    void load_trade_limits(const std::string& daily_dir, const std::string& tradingday, const std::string& book,
                           std::vector<std::string>& univ_list);
    void write_samples();
    void write_factor_table() const;
    // 逐笔到达就写入因子。触发成立时按当前已写入的状态立刻计算，不等这一毫秒结束。
    void feed_order(const MDUniOrder& order);
    void feed_trade(const MDUniTrans& trans);
    void emit_now(StockId stock, std::vector<TriggerRecorder> hits, int32_t actual_time);

    int64_t turnover_divisor_ = 100000;

    struct ScenarioLimit {
        int cap = 0;
        int order = 0;
        int used = 0;
        int book_target = 0;
        int* book_bought = nullptr;
    };

    struct SampleRow {
        StockId stock_id = 0;
        std::string univ_name;
        uint32_t trigger_idx = 0;
        int32_t trigger_time = 0;
        int32_t actual_time = 0;
        float pred_value = 0;
        int scenario_order = -1;
        int bought = 0;
        int32_t order_data_idx = 0;
        int32_t trade_data_idx = 0;
        std::vector<double> factors;
    };

    FactorManager factors_;
    TriggerManager triggers_;
    ModelManager models_;
    std::unordered_map<std::string, ScenarioLimit> limits_;
    // 每个账本一份已买笔数。map 插入不会使已有元素的指针失效。
    std::map<std::string, int> book_bought_;
    struct HitLat {
        double e2e_us = 0;
        double calc_us = 0;
        double pred_us = 0;
        bool full = false;
    };

    std::vector<SampleRow> samples_;
    std::vector<HitLat> hit_lats_;
    bool log_hits_ = true;
    std::mutex hit_mu_;
    std::condition_variable commit_cv_;
    std::atomic<uint64_t> commit_seq_{0};
    uint64_t commit_turn_ = 0;
    std::string daily_dir_;
    std::string output_dir_;
    std::string tradingday_;
    int64_t trigger_count_ = 0;
};
