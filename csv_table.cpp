#include "csv_table.h"

#include <fstream>
#include <sstream>

namespace {

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> cells;
    std::string cell;
    std::stringstream ss(line);
    while (std::getline(ss, cell, ',')) {
        if (!cell.empty() && cell.back() == '\r') {
            cell.pop_back();
        }
        cells.push_back(cell);
    }
    return cells;
}

double parse_cell(const std::string& text) {
    if (text.empty() || text == "nan" || text == "NaN") {
        return lfm_nan();
    }
    try {
        return std::stod(text);
    } catch (...) {
        return lfm_nan();
    }
}

}

bool load_csv_table(const std::string& path, CsvTable& out) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string line;
    if (!std::getline(in, line)) {
        return false;
    }
    auto header = split_csv(line);
    if (header.empty()) {
        return false;
    }
    out.columns.assign(header.begin() + 1, header.end());
    out.rows.clear();
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        auto cells = split_csv(line);
        if (cells.empty()) {
            continue;
        }
        std::string code = cells[0];
        if (code.size() < 6) {
            code = std::string(6 - code.size(), '0') + code;
        } else if (code.size() > 6) {
            code = code.substr(0, 6);
        }
        std::vector<double> values(out.columns.size(), lfm_nan());
        for (size_t i = 0; i < out.columns.size() && i + 1 < cells.size(); ++i) {
            values[i] = parse_cell(cells[i + 1]);
        }
        out.rows.emplace(std::move(code), std::move(values));
    }
    return true;
}
