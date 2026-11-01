#pragma once

#include "lfm_types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// 按启动时选中的账本加载。因子行已经是模型输入顺序，直接送进该场景的 3 棵树。
class LFM_API ModelManager {
public:
    using PredictFn = void (*)(double* data, double* result, int32_t start, int32_t end);

    ModelManager() = default;
    ~ModelManager();

    ModelManager(const ModelManager&) = delete;
    ModelManager& operator=(const ModelManager&) = delete;

    void init(const std::string& lfm_dir, const std::vector<std::string>& books);
    bool ready() const { return ready_; }

    // 返回三棵树均值。passed 为均值是否大于该场景阈值。找不到场景时返回 NaN。
    float predict(const std::string& univ, const double* row, int* passed) const;

private:
    void load_book(const std::string& lfm_dir, const std::string& book);

    struct Scenario {
        float threshold = 0;
        PredictFn fn[3] = {};
        void* handle[3] = {};
    };

    std::unordered_map<std::string, Scenario> scenarios_;
    bool ready_ = false;
};
