#pragma once

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

// 编 so 时打开。外壳编译不定义它，只调用已经导出的符号。
#if defined(LFM_BUILD_SO)
#define LFM_API __attribute__((visibility("default")))
#else
#define LFM_API
#endif

// 盘前 144，然后 snap、trans、ot、otd、sot2。长度写死。
inline constexpr int LAST_FACTOR_N = 144;
inline constexpr int SNAP_FACTOR_N = 94;
inline constexpr int TRANS_FACTOR_N = 49;
inline constexpr int OT_FACTOR_N = 111;
inline constexpr int OTD_FACTOR_N = 479;
inline constexpr int SOT_FACTOR_N = 53;
inline constexpr int T_FACTOR_N =
    SNAP_FACTOR_N + TRANS_FACTOR_N + OT_FACTOR_N + OTD_FACTOR_N + SOT_FACTOR_N;
inline constexpr int FACTOR_N = LAST_FACTOR_N + T_FACTOR_N;

inline constexpr int SNAP_OFFSET = LAST_FACTOR_N;
inline constexpr int TRANS_OFFSET = SNAP_OFFSET + SNAP_FACTOR_N;
inline constexpr int OT_OFFSET = TRANS_OFFSET + TRANS_FACTOR_N;
inline constexpr int OTD_OFFSET = OT_OFFSET + OT_FACTOR_N;
inline constexpr int SOT_OFFSET = OTD_OFFSET + OTD_FACTOR_N;

inline constexpr int T_0930 = 93000000;

using StockId = int32_t;

inline constexpr int kMaxStockId = 1000000;

inline bool valid_stock(StockId id) {
    return id >= 0 && id < kMaxStockId;
}

inline double lfm_nan() {
    return std::numeric_limits<double>::quiet_NaN();
}

inline std::string stock_code6(StockId id) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06d", id);
    return buf;
}
