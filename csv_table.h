#pragma once

#include "lfm_types.h"

#include <string>
#include <unordered_map>
#include <vector>

// 日频盘前 csv：首列是 6 位代码，其余列是字段。空表头列名忽略。
struct CsvTable {
    std::vector<std::string> columns;
    std::unordered_map<std::string, std::vector<double>> rows;
};

bool load_csv_table(const std::string& path, CsvTable& out);
