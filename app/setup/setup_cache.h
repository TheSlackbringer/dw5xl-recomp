#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include <atomic>
#include <optional>

namespace dw5xl::setup {
struct CacheInput
{
    std::string name;
    std::filesystem::path path;
    std::optional<std::string> content; // logical inputs such as relative tool-link configuration
};
// Physical input paths are not keys: moving identical packages must preserve semantic identity.
std::string cacheKey(const std::vector<CacheInput> &inputs, const std::map<std::string,std::string> &values,
                      std::string &error);
bool cachedOutputs(const std::filesystem::path &record, const std::string &key,
                    const std::filesystem::path &output, std::string &reason);
bool publishCachedOutputs(const std::filesystem::path &record, const std::string &key,
                           const std::filesystem::path &output, std::string &error);
bool atomicWrite(const std::filesystem::path &path, const std::string &bytes, std::string &error);
std::string fileDigest(const std::filesystem::path &path, std::string &error);
bool translatorInputs(const std::filesystem::path &program, const std::filesystem::path &workingDirectory,
                       const std::filesystem::path &log, std::vector<CacheInput> &inputs, std::string &error,
                       const std::atomic<bool> *cancel=nullptr);
bool vuRuntimeInputs(std::vector<CacheInput> &inputs, std::string &error);
const char *vuProducerIdentity();
bool treeInputs(const std::filesystem::path &root,const std::string &label,std::vector<CacheInput> &inputs,std::string &error);
std::string packageIdentity(const std::filesystem::path &root,std::string &error);
bool buildReady(const std::filesystem::path &root,const std::filesystem::path &build,
                 const std::string &release,std::string &error);

class SetupLease
{
public:
    static std::unique_ptr<SetupLease> acquire(const std::filesystem::path &data, std::string &error);
    ~SetupLease();
    SetupLease(const SetupLease &)=delete;
    SetupLease &operator=(const SetupLease &)=delete;
private:
    SetupLease()=default;
    intptr_t m_handle=-1;
};
} // namespace dw5xl::setup
