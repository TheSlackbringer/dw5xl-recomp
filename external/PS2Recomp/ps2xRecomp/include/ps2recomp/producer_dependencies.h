#pragma once
// Actual loaded code dependencies, not package extraction paths or unrelated launcher source.
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <set>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <link.h>
#endif

namespace ps2recomp {
inline std::vector<std::filesystem::path> producerDependencies()
{
    std::vector<std::filesystem::path> result;
#ifdef _WIN32
    // Walk loader-resolved imports, including forwarded/API-set destinations. Enumerating every
    // loaded module also captures transient compatibility/UI/security modules unrelated to codegen.
    // IAT owners identify the actual provider without guessing API-set filenames or SDK locations.
    const HMODULE main=GetModuleHandleW(nullptr);
    if(!main) throw std::runtime_error("Cannot inspect producer image");
    std::vector<HMODULE> pending{main};std::set<HMODULE> visited;
    for(size_t next=0;next<pending.size();++next)
    {
        const auto module=pending[next];if(!visited.insert(module).second) continue;
        if(module!=main)
        {
            std::vector<wchar_t> name(32768);
            const DWORD count=GetModuleFileNameW(module,name.data(),DWORD(name.size()));
            if(!count || count>=name.size()) throw std::runtime_error("Unknown producer module path");
            result.emplace_back(std::wstring(name.data(),count));
        }
        const auto base=reinterpret_cast<const unsigned char *>(module);
        const auto dos=reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>1024*1024)
            throw std::runtime_error("Unknown producer image header");
        const auto pe=reinterpret_cast<const IMAGE_NT_HEADERS *>(base+dos->e_lfanew);
        if(pe->Signature!=IMAGE_NT_SIGNATURE || pe->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR_MAGIC ||
           pe->OptionalHeader.NumberOfRvaAndSizes<=IMAGE_DIRECTORY_ENTRY_IMPORT)
            throw std::runtime_error("Unknown producer import format");
        const size_t size=pe->OptionalHeader.SizeOfImage;
        const auto bounded=[&](size_t rva,size_t bytes) {return rva<=size && bytes<=size-rva;};
        // A producer's own optional/dynamic code dependency needs an explicit identity protocol;
        // do not silently certify an unresolved delay import as a complete producer description.
        if(module==main && pe->OptionalHeader.NumberOfRvaAndSizes>IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT &&
           pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress)
            throw std::runtime_error("Delayed producer imports need an explicit identity");
        const auto imports=pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if(!imports.VirtualAddress && !imports.Size) continue;
        if(!imports.VirtualAddress || !bounded(imports.VirtualAddress,imports.Size))
            throw std::runtime_error("Incomplete producer import directory");
        bool ended=false;
        for(size_t offset=0;offset+sizeof(IMAGE_IMPORT_DESCRIPTOR)<=imports.Size;offset+=sizeof(IMAGE_IMPORT_DESCRIPTOR))
        {
            const auto entry=reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(base+imports.VirtualAddress+offset);
            if(!entry->Name && !entry->FirstThunk) {ended=true;break;}
            if(!entry->Name || !entry->FirstThunk || !bounded(entry->Name,1))
                throw std::runtime_error("Incomplete producer import descriptor");
            size_t length=0;
            while(bounded(size_t(entry->Name)+length,1) && base[entry->Name+length]) ++length;
            if(!bounded(size_t(entry->Name)+length,1)) throw std::runtime_error("Unknown producer import name");
            // Keep a loaded forwarding library as well as the function's resolved implementation.
            if(const auto declared=GetModuleHandleA(reinterpret_cast<const char *>(base+entry->Name)))
                if(!visited.count(declared)) pending.push_back(declared);
            size_t thunk=entry->FirstThunk;
            for(;;thunk+=sizeof(IMAGE_THUNK_DATA))
            {
                if(!bounded(thunk,sizeof(IMAGE_THUNK_DATA))) throw std::runtime_error("Incomplete producer import table");
                const auto address=reinterpret_cast<const IMAGE_THUNK_DATA *>(base+thunk)->u1.Function;
                if(!address) break;
                HMODULE owner=nullptr;
                if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCWSTR>(address),&owner) || !owner)
                    throw std::runtime_error("Unknown resolved producer dependency");
                if(!visited.count(owner)) pending.push_back(owner);
            }
        }
        if(!ended) throw std::runtime_error("Unterminated producer import directory");
    }
#else
    struct State {std::vector<std::filesystem::path> *paths;bool failed=false;} state{&result};
    dl_iterate_phdr([](dl_phdr_info *info,size_t,void *pointer) {
        auto &s=*static_cast<State *>(pointer);
        try
        {
            if(info->dlpi_name && *info->dlpi_name)
            {
                const std::string name(info->dlpi_name);
                if(name.find("linux-vdso")!=0) s.paths->emplace_back(name);
            }
            return 0;
        }
        catch(...) {s.failed=true;return 1;}
    },&state);
    if(state.failed) throw std::runtime_error("Incomplete producer dependency list");
#endif
    return result;
}
} // namespace ps2recomp
