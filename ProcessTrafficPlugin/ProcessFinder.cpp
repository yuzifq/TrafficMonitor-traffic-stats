#include "ProcessFinder.h"

#include <TlHelp32.h>

#include <array>
#include <cwctype>

namespace
{
bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (size_t i = 0; i < left.size(); ++i)
    {
        if (std::towlower(left[i]) != std::towlower(right[i]))
        {
            return false;
        }
    }

    return true;
}

std::wstring QueryProcessImagePath(DWORD pid)
{
    std::array<wchar_t, 32768> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
    {
        return {};
    }

    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size) != FALSE)
    {
        result.assign(buffer.data(), size);
    }

    CloseHandle(process);
    return result;
}
}

std::vector<CProcessFinder::ProcessEntry> CProcessFinder::EnumerateProcesses()
{
    std::vector<ProcessEntry> processes;

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return processes;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            processes.push_back(ProcessEntry{ entry.th32ProcessID, entry.szExeFile, QueryProcessImagePath(entry.th32ProcessID) });
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return processes;
}

std::vector<DWORD> CProcessFinder::FindProcessIdsByExeName(const std::wstring& exe_name)
{
    std::vector<DWORD> process_ids;
    const auto processes = EnumerateProcesses();
    for (const auto& process : processes)
    {
        if (EqualsIgnoreCase(process.exeName, exe_name))
        {
            process_ids.push_back(process.pid);
        }
    }

    return process_ids;
}

std::wstring CProcessFinder::FindFirstProcessPathByExeName(const std::wstring& exe_name)
{
    const auto processes = EnumerateProcesses();
    for (const auto& process : processes)
    {
        if (EqualsIgnoreCase(process.exeName, exe_name) && !process.exePath.empty())
        {
            return process.exePath;
        }
    }

    return {};
}
