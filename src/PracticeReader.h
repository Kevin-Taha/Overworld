#pragma once

#include "PracticeWindow.h"

namespace gamepad::practice {
class ReaderBackend {
public:
    virtual ~ReaderBackend() = default;
    virtual Tick now() = 0;
    virtual bool focused(HWND window) = 0;
    virtual Controller slot(unsigned index) = 0;
};

// The worker retains its backend/state on a native-call timeout, but stop is terminal:
// no subsequent slot or batch can start. The practice process exits after a stop failure.
class Reader final : public ControllerSource {
public:
    explicit Reader(std::shared_ptr<ReaderBackend> backend);
    ~Reader() override;
    bool start();
    bool stop();
    [[nodiscard]] bool stopped() const;
    [[nodiscard]] std::uint64_t publications() const;
    [[nodiscard]] const std::wstring& error() const { return error_; }
    void authorize(HWND window) override;
    Snapshot read() override;
    Tick now() override;
private:
    struct State;
    static DWORD WINAPI worker(void* context);
    std::shared_ptr<State> state_;
    HANDLE thread_ = nullptr;
    bool start_attempted_ = false, stop_attempted_ = false, stop_result_ = false;
    std::wstring error_;
};
} // namespace gamepad::practice
