#include "lfm_strategy.h"

#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

bool is_books(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (char ch : text) {
        if (ch < 'A' || ch > 'Z') {
            return false;
        }
    }
    return true;
}

std::vector<std::string> books_from(const std::string& text) {
    std::string seen;
    for (char ch : text) {
        if (seen.find(ch) != std::string::npos) {
            throw std::runtime_error(std::string("duplicate book: ") + ch);
        }
        seen.push_back(ch);
    }
    std::sort(seen.begin(), seen.end());
    std::vector<std::string> books;
    books.reserve(seen.size());
    for (char ch : seen) {
        books.emplace_back(1, ch);
    }
    return books;
}

bool all_digits(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir.back() == '/') {
        return dir + name;
    }
    return dir + "/" + name;
}

struct OtherPaths {
    std::string daily_dir;
    std::string output_dir;
    std::string lfm_dir;
};

std::string required_string(const rapidjson::Document& doc, const char* key, const std::string& path) {
    if (!doc.HasMember(key) || !doc[key].IsString() || doc[key].GetStringLength() == 0) {
        throw std::runtime_error(std::string("other_config ") + key + " must be a non-empty string: " + path);
    }
    return doc[key].GetString();
}

OtherPaths load_other_paths(const std::string& path, const std::string& tradingday) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("open other_config failed: " + path);
    }
    rapidjson::IStreamWrapper stream(in);
    rapidjson::Document doc;
    doc.ParseStream(stream);
    if (doc.HasParseError() || !doc.IsObject()) {
        throw std::runtime_error("parse other_config failed: " + path);
    }
    OtherPaths paths;
    paths.daily_dir = join_path(required_string(doc, "daily_input_path", path), tradingday);
    paths.output_dir = join_path(required_string(doc, "output_path", path), tradingday);
    paths.lfm_dir = required_string(doc, "lfm_input_path", path);
    return paths;
}

int load_strategy_id(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("open live_config failed: " + path);
    }
    rapidjson::IStreamWrapper stream(in);
    rapidjson::Document doc;
    doc.ParseStream(stream);
    if (doc.HasParseError() || !doc.IsObject()) {
        throw std::runtime_error("parse live_config failed: " + path);
    }
    if (!doc.HasMember("strategy_id")) {
        return 2;
    }
    if (!doc["strategy_id"].IsInt() || doc["strategy_id"].GetInt() < 0) {
        throw std::runtime_error("live_config strategy_id must be a non-negative integer");
    }
    return doc["strategy_id"].GetInt();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: lfm_stra <tradingday> [replay|live] [other_config.json] [live_config.json] [books] [end_ntp]\n"
                  << "  mode defaults to live; books defaults to DE, for example D, E, or DFG\n"
                  << "  other_config.json defaults to ./other_config.json\n"
                  << "  live_config.json defaults to ./live_config.json\n"
                  << "  daily dir is daily_input_path/tradingday\n"
                  << "  output dir is output_path/tradingday/<books>\n"
                  << "  models are lfm_input_path/model_param_<book>.json and the named .so files\n"
                  << "  strategy_id in live_config.json defaults to 2\n";
        return 1;
    }
    const std::string tradingday = argv[1];
    std::string mode = "live";
    int index = 2;
    if (index < argc) {
        const std::string arg = argv[index];
        if (arg == "replay" || arg == "live") {
            mode = arg;
            ++index;
        }
    }
    std::string other_config_path = "other_config.json";
    std::string live_config_path = "live_config.json";
    std::string books_text = "DE";
    bool books_set = false;
    std::vector<std::string> config_paths;
    int32_t end_ntp = 0;
    for (int i = index; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 == argc && all_digits(arg)) {
            end_ntp = std::atoi(arg.c_str());
            continue;
        }
        if (is_books(arg)) {
            if (books_set) {
                std::cerr << "usage: lfm_stra <tradingday> [replay|live] [other_config.json] [live_config.json] [books] [end_ntp]\n";
                return 1;
            }
            books_text = arg;
            books_set = true;
            continue;
        }
        config_paths.push_back(arg);
    }
    if (config_paths.size() > 2) {
        std::cerr << "usage: lfm_stra <tradingday> [replay|live] [other_config.json] [live_config.json] [books] [end_ntp]\n";
        return 1;
    }
    if (config_paths.size() >= 1) {
        other_config_path = config_paths[0];
    }
    if (config_paths.size() == 2) {
        live_config_path = config_paths[1];
    }
    const bool live = mode == "live";

    try {
        const std::vector<std::string> books = books_from(books_text);
        std::string book_tag;
        for (const std::string& book : books) {
            book_tag += book;
        }
        OtherPaths paths = load_other_paths(other_config_path, tradingday);
        paths.output_dir = join_path(paths.output_dir, book_tag);
        const int strategy_id = load_strategy_id(live_config_path);
        lfm_strategy stra(strategy_id);
        if (end_ntp > 0) {
            stra.set_end_ntp(end_ntp);
        }
        stra.init(tradingday, paths.daily_dir, paths.output_dir, paths.lfm_dir, books);
        if (live) {
            std::cerr << "lfm live global_index" << std::endl;
            stra.start_work();
            while (!stra.finished()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            stra.stop();
        } else {
            std::cerr << "lfm replay global_index" << std::endl;
            stra.replay_global_index();
        }
    } catch (const std::exception& ex) {
        std::cerr << "lfm strategy failed: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}
