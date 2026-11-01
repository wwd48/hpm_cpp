#!/bin/bash
# 一天的回测：先把 parquet 写进共享内存和 global_index，再由 lfm_stra 读完。
set -euo pipefail
if [[ $# -lt 1 ]]; then
    echo "usage: lfm_backtest.sh <tradingday> [other_config.json] [live_config.json] [books] [end_ntp]" >&2
    exit 1
fi
tradingday=$1
config=${2:-other_config.json}
live_config=${3:-live_config.json}
books=${4:-DE}
end_ntp=${5:-}
here=$(cd "$(dirname "$0")" && pwd)
cell=$(cd "$here/../../sp_cpp/cell" && pwd)
sim_md=$cell/app/unitTest/sim_md/sim_md
conf=$cell/app/unitTest/sim_md/conf.json
lfm=$here/lfm_stra
if [[ -n "$end_ntp" ]]; then
    "$sim_md" "$conf" "$tradingday" "$end_ntp"
    "$lfm" "$tradingday" replay "$config" "$live_config" "$books" "$end_ntp"
else
    "$sim_md" "$conf" "$tradingday"
    "$lfm" "$tradingday" replay "$config" "$live_config" "$books"
fi
