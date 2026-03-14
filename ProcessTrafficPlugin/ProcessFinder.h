#pragma once

#include <string>
#include <vector>

#include <Windows.h>

class CProcessFinder
{
public:
    struct ProcessEntry
    {
        DWORD pid;
        std::wstring exeName;
        std::wstring exePath;
    };

    static std::vector<DWORD> FindProcessIdsByExeName(const std::wstring& exe_name);
    static std::vector<ProcessEntry> EnumerateProcesses();
    static std::wstring FindFirstProcessPathByExeName(const std::wstring& exe_name);
};
