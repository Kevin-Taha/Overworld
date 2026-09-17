#pragma once

#include "TextDelivery.h"

#include <Windows.h>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace gamepad {
// Kernel window metadata only; no messages, accessibility calls or value reads.
PredictionSupport prediction_target_support(TextWindow target);

class PredictionTarget {
public:
    ~PredictionTarget();
    void start();
    void close();
    PredictionEvidence observe(const TextTarget& target, std::uint64_t revision, Tick now);

private:
    struct Request {
        TextTarget target;
        std::uint64_t revision = 0;
        std::uint64_t generation = 0;
        bool operator==(const Request&) const = default;
    };
    struct Result {
        Request request;
        PredictionEvidence evidence;
    };
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<Request> request_;
    std::optional<Result> result_;
    std::uint64_t generation_ = 0;
    std::jthread worker_;
};
} // namespace gamepad
