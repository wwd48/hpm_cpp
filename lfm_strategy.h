#pragma once

#include "lfm_parq_stra.h"
#include "base_strategy.h"

#include <atomic>
#include <memory>
#include <string>

// 挂在 base_strategy 上，行情从 global_index 和三块共享内存读。
// 实盘走 work()，回放把已经写好的 global_index 读完。
class lfm_strategy : public base_strategy {
public:
    explicit lfm_strategy(int strategy_id);
    ~lfm_strategy() override;

    void init(const std::string& tradingday, const std::string& daily_dir, const std::string& output_dir,
              const std::string& lfm_dir, const std::vector<std::string>& books);
    void set_end_ntp(int32_t end_ntp) { end_ntp_ = end_ntp; }
    void replay_global_index();
    void work() override;
    // 信号处理里只置位。回放和实盘的循环看到后退出，再写结果。
    void request_stop() noexcept { stop_.store(true, std::memory_order_release); }

    bool finished() const { return stop_.load(std::memory_order_acquire); }

    void on_snap_md(const MDStockSnap& snap, stk_item* item) override;
    void on_order_md(const MDUniOrder& order, stk_item* item) override;
    void on_trans_md(const MDUniTrans& trans, stk_item* item) override;

private:
    void open_market_data(const std::vector<StockId>& stocks);
    size_t process_global_index();
    void finish();

    lfm_parq_stra engine_;
    std::unique_ptr<shm_md_block<MDUniTrans>> trans_shm_;
    std::unique_ptr<shm_md_block<MDUniOrder>> order_shm_;
    std::unique_ptr<shm_md_block<MDStockSnap>> snap_shm_;
    std::unique_ptr<cell_sub<global_index>> global_sub_;
    std::atomic<bool> stop_{false};
    int32_t end_ntp_ = 0;
    bool inited_ = false;
    bool finished_book_ = false;
    std::string trans_shm_name_ = "SIM_UniTrans.md";
    std::string order_shm_name_ = "SIM_UniOrder.md";
    std::string snap_shm_name_ = "SIM_MDStockSnap.md";
};
