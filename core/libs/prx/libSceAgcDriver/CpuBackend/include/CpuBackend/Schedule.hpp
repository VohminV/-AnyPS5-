#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace AgcDriver::CpuBackend {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    std::size_t Threads() const;
    void ForEach(std::size_t items, const std::function<void(std::size_t begin, std::size_t end)>& work);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

std::size_t SuggestThreads();
bool RangesIndependent(std::size_t beginA, std::size_t endA, std::size_t beginB, std::size_t endB);
std::vector<std::pair<std::size_t, std::size_t>> SplitRange(std::size_t total, std::size_t parts);

} // namespace AgcDriver::CpuBackend
