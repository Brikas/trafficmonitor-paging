// TrafficMonitor plugin: hard page reads per second (with taskbar graph) and page file usage.
// Built against the V1.86 plugin interface (API 7), vendored as PluginInterface.h.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include "PluginInterface.h"

#pragma comment(lib, "pdh.lib")

namespace
{
    // Graph is log-scaled: 0 reads/s is empty, this many reads/s or more is full height.
    constexpr double kGraphFullReads = 1000.0;
    constexpr double kPageBytes = 4096.0;
    constexpr double kMB = 1024.0 * 1024.0;
    constexpr double kGB = 1024.0 * 1024.0 * 1024.0;

    // Layout of SYSTEM_PAGEFILE_INFORMATION (class 18) from NtQuerySystemInformation.
    struct PageFileInfo
    {
        ULONG NextEntryOffset;
        ULONG TotalSize;    // pages
        ULONG TotalInUse;   // pages
        ULONG PeakUsage;    // pages
        USHORT NameLength;
        USHORT NameMaxLength;
        PWSTR NameBuffer;
    };
    using NtQuerySystemInformationFn = LONG(WINAPI*)(ULONG, PVOID, ULONG, PULONG);

    struct Stats
    {
        bool valid = false;
        double reads = 0;       // Page Reads/sec: disk read operations for hard faults
        double pages_in = 0;    // Pages Input/sec
        double writes = 0;      // Page Writes/sec
        double pages_out = 0;   // Pages Output/sec
        double pf_used_gb = 0;
        double pf_size_gb = 0;
    };

    Stats g_stats;
}

class PageReadsItem : public IPluginItem
{
public:
    const wchar_t* GetItemName() const override { return L"Hard page reads/s"; }
    const wchar_t* GetItemId() const override { return L"paging_reads"; }
    const wchar_t* GetItemLableText() const override { return L"PGIN:"; }
    const wchar_t* GetItemValueText() const override { return m_value; }
    const wchar_t* GetItemValueSampleText() const override { return L"9999/s"; }
    int IsDrawResourceUsageGraph() const override { return 1; }
    float GetResourceUsageGraphValue() const override
    {
        double v = std::log10(1.0 + g_stats.reads) / std::log10(1.0 + kGraphFullReads);
        return static_cast<float>(v > 1.0 ? 1.0 : v);
    }

    void Update()
    {
        if (g_stats.valid)
            swprintf_s(m_value, L"%.0f/s", g_stats.reads);
        else
            wcscpy_s(m_value, L"--");
    }

private:
    wchar_t m_value[32] = L"--";
};

class PageFileItem : public IPluginItem
{
public:
    const wchar_t* GetItemName() const override { return L"Page file used"; }
    const wchar_t* GetItemId() const override { return L"paging_file_used"; }
    const wchar_t* GetItemLableText() const override { return L"SWAP:"; }
    const wchar_t* GetItemValueText() const override { return m_value; }
    const wchar_t* GetItemValueSampleText() const override { return L"99.9 GB"; }

    void Update() { swprintf_s(m_value, L"%.1f GB", g_stats.pf_used_gb); }

private:
    wchar_t m_value[32] = L"--";
};

class PagingPlugin : public ITMPlugin
{
public:
    PagingPlugin()
    {
        m_ntqsi = reinterpret_cast<NtQuerySystemInformationFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
        // English counter names work on any Windows display language.
        if (PdhOpenQueryW(nullptr, 0, &m_query) == ERROR_SUCCESS)
        {
            PdhAddEnglishCounterW(m_query, L"\\Memory\\Page Reads/sec", 0, &m_reads);
            PdhAddEnglishCounterW(m_query, L"\\Memory\\Pages Input/sec", 0, &m_pages_in);
            PdhAddEnglishCounterW(m_query, L"\\Memory\\Page Writes/sec", 0, &m_writes);
            PdhAddEnglishCounterW(m_query, L"\\Memory\\Pages Output/sec", 0, &m_pages_out);
            PdhCollectQueryData(m_query);   // rate counters need a first sample to diff against
        }
    }

    IPluginItem* GetItem(int index) override
    {
        switch (index)
        {
        case 0: return &m_reads_item;
        case 1: return &m_file_item;
        default: return nullptr;
        }
    }

    // Called by TrafficMonitor once per update interval.
    void DataRequired() override
    {
        g_stats.valid = false;
        if (m_query != nullptr && PdhCollectQueryData(m_query) == ERROR_SUCCESS)
        {
            g_stats.valid = Read(m_reads, g_stats.reads)
                & Read(m_pages_in, g_stats.pages_in)
                & Read(m_writes, g_stats.writes)
                & Read(m_pages_out, g_stats.pages_out);
        }
        ReadPageFiles();
        m_reads_item.Update();
        m_file_item.Update();
        swprintf_s(m_tooltip,
            L"Hard page reads: %.0f/s (%.1f MB/s)\r\nPage writes: %.0f/s (%.1f MB/s)\r\nPage file: %.1f / %.1f GB",
            g_stats.reads, g_stats.pages_in * kPageBytes / kMB,
            g_stats.writes, g_stats.pages_out * kPageBytes / kMB,
            g_stats.pf_used_gb, g_stats.pf_size_gb);
    }

    const wchar_t* GetInfo(PluginInfoIndex index) override
    {
        switch (index)
        {
        case TMI_NAME: return L"Paging Monitor";
        case TMI_DESCRIPTION: return L"Hard page reads per second and page file usage.";
        case TMI_AUTHOR: return L"Brikas";
        case TMI_VERSION: return L"1.0";
        default: return L"";
        }
    }

    const wchar_t* GetTooltipInfo() override { return m_tooltip; }

private:
    static bool Read(PDH_HCOUNTER counter, double& out)
    {
        PDH_FMT_COUNTERVALUE value{};
        if (counter == nullptr || PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) != ERROR_SUCCESS)
            return false;
        out = value.doubleValue;
        return true;
    }

    // Sums used and total size over all page files.
    void ReadPageFiles()
    {
        if (m_ntqsi == nullptr)
            return;
        alignas(8) BYTE buffer[4096];
        ULONG len = 0;
        if (m_ntqsi(18, buffer, sizeof(buffer), &len) < 0 || len == 0)
            return;
        double used = 0, size = 0;
        for (BYTE* p = buffer;;)
        {
            auto* info = reinterpret_cast<PageFileInfo*>(p);
            used += info->TotalInUse;
            size += info->TotalSize;
            if (info->NextEntryOffset == 0)
                break;
            p += info->NextEntryOffset;
        }
        g_stats.pf_used_gb = used * kPageBytes / kGB;
        g_stats.pf_size_gb = size * kPageBytes / kGB;
    }

    PDH_HQUERY m_query = nullptr;
    PDH_HCOUNTER m_reads = nullptr, m_pages_in = nullptr, m_writes = nullptr, m_pages_out = nullptr;
    NtQuerySystemInformationFn m_ntqsi = nullptr;
    PageReadsItem m_reads_item;
    PageFileItem m_file_item;
    wchar_t m_tooltip[256] = L"";
};

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    static PagingPlugin instance;
    return &instance;
}
