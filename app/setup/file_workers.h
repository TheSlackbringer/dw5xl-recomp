#pragma once
// Bounded independent file work. Completion order never defines cache bytes or source layout.
#include <algorithm>
#include <atomic>
#include <exception>
#include <thread>
#include <vector>

namespace dw5xl::setup::detail {
template<class Work> void parallelFiles(size_t count,Work work)
{
    const size_t workers=std::min<size_t>(count,std::min<unsigned>(8,std::max(1u,std::thread::hardware_concurrency())));
    std::atomic<size_t> next{0};std::atomic<bool> failed{false};
    std::vector<std::exception_ptr> errors(count);
    {
        std::vector<std::jthread> pool;
        for(size_t i=0;i<workers;++i) pool.emplace_back([&] {
            while(!failed)
            {
                const auto index=next.fetch_add(1);
                if(index>=count) break;
                try {work(index);}
                catch(...) {errors[index]=std::current_exception();failed=true;}
            }
        });
    } // every worker joins before errors, metadata, cleanup or publication reach the caller
    for(const auto &error:errors) if(error) std::rethrow_exception(error);
}
} // namespace dw5xl::setup::detail
