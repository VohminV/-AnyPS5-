#include "CpuBackend/Schedule.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

namespace AgcDriver::CpuBackend {

struct ThreadPool::Impl {
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable done;
    std::atomic<std::size_t> pending{0};
    bool stop = false;
    std::size_t threads = 0;
};

ThreadPool::ThreadPool(std::size_t threads) {
    if (threads == 0) {
        threads = SuggestThreads();
    }
    if (threads == 0) {
        threads = 1;
    }
    if (threads > 64) {
        threads = 64;
    }
    impl_ = new Impl{};
    impl_->threads = threads;
    if (threads == 1) {
        return;
    }
    for (std::size_t i = 0; i < threads; ++i) {
        impl_->workers.emplace_back([impl = impl_]() {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(impl->mutex);
                    impl->ready.wait(lock, [impl]() { return impl->stop || !impl->tasks.empty(); });
                    if (impl->stop && impl->tasks.empty()) {
                        return;
                    }
                    task = std::move(impl->tasks.front());
                    impl->tasks.pop();
                }
                task();
                if (impl->pending.fetch_sub(1) == 1) {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->done.notify_all();
                }
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->workers.empty()) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->stop = true;
        }
        impl_->ready.notify_all();
        for (auto& worker : impl_->workers) {
            worker.join();
        }
    }
    delete impl_;
}

std::size_t ThreadPool::Threads() const {
    return impl_ != nullptr ? impl_->threads : 0;
}

void ThreadPool::ForEach(std::size_t items, const std::function<void(std::size_t begin, std::size_t end)>& work) {
    if (items == 0) {
        return;
    }
    if (impl_ == nullptr || impl_->threads <= 1) {
        work(0, items);
        return;
    }
    std::size_t parts = impl_->threads;
    if (parts > items) {
        parts = items;
    }
    auto ranges = SplitRange(items, parts);
    impl_->pending.store(ranges.size());
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (auto& range : ranges) {
            impl_->tasks.emplace([work, range]() { work(range.first, range.second); });
        }
    }
    impl_->ready.notify_all();
    {
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->done.wait(lock, [this]() { return impl_->pending.load() == 0; });
    }
}

std::size_t SuggestThreads() {
    unsigned detected = std::thread::hardware_concurrency();
    if (detected <= 1) {
        return 1;
    }
    unsigned usable = detected > 1 ? detected - 1 : 1;
    if (usable > 8) {
        usable = 8;
    }
    return static_cast<std::size_t>(usable);
}

bool RangesIndependent(std::size_t beginA, std::size_t endA, std::size_t beginB, std::size_t endB) {
    return endA <= beginB || endB <= beginA;
}

std::vector<std::pair<std::size_t, std::size_t>> SplitRange(std::size_t total, std::size_t parts) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    if (parts == 0 || total == 0) {
        return out;
    }
    if (parts > total) {
        parts = total;
    }
    out.reserve(parts);
    std::size_t base = total / parts;
    std::size_t rest = total % parts;
    std::size_t cursor = 0;
    for (std::size_t i = 0; i < parts; ++i) {
        std::size_t size = base + (i < rest ? 1 : 0);
        out.emplace_back(cursor, cursor + size);
        cursor += size;
    }
    return out;
}

} // namespace AgcDriver::CpuBackend
