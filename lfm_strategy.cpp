#include "lfm_strategy.h"

#include "csv_table.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <csignal>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

std::atomic<lfm_strategy*> g_stop_target{nullptr};
std::atomic<int> g_stop_hits{0};

void on_stop_signal(int sig) {
    if (g_stop_hits.fetch_add(1, std::memory_order_relaxed) > 0) {
        std::signal(sig, SIG_DFL);
        std::raise(sig);
        return;
    }
    if (lfm_strategy* target = g_stop_target.load(std::memory_order_acquire)) {
        target->request_stop();
    }
}

void arm_stop_signals(lfm_strategy* target) {
    g_stop_hits.store(0, std::memory_order_relaxed);
    g_stop_target.store(target, std::memory_order_release);
    struct sigaction act {};
    act.sa_handler = on_stop_signal;
    sigemptyset(&act.sa_mask);
    sigaction(SIGINT, &act, nullptr);
    sigaction(SIGTERM, &act, nullptr);
}

bool shanghai_stock(int32_t instrument_id) {
    const int prefix = instrument_id / 10000;
    return prefix == 60 || prefix == 68;
}

void require_shm(const std::string& name) {
    const std::string path = "/dev/shm/" + name;
    std::ifstream in(path);
    if (!in.good()) {
        throw std::runtime_error("shared memory missing: " + path);
    }
}

}

lfm_strategy::lfm_strategy(int strategy_id) : base_strategy(strategy_id) {
    arm_stop_signals(this);
}

lfm_strategy::~lfm_strategy() {
    g_stop_target.store(nullptr, std::memory_order_release);
    try {
        finish();
    } catch (const std::exception& ex) {
        std::cerr << "lfm strategy finish failed: " << ex.what() << std::endl;
    }
}

void lfm_strategy::init(const std::string& tradingday, const std::string& daily_dir, const std::string& output_dir,
                        const std::string& lfm_dir, const std::vector<std::string>& books) {
    CsvTable table;
    const std::string path = daily_dir + "/" + tradingday + "_universe.csv";
    if (!load_csv_table(path, table)) {
        throw std::runtime_error("load universe failed: " + path);
    }
    std::vector<StockId> stocks;
    stocks.reserve(table.rows.size());
    for (const auto& item : table.rows) {
        const int id = std::stoi(item.first);
        if (valid_stock(id)) {
            stocks.push_back(id);
        }
    }
    std::filesystem::create_directories(output_dir);
    engine_.init_lfm(stocks, daily_dir, output_dir, lfm_dir, tradingday, books);
    open_market_data(stocks);
    inited_ = true;
    std::string book_tag;
    for (const std::string& book : books) {
        book_tag += book;
    }
    std::cerr << "lfm strategy init tradingday " << tradingday << " universe " << stocks.size()
              << " books " << book_tag << " daily " << daily_dir << " output " << output_dir
              << " lfm " << lfm_dir << std::endl;
}

void lfm_strategy::open_market_data(const std::vector<StockId>& stocks) {
    require_shm(trans_shm_name_);
    require_shm(order_shm_name_);
    require_shm(snap_shm_name_);
    require_shm("global_index");

    size_t shm_size = 0;
    trans_shm_ = std::make_unique<shm_md_block<MDUniTrans>>();
    order_shm_ = std::make_unique<shm_md_block<MDUniOrder>>();
    snap_shm_ = std::make_unique<shm_md_block<MDStockSnap>>();
    trans_shm_->get_shm(trans_shm_name_, shm_size, true);
    order_shm_->get_shm(order_shm_name_, shm_size, true);
    snap_shm_->get_shm(snap_shm_name_, shm_size, true);

    for (StockId id : stocks) {
        const int8_t exchange = shanghai_stock(id) ? EXCHANGE_SSE : EXCHANGE_SZE;
        stk_item* item = create_or_get_stk(static_cast<uint32_t>(id), exchange);
        const uint64_t code = static_cast<uint64_t>(id);
        if (trans_shm_->get_hdr(code) != nullptr && snap_shm_->get_hdr(code) != nullptr &&
            order_shm_->get_hdr(code) != nullptr) {
            item->set_md_reader(trans_shm_.get(), snap_shm_.get(), order_shm_.get());
        }
    }
    global_sub_ = std::make_unique<cell_sub<global_index>>();
    global_sub_->set_read_index(0);
}

size_t lfm_strategy::process_global_index() {
    if (!global_sub_) {
        return 0;
    }
    size_t dispatched = 0;
    global_index* item = nullptr;
    while (!stop_.load(std::memory_order_acquire)) {
        const uint64_t published = global_sub_->get_cur_sequence();
        const uint64_t consumed = global_sub_->get_read_index();
        const uint64_t capacity = static_cast<uint64_t>(global_sub_->m_ptr->ring_size);
        if (capacity == 0 || (published > consumed && published - consumed > capacity)) {
            throw std::runtime_error("global_index records overwritten before consumption");
        }
        if (!global_sub_->try_read_new(item)) {
            break;
        }
        ++dispatched;
        if (item == nullptr) {
            continue;
        }
        const global_index index = *item;
        stk_item* stock = get_stk(index.instrumentId);
        if (stock == nullptr) {
            continue;
        }
        if (index.type == CN_STK_ORDER && !stock->is_sub_order()) {
            order_shm_->load_instruments_addresses();
            if (order_shm_->get_hdr(index.instrumentId) == nullptr) {
                throw std::runtime_error("order shared memory header missing: " +
                                         std::to_string(index.instrumentId));
            }
            stock->set_md_reader(trans_shm_.get(), snap_shm_.get(), order_shm_.get());
        } else if (index.type == CN_STK_TRANS && !stock->is_sub_trans()) {
            trans_shm_->load_instruments_addresses();
            if (trans_shm_->get_hdr(index.instrumentId) == nullptr) {
                throw std::runtime_error("trade shared memory header missing: " +
                                         std::to_string(index.instrumentId));
            }
            stock->set_md_reader(trans_shm_.get(), snap_shm_.get(), order_shm_.get());
        } else if (index.type == CN_STK_SNAP && !stock->is_sub_snap()) {
            snap_shm_->load_instruments_addresses();
            if (snap_shm_->get_hdr(index.instrumentId) == nullptr) {
                throw std::runtime_error("snapshot shared memory header missing: " +
                                         std::to_string(index.instrumentId));
            }
            stock->set_md_reader(trans_shm_.get(), snap_shm_.get(), order_shm_.get());
        }
        int32_t ntp = 0;
        if (index.type == CN_STK_ORDER && stock->is_sub_order()) {
            auto* order = stock->get_cur_order(index.index);
            if (order == nullptr) {
                throw std::runtime_error("order shared memory record missing: instrument=" +
                                         std::to_string(index.instrumentId) +
                                         " index=" + std::to_string(index.index));
            }
            ntp = order->nTP;
            if (end_ntp_ > 0 && ntp > end_ntp_) {
                stop_.store(true, std::memory_order_release);
                break;
            }
            on_order_md(*order, stock);
        } else if (index.type == CN_STK_TRANS && stock->is_sub_trans()) {
            auto* trans = stock->get_cur_trans(index.index);
            if (trans == nullptr) {
                throw std::runtime_error("trade shared memory record missing: instrument=" +
                                         std::to_string(index.instrumentId) +
                                         " index=" + std::to_string(index.index));
            }
            ntp = trans->nTP;
            if (end_ntp_ > 0 && ntp > end_ntp_) {
                stop_.store(true, std::memory_order_release);
                break;
            }
            on_trans_md(*trans, stock);
        } else if (index.type == CN_STK_SNAP && stock->is_sub_snap()) {
            auto* snap = stock->get_cur_snap(index.index);
            if (snap == nullptr) {
                throw std::runtime_error("snapshot shared memory record missing: instrument=" +
                                         std::to_string(index.instrumentId) +
                                         " index=" + std::to_string(index.index));
            }
            ntp = snap->nTP;
            if (end_ntp_ > 0 && ntp > end_ntp_) {
                stop_.store(true, std::memory_order_release);
                break;
            }
            on_snap_md(*snap, stock);
        }
    }
    return dispatched;
}

void lfm_strategy::work() {
    if (stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return;
    }
    try {
        base_strategy::process_td_ev();
        base_strategy::process_pos_ev();
        if (process_global_index() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const std::exception& ex) {
        std::cerr << "lfm live work failed: " << ex.what() << std::endl;
        stop_.store(true, std::memory_order_release);
    }
}

void lfm_strategy::replay_global_index() {
    if (!inited_) {
        throw std::runtime_error("replay_global_index called before init");
    }
    uint64_t dispatched = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (!stop_.load(std::memory_order_acquire)) {
        const size_t processed = process_global_index();
        dispatched += processed;
        if (processed == 0) {
            break;
        }
    }
    finish();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::cerr << "lfm global replay done events " << dispatched << " ms " << ms
              << " triggers " << engine_.trigger_count() << std::endl;
}

void lfm_strategy::finish() {
    if (!inited_ || finished_book_) {
        return;
    }
    if (g_stop_hits.load(std::memory_order_relaxed) > 0) {
        std::cerr << "lfm caught stop signal, writing results" << std::endl;
    }
    finished_book_ = true;
    engine_.flush_end();
}

void lfm_strategy::on_snap_md(const MDStockSnap& snap, stk_item*) {
    engine_.on_snap_md(snap);
}

void lfm_strategy::on_order_md(const MDUniOrder& order, stk_item*) {
    engine_.on_order_md(order);
}

void lfm_strategy::on_trans_md(const MDUniTrans& trans, stk_item*) {
    engine_.on_trans_md(trans);
}
