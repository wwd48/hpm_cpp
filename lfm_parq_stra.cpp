#include "lfm_parq_stra.h"

#include "rapidjson/document.h"
#include "tch_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

struct EvAcc {
    std::atomic<uint64_t> n{0};
    std::atomic<uint64_t> ns{0};
    void add(uint64_t dt) {
        n.fetch_add(1, std::memory_order_relaxed);
        ns.fetch_add(dt, std::memory_order_relaxed);
    }
};

EvAcc g_order_fac;
EvAcc g_trade_fac;
EvAcc g_snap_fac;
EvAcc g_order_trg;
EvAcc g_trade_trg;
EvAcc g_advance_trg;

template <class F>
void timed(EvAcc& acc, F&& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    acc.add(static_cast<uint64_t>(dt < 0 ? 0 : dt));
}

void report_acc(std::ostream& out, const char* title, const EvAcc& acc) {
    const uint64_t n = acc.n.load(std::memory_order_relaxed);
    const uint64_t ns = acc.ns.load(std::memory_order_relaxed);
    const double us = n ? static_cast<double>(ns) / static_cast<double>(n) / 1000.0 : 0;
    out << title << " n " << n << " mean_us " << us << " sum_s " << (static_cast<double>(ns) / 1e9) << "\n";
}

}

void lfm_parq_stra::load_trade_limits(const std::string& daily_dir, const std::string& tradingday,
                                      const std::string& book, std::vector<std::string>& univ_list) {
    const std::string path = daily_dir + "/" + tradingday + "_trade_config_" + book + ".json";
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("open trade config failed: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    rapidjson::Document doc;
    doc.Parse(ss.str().c_str());
    if (doc.HasParseError() || !doc.HasMember("target_tot_num") || !doc.HasMember("univ_list") ||
        !doc.HasMember("scenarios")) {
        throw std::runtime_error("bad trade config: " + path);
    }
    int& bought = book_bought_[book];
    bought = 0;
    const int target = doc["target_tot_num"].GetInt();
    int order = 0;
    for (const auto& name_v : doc["univ_list"].GetArray()) {
        const std::string name = name_v.GetString();
        if (limits_.find(name) != limits_.end()) {
            throw std::runtime_error("scenario " + name + " appears in more than one selected book");
        }
        const auto sit = doc["scenarios"].FindMember(name.c_str());
        if (sit == doc["scenarios"].MemberEnd()) {
            throw std::runtime_error(path + " missing cap for " + name);
        }
        ScenarioLimit limit;
        limit.cap = sit->value.GetInt();
        limit.order = order++;
        limit.book_target = target;
        limit.book_bought = &bought;
        limits_.emplace(name, limit);
        univ_list.push_back(name);
    }
}

void lfm_parq_stra::init_lfm(const std::vector<StockId>& stock_list, const std::string& daily_dir,
                             const std::string& output_dir, const std::string& lfm_dir,
                             const std::string& tradingday, const std::vector<std::string>& books) {
    const std::vector<std::string> pre_data_list = {"pre_close", "limit_up", "limit_down", "lastamtul"};
    daily_dir_ = daily_dir;
    output_dir_ = output_dir;
    tradingday_ = tradingday;
    std::vector<std::string> univ_list;
    for (const std::string& book : books) {
        load_trade_limits(daily_dir, tradingday, book, univ_list);
    }
    if (univ_list.empty()) {
        throw std::runtime_error("selected books have no scenarios");
    }
    factors_.init(stock_list, univ_list, daily_dir, tradingday);
    triggers_.init(stock_list, univ_list, pre_data_list, daily_dir, tradingday);
    models_.init(lfm_dir, books);
    trigger_count_ = 0;
    commit_turn_ = 0;
    commit_seq_.store(0);
}

namespace {

thread_local std::unordered_map<int32_t, std::vector<TriggerRecorder>> g_pending;
thread_local std::unordered_set<int32_t> g_seen;

struct Stamp {
    int32_t ntp = 0;
    bool open = false;
};

thread_local std::unordered_map<int32_t, Stamp> g_stamp;
// 之前各毫秒已经写进因子的条数：委托行数，真实成交行数。
thread_local std::unordered_map<int32_t, std::pair<int32_t, int32_t>> g_applied;

bool real_fill(const MDUniTrans& trans) {
    return trans.tickType == SZ_FILL && trans.tradePrice != 0 && trans.tradeQty != 0 && trans.bidId != 0 &&
           trans.askId != 0;
}

}

void lfm_parq_stra::set_turnover_divisor(int64_t divisor) {
    if (divisor > 0) {
        turnover_divisor_ = divisor;
    }
}

void lfm_parq_stra::defer_hits(const std::vector<TriggerRecorder>& hits) {
    for (const TriggerRecorder& hit : hits) {
        g_pending[hit.stock_id].push_back(hit);
    }
}

void lfm_parq_stra::settle(StockId stock, std::vector<TriggerRecorder> due, int32_t actual_time) {
    if (due.empty()) {
        return;
    }
    handle_hits(due, actual_time);
    if (!triggers_.still_open(stock)) {
        factors_.seal(stock);
    } else {
        factors_.release_clock(stock);
    }
}

void lfm_parq_stra::emit_now(StockId stock, std::vector<TriggerRecorder> hits, int32_t actual_time) {
    if (hits.empty()) {
        return;
    }
    const auto applied = g_applied[stock];
    for (TriggerRecorder& hit : hits) {
        hit.order_data_idx = applied.first;
        hit.trade_data_idx = applied.second;
    }
    settle(stock, std::move(hits), actual_time);
}

void lfm_parq_stra::feed_order(const MDUniOrder& order) {
    if (factors_.sealed(order.instrumentId)) {
        return;
    }
    factors_.updateOrder(order);
    g_applied[order.instrumentId].first += 1;
}

void lfm_parq_stra::feed_trade(const MDUniTrans& trans) {
    if (factors_.sealed(trans.instrumentId)) {
        return;
    }
    factors_.updateTrade(trans);
    if (real_fill(trans)) {
        g_applied[trans.instrumentId].second += 1;
    }
}

void lfm_parq_stra::before_event(StockId stock, int32_t ntp) {
    if (!valid_stock(stock)) {
        return;
    }
    g_seen.insert(stock);
    Stamp& stamp = g_stamp[stock];
    if (stamp.open && ntp > stamp.ntp) {
        std::vector<TriggerRecorder> due;
        timed(g_advance_trg, [&] { due = triggers_.advance_time(stock, ntp); });
        auto it = g_pending.find(stock);
        if (it != g_pending.end()) {
            std::vector<TriggerRecorder> keep;
            keep.reserve(it->second.size());
            for (TriggerRecorder& hit : it->second) {
                if (hit.trigger_time > 0 && hit.trigger_time < ntp) {
                    due.push_back(std::move(hit));
                } else {
                    keep.push_back(std::move(hit));
                }
            }
            it->second.swap(keep);
        }
        // 时间往前走时，因子里已经是上一笔之前的状态。这里只结算到点的触发。
        emit_now(stock, std::move(due), stamp.ntp);
        stamp.open = false;
    }
    if (!g_stamp[stock].open) {
        g_stamp[stock].open = true;
        g_stamp[stock].ntp = ntp;
    }
}

void lfm_parq_stra::write_samples() {
    if (!log_hits_) {
        return;
    }
    std::lock_guard<std::mutex> lock(hit_mu_);
    const std::string path = output_dir_ + "/" + tradingday_ + "_trigger_sample.csv";
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("write trigger sample failed: " + path);
    }
    out << "stock_id,scenario,trigger_idx,trigger_time,actual_time,order_data_idx,trade_data_idx,pred_value,scenario_order,bought\n";
    out.precision(9);
    for (const SampleRow& row : samples_) {
        out << row.stock_id << ',' << row.univ_name << ',' << row.trigger_idx << ','
            << row.trigger_time << ',' << row.actual_time << ','
            << row.order_data_idx << ',' << row.trade_data_idx << ','
            << row.pred_value << ','
            << row.scenario_order << ',' << row.bought << '\n';
    }
}

void lfm_parq_stra::write_factor_table() const {
    if (samples_.empty()) {
        return;
    }
    const std::string path = output_dir_ + "/" + tradingday_ + "_factor.csv";
    std::ofstream out(path);
    if (!out) {
        throw std::runtime_error("write factor table failed: " + path);
    }
    std::vector<std::string> names;
    names.reserve(FACTOR_N);
    std::ifstream name_in("/home/sirocco/yproject/arr_yy/model/model_convert/factor_list_version1.json");
    if (name_in) {
        std::ostringstream ss;
        ss << name_in.rdbuf();
        rapidjson::Document doc;
        doc.Parse(ss.str().c_str());
        if (doc.IsArray()) {
            for (rapidjson::SizeType i = 0; i < doc.Size() && names.size() < FACTOR_N; ++i) {
                if (doc[i].IsString()) {
                    names.emplace_back(doc[i].GetString());
                }
            }
        }
    }
    while (names.size() < FACTOR_N) {
        names.push_back("f" + std::to_string(names.size()));
    }
    out << "stock_id,univ";
    for (const std::string& name : names) {
        out << ',' << name;
    }
    out << '\n';
    out.precision(17);
    for (const SampleRow& row : samples_) {
        out << row.stock_id << ',' << row.univ_name;
        for (int i = 0; i < FACTOR_N; ++i) {
            out << ',';
            if (i < static_cast<int>(row.factors.size())) {
                const double value = row.factors[static_cast<size_t>(i)];
                if (std::isfinite(value)) {
                    out << value;
                } else {
                    out << "nan";
                }
            } else {
                out << "nan";
            }
        }
        out << '\n';
    }
    std::cerr << "lfm factor table " << path << " rows " << samples_.size() << " cols " << FACTOR_N << std::endl;
}

void lfm_parq_stra::flush_end() {
    std::ifstream mem("/proc/self/status");
    std::string line;
    while (std::getline(mem, line)) {
        if (line.compare(0, 6, "VmRSS:") == 0 || line.compare(0, 6, "VmHWM:") == 0 ||
            line.compare(0, 8, "RssAnon:") == 0) {
            std::cerr << "lfm " << line << "\n";
        }
    }
    factors_.dump_mem(std::cerr);
    std::cerr << "lfm samples " << samples_.size() << "\n";
    std::unordered_set<int32_t> ids = g_seen;
    for (const auto& item : g_pending) ids.insert(item.first);
    for (const auto& item : g_stamp) ids.insert(item.first);
    for (int32_t stock : ids) {
        std::vector<TriggerRecorder> due;
        auto it = g_pending.find(stock);
        if (it != g_pending.end()) due.swap(it->second);
        std::vector<TriggerRecorder> fin = triggers_.finish_stock(stock);
        due.insert(due.end(), fin.begin(), fin.end());
        emit_now(stock, std::move(due), g_stamp[stock].ntp);
    }
    g_pending.clear();
    write_samples();
    write_factor_table();
}

void lfm_parq_stra::on_snap_md(const MDStockSnap& snap) {
    before_event(snap.instrumentId, snap.nTP);
    if (factors_.sealed(snap.instrumentId)) {
        return;
    }
    MDStockSnap row = snap;
    if (row.turnover > 0 && turnover_divisor_ > 1) {
        row.turnover /= turnover_divisor_;
    }
    timed(g_snap_fac, [&] { factors_.updateSnap(row); });
}

void lfm_parq_stra::on_order_md(const MDUniOrder& order) {
    before_event(order.instrumentId, order.nTP);
    feed_order(order);
    std::vector<TriggerRecorder> hits;
    timed(g_order_trg, [&] { hits = triggers_.order_trigger(order); });
    emit_now(order.instrumentId, std::move(hits), order.nTP);
}

void lfm_parq_stra::on_trans_md(const MDUniTrans& trans) {
    before_event(trans.instrumentId, trans.nTP);
    std::vector<TriggerRecorder> hits;
    timed(g_trade_trg, [&] { hits = triggers_.trade_trigger(trans); });
    std::vector<TriggerRecorder> cf_hits;
    std::vector<TriggerRecorder> rest;
    for (TriggerRecorder& hit : hits) {
        const bool cf = hit.univ_name == "cf7_i" || hit.univ_name == "cf8_i";
        if (cf) cf_hits.push_back(std::move(hit));
        else rest.push_back(std::move(hit));
    }
    // 确认就发生在这笔成交上。先把这笔写入，再算 cf。之后才到的委托不进这次计算。
    feed_trade(trans);
    if (trans.tickType == SZ_CANCEL && trans.tradeQty > 0 && (trans.bidId != 0 || trans.askId != 0)) {
        MDUniOrder cancel{};
        cancel.instrumentId = trans.instrumentId;
        cancel.nTP = trans.nTP;
        cancel.orderId = trans.bidId != 0 ? trans.bidId : trans.askId;
        cancel.qty = trans.tradeQty;
        cancel.price = trans.tradePrice;
        cancel.tickType = 4;
        cancel.bsFlag = trans.bidId != 0 ? 1 : -1;
        feed_order(cancel);
    }
    emit_now(trans.instrumentId, std::move(cf_hits), trans.nTP);
    emit_now(trans.instrumentId, std::move(rest), trans.nTP);
}

int64_t lfm_parq_stra::trigger_count() const {
    return trigger_count_;
}

void lfm_parq_stra::enable_calc_timing(bool on) {
    factors_.enable_calc_timing(on);
}

void lfm_parq_stra::set_log_hits(bool on) {
    log_hits_ = on;
}

void lfm_parq_stra::dump_event_timing(std::ostream& out) const {
    report_acc(out, "order_factor", g_order_fac);
    report_acc(out, "order_trigger", g_order_trg);
    report_acc(out, "trade_factor", g_trade_fac);
    report_acc(out, "trade_trigger", g_trade_trg);
    report_acc(out, "snap_factor", g_snap_fac);
    report_acc(out, "time_advance_trigger", g_advance_trg);
}

void lfm_parq_stra::dump_calc_timing(std::ostream& out) const {
    factors_.dump_calc_timing(out);
}

namespace {

double hit_percentile(std::vector<double>& xs, double q) {
    if (xs.empty()) {
        return 0;
    }
    const size_t i = static_cast<size_t>(q * static_cast<double>(xs.size() - 1));
    std::nth_element(xs.begin(), xs.begin() + static_cast<std::ptrdiff_t>(i), xs.end());
    return xs[i];
}

void dump_us(std::ostream& out, const char* title, std::vector<double> us) {
    out << title << " n " << us.size();
    if (us.empty()) {
        out << "\n";
        return;
    }
    double sum = 0;
    double mx = 0;
    for (double v : us) {
        sum += v;
        mx = std::max(mx, v);
    }
    const double p50 = hit_percentile(us, 0.50);
    const double p90 = hit_percentile(us, 0.90);
    const double p95 = hit_percentile(us, 0.95);
    const double p99 = hit_percentile(us, 0.99);
    const double p999 = hit_percentile(us, 0.999);
    out << " mean_us " << (sum / static_cast<double>(us.size()))
        << " p50_us " << p50 << " p90_us " << p90 << " p95_us " << p95
        << " p99_us " << p99 << " p999_us " << p999 << " max_us " << mx << "\n";
    const double edges[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000};
    int prev = 0;
    int acc = 0;
    out << title << " hist";
    for (double edge : edges) {
        int n = 0;
        for (double v : us) {
            if (v >= prev && v < edge) {
                ++n;
            }
        }
        acc += n;
        out << " [" << prev << "," << static_cast<int>(edge) << ") " << n;
        prev = static_cast<int>(edge);
    }
    out << " [" << prev << "+) " << (static_cast<int>(us.size()) - acc) << "\n";
}

}

void lfm_parq_stra::dump_hit_timing(std::ostream& out) const {
    std::vector<double> e2e, e2e_full, e2e_reuse, calc, pred, work;
    e2e.reserve(hit_lats_.size());
    for (const HitLat& t : hit_lats_) {
        e2e.push_back(t.e2e_us);
        calc.push_back(t.calc_us);
        pred.push_back(t.pred_us);
        work.push_back(t.calc_us + t.pred_us);
        if (t.full) {
            e2e_full.push_back(t.e2e_us);
        } else {
            e2e_reuse.push_back(t.e2e_us);
        }
    }
    dump_us(out, "hit_e2e", e2e);
    dump_us(out, "hit_e2e_full", e2e_full);
    dump_us(out, "hit_e2e_reuse", e2e_reuse);
    dump_us(out, "hit_calc", calc);
    dump_us(out, "hit_pred", pred);
    dump_us(out, "hit_work", work);
}

void lfm_parq_stra::handle_hits(std::vector<TriggerRecorder>& hits, int32_t actual_time) {
    if (hits.empty()) {
        return;
    }
    struct Done {
        TriggerRecorder* hit = nullptr;
        float pred = 0;
        int passed = 0;
        uint64_t ticket = 0;
        HitLat lat;
        std::vector<double> factors;
    };
    std::vector<Done> done;
    done.reserve(hits.size());
    const uint64_t ticket0 = commit_seq_.fetch_add(static_cast<uint64_t>(hits.size()));
    uint64_t ticket = ticket0;
    for (TriggerRecorder& hit : hits) {
        const auto c0 = std::chrono::steady_clock::now();
        const bool full = factors_.calculatefactor(hit.stock_id, hit.univ_name, hit.trigger_time);
        const auto c1 = std::chrono::steady_clock::now();
        const double* row = factors_.factor_row(hit.univ_name, hit.stock_id);
        int passed = 0;
        const float pred = models_.predict(hit.univ_name, row, &passed);
        const auto c2 = std::chrono::steady_clock::now();
        Done item;
        item.hit = &hit;
        item.pred = pred;
        item.passed = passed;
        item.lat.e2e_us = std::chrono::duration<double, std::micro>(c2 - c0).count();
        item.lat.calc_us = std::chrono::duration<double, std::micro>(c1 - c0).count();
        item.lat.pred_us = std::chrono::duration<double, std::micro>(c2 - c1).count();
        item.lat.full = full;
        if (row != nullptr) {
            item.factors.assign(row, row + FACTOR_N);
        }
        item.ticket = ticket++;
        done.push_back(item);
    }

    for (Done& item : done) {
        std::unique_lock<std::mutex> lock(hit_mu_);
        commit_cv_.wait(lock, [&] { return commit_turn_ == item.ticket; });
        TriggerRecorder& hit = *item.hit;
        const float pred = item.pred;
        const int passed = item.passed;
        hit_lats_.push_back(item.lat);
        hit.set_pred(passed, pred);
        hit.md_time = actual_time;
        int bought = 0;
        int scenario_order = -1;
        auto lit = limits_.find(hit.univ_name);
        if (lit != limits_.end()) {
            scenario_order = lit->second.order;
            if (passed && lit->second.used < lit->second.cap &&
                lit->second.book_bought != nullptr && *lit->second.book_bought < lit->second.book_target) {
                bought = 1;
                lit->second.used += 1;
                *lit->second.book_bought += 1;
            }
        }
        SampleRow sample;
        sample.stock_id = hit.stock_id;
        sample.univ_name = hit.univ_name;
        sample.trigger_idx = hit.has_order_id ? hit.order_id : hit.trade_id;
        sample.trigger_time = hit.trigger_time;
        sample.actual_time = actual_time;
        sample.pred_value = pred;
        sample.scenario_order = scenario_order;
        sample.bought = bought;
        sample.order_data_idx = hit.order_data_idx;
        sample.trade_data_idx = hit.trade_data_idx;
        sample.factors = std::move(item.factors);
        samples_.push_back(std::move(sample));
        if (hit.close_gate) {
            triggers_.close_gate(hit.stock_id, hit.univ_name);
        }
        if (log_hits_) {
            std::cout << hit.stock_id << "," << hit.trigger_time << "," << hit.univ_name
                      << "," << hit.pred_value << "," << bought << "\n";
        }
        trigger_count_ += 1;
        commit_turn_ += 1;
        lock.unlock();
        commit_cv_.notify_all();
    }
}
