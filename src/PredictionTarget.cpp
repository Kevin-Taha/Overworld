#include "PredictionTarget.h"

#include <chrono>
#include <algorithm>
#include <cwchar>

namespace gamepad {
namespace {
Tick clock_now()
{
    return static_cast<Tick>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool same_window(TextWindow target)
{
    DWORD process = 0;
    const auto thread = GetWindowThreadProcessId(reinterpret_cast<HWND>(target.window), &process);
    return target.window && process == target.process && thread && thread == target.thread;
}
}

PredictionSupport prediction_target_support(TextWindow target)
{
    if (!same_window(target)) { return PredictionSupport::Unavailable; }
    const auto window = reinterpret_cast<HWND>(target.window);
    wchar_t name[32]{}, real_name[32]{};
    if (!GetClassNameW(window, name, 32) || !RealGetWindowClassW(window, real_name, 32)) {
        return PredictionSupport::Unavailable;
    }
    if (_wcsicmp(name, L"Edit") != 0 || _wcsicmp(real_name, L"Edit") != 0) {
        return PredictionSupport::Unsupported;
    }
    SetLastError(ERROR_SUCCESS);
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    if (!style && GetLastError() != ERROR_SUCCESS) { return PredictionSupport::Unavailable; }
    if (style & ES_PASSWORD) { return PredictionSupport::Protected; }
    // Unlike RichEdit, the system multiline Edit does not support password masking.
    if (!(style & ES_MULTILINE) || !(style & WS_CHILD) || (style & (ES_READONLY | WS_DISABLED))
        || !IsWindowVisible(window) || !IsWindowEnabled(window)) { return PredictionSupport::Unsupported; }
    return same_window(target) ? PredictionSupport::PlainEdit : PredictionSupport::Unavailable;
}

PredictionTarget::~PredictionTarget()
{
    worker_.request_stop();
    wake_.notify_all();
}

void PredictionTarget::start()
{
    worker_ = std::jthread([this](std::stop_token stop) {
        std::unique_lock lock(mutex_);
        while (!stop.stop_requested()) {
            wake_.wait(lock, stop, [&] { return request_.has_value(); });
            if (stop.stop_requested()) { break; }
            const auto request = *request_;
            request_.reset();
            lock.unlock();
            PredictionEvidence evidence;
            evidence.sampled_at = clock_now();
            evidence.revision = request.revision;
            evidence.support = prediction_target_support(request.target.windows[2]);
            if (evidence.support == PredictionSupport::PlainEdit) {
                const auto deadline = evidence.sampled_at + 60;
                const auto query = [&](UINT message, DWORD_PTR& result) {
                    const auto time = clock_now();
                    if (stop.stop_requested() || time >= deadline) { return false; }
                    const auto timeout = static_cast<UINT>((std::min)(Tick{20}, deadline - time));
                    return SendMessageTimeoutW(reinterpret_cast<HWND>(request.target.windows[2].window),
                        message, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, timeout, &result) != 0;
                };
                DWORD_PTR before = 0, selection = 0, after = 0;
                // Some classic Edit implementations wrap EM_GETSEL rather than
                // returning -1 above 65535. Never trust the packed result alone.
                // Scalar length/selection messages use no caller-owned text buffers.
                if (!query(WM_GETTEXTLENGTH, before) || before >= 65535
                    || !query(EM_GETSEL, selection) || !query(WM_GETTEXTLENGTH, after)
                    || before != after || after >= 65535 || LOWORD(selection) > after || HIWORD(selection) > after
                    || clock_now() > deadline) {
                    evidence.support = PredictionSupport::Unavailable;
                } else if (LOWORD(selection) == HIWORD(selection)) {
                    evidence.caret = LOWORD(selection);
                }
                const auto final = prediction_target_support(request.target.windows[2]);
                if (final != PredictionSupport::PlainEdit) { evidence.support = final; evidence.caret.reset(); }
            }
            lock.lock();
            if (request.generation == generation_) { result_ = Result{request, evidence}; }
        }
    });
}

void PredictionTarget::close()
{
    std::lock_guard lock(mutex_);
    ++generation_;
    request_.reset();
    result_.reset();
}

PredictionEvidence PredictionTarget::observe(const TextTarget& target, std::uint64_t revision, Tick now)
{
    const auto support = prediction_target_support(target.windows[2]);
    if (support != PredictionSupport::PlainEdit) { close(); return {support}; }
    std::lock_guard lock(mutex_);
    const Request desired{target, revision, generation_};
    const bool matching = result_ && result_->request == desired;
    PredictionEvidence evidence{support};
    if (matching && result_->evidence.sampled_at <= now && now - result_->evidence.sampled_at <= 100) {
        evidence = result_->evidence;
    }
    if (!matching || now < result_->evidence.sampled_at || now - result_->evidence.sampled_at >= 16) {
        request_ = desired;
        wake_.notify_one();
    }
    return evidence;
}
} // namespace gamepad
