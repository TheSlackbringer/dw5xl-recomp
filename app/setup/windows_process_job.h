#pragma once

// Private Windows job cleanup. A process wait, rather than a job notification or
// elapsed delay, establishes that each owned process has finished terminating.
#include <windows.h>
#include <cstddef>
#include <exception>
#include <string>
#include <vector>

namespace dw5xl::setup::windows_process {
inline bool stopAndWait(HANDLE job, std::string &error)
{
    struct Processes
    {
        std::vector<HANDLE> handles;
        ~Processes() { for (HANDLE process : handles) CloseHandle(process); }
    } processes;
    const auto fail = [&](const char *operation, DWORD code) {
        if (error.empty()) error = std::string(operation) + " (error " + std::to_string(code) + ")";
    };
    try
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        if (!QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr))
            fail("cannot query owned job limits", GetLastError());
        else
        {
            // Existing members remain owned. Admission of another process would exceed one
            // while any member is alive, so the following snapshot cannot gain new children.
            // This limit is applied only when this private command is being cleaned up.
            limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
            limits.BasicLimitInformation.ActiveProcessLimit = 1;
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
                fail("cannot close owned job process admission", GetLastError());
        }
        size_t capacity = 1;
        std::vector<ULONG_PTR> storage;
        for (;;)
        {
            constexpr size_t header = offsetof(JOBOBJECT_BASIC_PROCESS_ID_LIST, ProcessIdList);
            if (capacity > (MAXDWORD - header) / sizeof(ULONG_PTR))
            { fail("owned job process list is too large", ERROR_ARITHMETIC_OVERFLOW); break; }
            const size_t bytes = header + capacity * sizeof(ULONG_PTR);
            storage.resize((bytes + sizeof(ULONG_PTR) - 1) / sizeof(ULONG_PTR));
            auto *list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST *>(storage.data());
            const BOOL queried = QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, DWORD(bytes), nullptr);
            if (!queried && GetLastError() != ERROR_MORE_DATA)
            { fail("cannot enumerate owned job processes", GetLastError()); break; }
            if (!queried || list->NumberOfProcessIdsInList < list->NumberOfAssignedProcesses)
            {
                capacity = list->NumberOfAssignedProcesses > capacity ? list->NumberOfAssignedProcesses : capacity * 2;
                continue;
            }
            for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i)
            {
                if (list->ProcessIdList[i] > MAXDWORD)
                { fail("invalid owned process identifier", ERROR_INVALID_DATA); continue; }
                HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                             DWORD(list->ProcessIdList[i]));
                if (!process)
                {
                    // An exited process whose object disappeared is already complete.
                    const DWORD code = GetLastError();
                    if (code != ERROR_INVALID_PARAMETER) fail("cannot retain owned process", code);
                    continue;
                }
                BOOL owned = FALSE;
                if (!IsProcessInJob(process, job, &owned))
                { fail("cannot verify owned process", GetLastError()); CloseHandle(process); continue; }
                if (!owned)
                {
                    // Never wait for an unrelated process if a snapshot PID has been reused.
                    if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0)
                        fail("owned process identity changed during cleanup", ERROR_INVALID_DATA);
                    CloseHandle(process); continue;
                }
                try { processes.handles.push_back(process); }
                catch (...) { CloseHandle(process); throw; }
            }
            break;
        }
    }
    catch (const std::exception &)
    { fail("cannot retain owned job process handles", ERROR_NOT_ENOUGH_MEMORY); }

    // Termination is asynchronous. Retained handles refer to these exact objects,
    // include nested jobs, and cannot turn into handles to reused process IDs.
    if (!TerminateJobObject(job, 1))
    { fail("cannot terminate owned job", GetLastError()); return false; }
    for (HANDLE process : processes.handles)
        if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0)
            fail("cannot wait for owned process termination", GetLastError());
    return error.empty();
}
} // namespace dw5xl::setup::windows_process
