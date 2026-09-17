#include "InstanceGuard.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void check(bool value, const char* reason)
{
    if (!value) { throw std::runtime_error(reason); }
}
class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_) { CloseHandle(value_); } }
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};
std::wstring unique_name(std::wstring_view suffix)
{
    LARGE_INTEGER counter{};
    check(QueryPerformanceCounter(&counter) != FALSE, "unique-name clock unavailable");
    return L"Local\\GamepadDesktop.OwnedMutexTests." + std::to_wstring(GetCurrentProcessId())
        + L"." + std::to_wstring(counter.QuadPart) + L"." + std::wstring(suffix);
}
void rejected_guard_does_not_retain_name()
{
    const auto name = unique_name(L"rejected");
    auto owner = std::make_unique<gamepad::MapperInstanceGuard>(name.c_str());
    check(owner->acquired() && !owner->already_running(), "first uniquely named owner acquires");
    gamepad::MapperInstanceGuard rejected(name.c_str());
    check(!rejected.acquired() && rejected.already_running(), "second guard rejects while owner exists");
    owner.reset();
    gamepad::MapperInstanceGuard replacement(name.c_str());
    check(replacement.acquired() && !replacement.already_running(),
        "replacement must acquire while the rejected guard object is still alive");
    check(rejected.already_running() && !rejected.acquired(), "rejected object retains diagnostic state only");
}
void ownership_is_released_not_abandoned()
{
    const auto name = unique_name(L"ownership");
    auto owner = std::make_unique<gamepad::MapperInstanceGuard>(name.c_str());
    check(owner->acquired(), "owner acquires for release test");
    Handle observer(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name.c_str()));
    check(observer.get() != nullptr, "owned observer opens mutex");
    DWORD before = WAIT_FAILED;
    std::thread held([&] { before = WaitForSingleObject(observer.get(), 0); });
    held.join();
    check(before == WAIT_TIMEOUT, "guard owns mutex until destruction");
    owner.reset();
    DWORD after = WAIT_FAILED;
    BOOL released = FALSE;
    std::thread available([&] {
        after = WaitForSingleObject(observer.get(), 0);
        if (after == WAIT_OBJECT_0 || after == WAIT_ABANDONED) { released = ReleaseMutex(observer.get()); }
    });
    available.join();
    check(after == WAIT_OBJECT_0 && released, "destructor must release ownership cleanly, not abandon it");
}
void failure_is_not_an_existing_instance()
{
    const auto name = unique_name(L"type-conflict");
    {
        Handle event(CreateEventW(nullptr, FALSE, FALSE, name.c_str()));
        check(event.get() != nullptr, "owned named event created");
        gamepad::MapperInstanceGuard failed(name.c_str());
        check(!failed.acquired() && !failed.already_running(), "CreateMutex failure is distinct from another mapper");
    }
    gamepad::MapperInstanceGuard replacement(name.c_str());
    check(replacement.acquired() && !replacement.already_running(), "failed creation retains no name or ownership");
}
}
int main()
{
    try {
        rejected_guard_does_not_retain_name();
        ownership_is_released_not_abandoned();
        failure_is_not_an_existing_instance();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "PASS uniquely named owned mutex rejection, destruction, and failure lifetimes; no mapper mutex or input used.\n";
}
