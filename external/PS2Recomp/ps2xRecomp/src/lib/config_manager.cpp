#include "ps2recomp/config_manager.h"
#include "ps2recomp/recompiler_reporter.h"
#include <toml.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <limits>
#include <thread>
#include <cctype>
#include <unordered_set>

namespace ps2recomp
{

    ConfigManager::ConfigManager(const std::string &configPath)
        : m_configPath(configPath)
    {
    }

    ConfigManager::~ConfigManager() = default;

    void ConfigManager::setReporter(RecompilerReporter *reporter)
    {
        m_reporter = reporter;
    }

    RecompilerConfig ConfigManager::loadConfig() const
    {
        RecompilerConfig config;

        try
        {
            if (m_reporter)
            {
                m_reporter->info("config", "Parsing toml file: " + m_configPath);
            }
            auto data = toml::parse(m_configPath);
            const auto &general = toml::find(data, "general");

            config.inputPath = toml::find<std::string>(general, "input");
            config.inputFormat = toml::find_or<std::string>(general, "input_format", "elf");
            config.moduleId = toml::find_or<std::string>(general, "module_id", "");
            const int64_t rawBase = toml::find_or<int64_t>(general, "raw_base", 0);
            if (rawBase < 0 || rawBase > std::numeric_limits<uint32_t>::max())
                throw std::runtime_error("raw_base is outside the guest address range");
            config.rawBase = static_cast<uint32_t>(rawBase);
            const int64_t rawCodeOffset = toml::find_or<int64_t>(general, "raw_code_offset", 0);
            if (rawCodeOffset < 0 || rawCodeOffset > std::numeric_limits<uint32_t>::max() ||
                (rawCodeOffset & 3) != 0)
                throw std::runtime_error("raw_code_offset must be an aligned guest-sized offset");
            config.rawCodeOffset = static_cast<uint32_t>(rawCodeOffset);
            if (config.inputFormat != "elf" && config.inputFormat != "raw")
                throw std::runtime_error("input_format must be elf or raw");
            if (config.inputFormat == "raw")
            {
                if (config.moduleId.empty() || !std::isalpha(static_cast<unsigned char>(config.moduleId.front())) ||
                    !std::all_of(config.moduleId.begin(), config.moduleId.end(), [](unsigned char c) {
                        return std::isalnum(c) || c == '_';
                    }))
                    throw std::runtime_error("raw module_id must be a C++ identifier starting with a letter");
                if ((config.rawBase & 3u) != 0u)
                    throw std::runtime_error("raw_base must be four-byte aligned");
            }
            else if (!config.moduleId.empty())
                throw std::runtime_error("module_id requires raw input_format");
            if (data.contains("code_copies"))
            {
                std::unordered_set<std::string> names;
                for (const auto &item : toml::find<toml::array>(data, "code_copies"))
                {
                    CodeCopy copy;
                    copy.name = toml::find<std::string>(item, "name");
                    if (copy.name.empty() || !std::isalpha(static_cast<unsigned char>(copy.name.front())) ||
                        !std::all_of(copy.name.begin(), copy.name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; }) ||
                        !names.insert(copy.name).second)
                        throw std::runtime_error("Code copy names must be unique C++ identifiers");
                    const auto address = [&](const char *key) {
                        const int64_t v = toml::find<int64_t>(item, key);
                        if (v < 0 || v > 0xffffffffll || (v & 3))
                            throw std::runtime_error(std::string("Invalid code copy ") + key);
                        return static_cast<uint32_t>(v);
                    };
                    copy.source = address("source"); copy.destination = address("destination"); copy.size = address("size");
                    if (!copy.size || uint64_t(copy.source) + copy.size > 0x100000000ull ||
                        uint64_t(copy.destination) + copy.size > 0x100000000ull)
                        throw std::runtime_error("Code copy range overflows or is empty");
                    std::vector<std::pair<uint32_t,uint32_t>> ranges;
                    for (const auto &range : toml::find<std::vector<std::vector<int64_t>>>(item, "functions"))
                    {
                        if (range.size() != 2 || range[0] < 0 || range[1] <= 0 ||
                            ((range[0] | range[1]) & 3) || uint64_t(range[0]) + range[1] > copy.size)
                            throw std::runtime_error("Code copy function range must be aligned and inside its image");
                        ranges.emplace_back(static_cast<uint32_t>(range[0]),static_cast<uint32_t>(range[1]));
                    }
                    std::sort(ranges.begin(), ranges.end());
                    for (size_t i = 1; i < ranges.size(); ++i)
                        if (uint64_t(ranges[i-1].first) + ranges[i-1].second > ranges[i].first)
                            throw std::runtime_error("Code copy function ranges overlap");
                    if (ranges.empty()) throw std::runtime_error("Code copy requires explicit function ranges");
                    copy.functions = std::move(ranges);
                    config.codeCopies.push_back(std::move(copy));
                }
            }
            config.ghidraMapPath = toml::find_or<std::string>(general, "ghidra_output", "");
            config.outputPath = toml::find<std::string>(general, "output");
            config.singleFileOutput = toml::find_or<bool>(general, "single_file_output", false);
            config.lowMemoryMode = toml::find_or<bool>(general, "low_memory_mode", config.lowMemoryMode);
            const int64_t configuredOutputWorkers = toml::find_or<int64_t>(
                general,
                "output_worker_threads",
                toml::find_or<int64_t>(general, "output_worker_thread", config.outputWorkerThreads));
            const int64_t clampedOutputWorkers = std::clamp<int64_t>(
                configuredOutputWorkers,
                0,
                std::thread::hardware_concurrency() * 2);
            if (configuredOutputWorkers != clampedOutputWorkers)
            {
                if (m_reporter)
                {
                    std::ostringstream msg;
                    msg << "output_worker_threads value " << configuredOutputWorkers
                        << " is out of range; clamped to " << clampedOutputWorkers << ".";
                    m_reporter->warning("config", msg.str());
                }
            }
            config.outputWorkerThreads = static_cast<uint32_t>(clampedOutputWorkers);
            config.patchSyscalls = toml::find_or<bool>(general, "patch_syscalls", config.patchSyscalls);
            config.patchCop0 = toml::find_or<bool>(general, "patch_cop0", config.patchCop0);
            config.patchCache = toml::find_or<bool>(general, "patch_cache", config.patchCache);

            if (general.contains("stubs") && general.at("stubs").is_array())
            {
                config.stubImplementations = toml::find<std::vector<std::string>>(general, "stubs");
            }
            else if (data.contains("stubs") && data.at("stubs").is_array())
            {
                config.stubImplementations = toml::find<std::vector<std::string>>(data, "stubs");
            }

            auto appendEntryPointHints = [&](const toml::value &table, const char *key)
            {
                if (!table.contains(key) || !table.at(key).is_array())
                {
                    return;
                }
                const auto values = toml::find<std::vector<std::string>>(table, key);
                config.entryPointHints.insert(
                    config.entryPointHints.end(), values.begin(), values.end());
            };
            appendEntryPointHints(general, "entry_points");
            appendEntryPointHints(data, "entry_points");
            // Backward compatibility
            appendEntryPointHints(general, "untracked_stubs");
            appendEntryPointHints(data, "untracked_stubs");

            std::sort(config.entryPointHints.begin(), config.entryPointHints.end());
            config.entryPointHints.erase(
                std::unique(config.entryPointHints.begin(), config.entryPointHints.end()),
                config.entryPointHints.end());

            if (general.contains("skip") && general.at("skip").is_array())
            {
                config.skipFunctions = toml::find<std::vector<std::string>>(general, "skip");
            }
            else if (data.contains("skip") && data.at("skip").is_array())
            {
                config.skipFunctions = toml::find<std::vector<std::string>>(data, "skip");
            }

            if (data.contains("patches") && data.at("patches").is_table())
            {
                const auto &patches = toml::find(data, "patches");

                if (patches.contains("instructions") && patches.at("instructions").is_array())
                {
                    const auto &instPatches = toml::find(patches, "instructions").as_array();
                    for (const auto &patch : instPatches)
                    {
                        if (patch.contains("address") && patch.contains("value"))
                        {
                            uint32_t address = 0;
                            const auto &addressValue = patch.at("address");
                            if (addressValue.is_string())
                            {
                                address = std::stoul(toml::find<std::string>(patch, "address"), nullptr, 0);
                            }
                            else if (addressValue.is_integer())
                            {
                                address = static_cast<uint32_t>(toml::find<int64_t>(patch, "address"));
                            }
                            else
                            {
                                continue;
                            }

                            const auto &valueField = patch.at("value");
                            if (valueField.is_string())
                            {
                                config.patches[address] = toml::find<std::string>(patch, "value");
                            }
                            else if (valueField.is_integer())
                            {
                                std::ostringstream valueStream;
                                valueStream << "0x" << std::hex
                                            << static_cast<uint32_t>(toml::find<int64_t>(patch, "value"));
                                config.patches[address] = valueStream.str();
                            }
                        }
                    }
                }
            }

            if (data.contains("mmio") && data.at("mmio").is_table())
            {
                const auto &mmioTable = toml::find(data, "mmio").as_table();
                for (const auto &[key, value] : mmioTable)
                {
                    uint32_t instAddr = std::stoul(key, nullptr, 0);
                    uint32_t mmioAddr = 0;
                    if (value.is_string())
                    {
                        mmioAddr = std::stoul(value.as_string(), nullptr, 0);
                    }
                    else if (value.is_integer())
                    {
                        mmioAddr = static_cast<uint32_t>(value.as_integer());
                    }
                    config.mmioByInstructionAddress[instAddr] = mmioAddr;
                }
            }

            if (data.contains("jump_tables") && data.at("jump_tables").is_table())
            {
                const auto &jumpTablesNode = data.at("jump_tables");
                if (jumpTablesNode.contains("table") && jumpTablesNode.at("table").is_array())
                {
                    const auto &tables = jumpTablesNode.at("table").as_array();
                    for (const auto &tableNode : tables)
                    {
                        if (!tableNode.is_table())
                        {
                            continue;
                        }

                        JumpTable table{};

                        if (tableNode.contains("address"))
                        {
                            const auto &addressValue = tableNode.at("address");
                            if (addressValue.is_string())
                            {
                                table.address = std::stoul(addressValue.as_string(), nullptr, 0);
                            }
                            else if (addressValue.is_integer())
                            {
                                table.address = static_cast<uint32_t>(addressValue.as_integer());
                            }
                        }

                        if (tableNode.contains("base_register"))
                        {
                            const auto &baseRegisterValue = tableNode.at("base_register");
                            if (baseRegisterValue.is_string())
                            {
                                table.baseRegister = std::stoul(baseRegisterValue.as_string(), nullptr, 0);
                            }
                            else if (baseRegisterValue.is_integer())
                            {
                                table.baseRegister = static_cast<uint32_t>(baseRegisterValue.as_integer());
                            }
                        }

                        if (table.address == 0u || !tableNode.contains("entries") || !tableNode.at("entries").is_array())
                        {
                            continue;
                        }

                        const auto &entries = tableNode.at("entries").as_array();
                        uint32_t fallbackIndex = 0u;
                        for (const auto &entryNode : entries)
                        {
                            if (!entryNode.is_table())
                            {
                                ++fallbackIndex;
                                continue;
                            }

                            JumpTableEntry entry{};
                            entry.index = fallbackIndex;

                            if (entryNode.contains("index"))
                            {
                                const auto &indexValue = entryNode.at("index");
                                if (indexValue.is_string())
                                {
                                    entry.index = std::stoul(indexValue.as_string(), nullptr, 0);
                                }
                                else if (indexValue.is_integer())
                                {
                                    entry.index = static_cast<uint32_t>(indexValue.as_integer());
                                }
                            }

                            bool hasTarget = false;
                            if (entryNode.contains("target"))
                            {
                                const auto &targetValue = entryNode.at("target");
                                if (targetValue.is_string())
                                {
                                    entry.target = std::stoul(targetValue.as_string(), nullptr, 0);
                                    hasTarget = true;
                                }
                                else if (targetValue.is_integer())
                                {
                                    entry.target = static_cast<uint32_t>(targetValue.as_integer());
                                    hasTarget = true;
                                }
                            }

                            if (hasTarget)
                            {
                                table.entries.push_back(entry);
                            }
                            ++fallbackIndex;
                        }

                        if (!table.entries.empty())
                        {
                            config.jumpTables.push_back(std::move(table));
                        }
                    }
                }
            }
        }
        catch (const std::exception &e)
        {
            if (m_reporter)
            {
                m_reporter->error("config", std::string("Error parsing configuration file: ") + e.what());
            }
            throw;
        }

        return config;
    }

    void ConfigManager::saveConfig(const RecompilerConfig &config) const
    {
        toml::value data;

        toml::table general;
        general["input"] = config.inputPath;
        general["input_format"] = config.inputFormat;
        if (config.inputFormat == "raw")
        {
            general["module_id"] = config.moduleId;
            general["raw_base"] = static_cast<int64_t>(config.rawBase);
            general["raw_code_offset"] = static_cast<int64_t>(config.rawCodeOffset);
        }
        general["ghidra_output"] = config.ghidraMapPath;
        general["output"] = config.outputPath;
        general["single_file_output"] = config.singleFileOutput;
        general["low_memory_mode"] = config.lowMemoryMode;
        general["output_worker_threads"] = static_cast<int64_t>(config.outputWorkerThreads);
        general["patch_syscalls"] = config.patchSyscalls;
        general["patch_cop0"] = config.patchCop0;
        general["patch_cache"] = config.patchCache;
        general["skip"] = config.skipFunctions;
        general["stubs"] = config.stubImplementations;
        general["entry_points"] = config.entryPointHints;
        data["general"] = general;
        if (!config.codeCopies.empty())
        {
            toml::array copies;
            for (const auto &copy : config.codeCopies)
            {
                toml::table item;
                item["name"] = copy.name;
                item["source"] = int64_t(copy.source); item["destination"] = int64_t(copy.destination); item["size"] = int64_t(copy.size);
                toml::array ranges;
                for (const auto &[offset,length] : copy.functions)
                    ranges.push_back(toml::array{int64_t(offset),int64_t(length)});
                item["functions"] = ranges;
                copies.push_back(item);
            }
            data["code_copies"] = copies;
        }

        if (!config.mmioByInstructionAddress.empty())
        {
            toml::table mmioTable;
            for (const auto &[instAddr, mmioAddr] : config.mmioByInstructionAddress)
            {
                std::ostringstream keyStream;
                keyStream << "0x" << std::hex << instAddr;
                std::ostringstream valStream;
                valStream << "0x" << std::hex << mmioAddr;
                mmioTable[keyStream.str()] = valStream.str();
            }
            data["mmio"] = mmioTable;
        }

        if (!config.jumpTables.empty())
        {
            toml::table jumpTables;
            toml::array tableArray;
            for (const auto &table : config.jumpTables)
            {
                toml::table tableNode;
                std::ostringstream addressStream;
                addressStream << "0x" << std::hex << table.address;
                tableNode["address"] = addressStream.str();
                tableNode["base_register"] = static_cast<int64_t>(table.baseRegister);

                toml::array entries;
                for (const auto &entry : table.entries)
                {
                    toml::table entryNode;
                    entryNode["index"] = static_cast<int64_t>(entry.index);
                    std::ostringstream targetStream;
                    targetStream << "0x" << std::hex << entry.target;
                    entryNode["target"] = targetStream.str();
                    entries.push_back(entryNode);
                }
                tableNode["entries"] = entries;
                tableArray.push_back(tableNode);
            }
            jumpTables["table"] = tableArray;
            data["jump_tables"] = jumpTables;
        }

        toml::table patches;
        toml::array instPatches;
        for (const auto &[addr, value] : config.patches)
        {
            std::ostringstream addrStream;
            addrStream << "0x" << std::hex << addr;

            toml::table p;
            p["address"] = addrStream.str();
            p["value"] = value;
            instPatches.push_back(p);
        }
        patches["instructions"] = instPatches;
        data["patches"] = patches;

        std::ofstream file(m_configPath);
        if (!file)
        {
            throw std::runtime_error("Failed to open file for writing: " + m_configPath);
        }

        file << data;
    }

} // namespace ps2recomp
