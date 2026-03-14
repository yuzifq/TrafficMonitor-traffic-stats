#pragma once

#include <Windows.h>

namespace TMPluginDetail
{
template<typename... TArgs>
inline void IgnoreUnused(const TArgs&...)
{
}
}

class IPluginItem
{
public:
    virtual const wchar_t* GetItemName() const = 0;
    virtual const wchar_t* GetItemId() const = 0;
    virtual const wchar_t* GetItemLableText() const = 0;
    virtual const wchar_t* GetItemValueText() const = 0;
    virtual const wchar_t* GetItemValueSampleText() const = 0;
    virtual bool IsCustomDraw() const { return false; }
    virtual int GetItemWidth() const { return 0; }
    virtual void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode)
    {
        TMPluginDetail::IgnoreUnused(hDC, x, y, w, h, dark_mode);
    }
    virtual int GetItemWidthEx(void* hDC) const
    {
        TMPluginDetail::IgnoreUnused(hDC);
        return GetItemWidth();
    }

    enum MouseEventType
    {
        MT_LCLICKED,
        MT_RCLICKED,
        MT_DBCLICKED,
        MT_WHEEL_UP,
        MT_WHEEL_DOWN,
    };

    enum MouseEventFlag
    {
        MF_TASKBAR_WND = 0x1
    };

    virtual int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag)
    {
        TMPluginDetail::IgnoreUnused(type, x, y, hWnd, flag);
        return 0;
    }

    enum KeyboardEventFlag
    {
        KF_TASKBAR_WND = 0x1
    };

    virtual int OnKeboardEvent(int key, bool ctrl, bool shift, bool alt, void* hWnd, int flag)
    {
        TMPluginDetail::IgnoreUnused(key, ctrl, shift, alt, hWnd, flag);
        return 0;
    }

    enum ItemInfoType
    {
        IIT_NONE = 0
    };

    virtual void* OnItemInfo(ItemInfoType type, void* para1, void* para2)
    {
        TMPluginDetail::IgnoreUnused(type, para1, para2);
        return nullptr;
    }
    virtual int IsDrawResourceUsageGraph() const { return -1; }
    virtual float GetResourceUsageGraphValue() const { return 0.0f; }
};

class ITrafficMonitor;

class ITMPlugin
{
public:
    enum PluginInfoIndex
    {
        TMI_NAME,
        TMI_DESCRIPTION,
        TMI_AUTHOR,
        TMI_COPYRIGHT,
        TMI_VERSION,
        TMI_URL,
        TMI_API_VERSION,
        TMI_MAX
    };

    enum OptionReturn
    {
        OR_OPTION_CHANGED,
        OR_OPTION_UNCHANGED,
        OR_OPTION_CANCELED
    };

    struct MonitorInfo
    {
        unsigned long long up_speed{};
        unsigned long long down_speed{};
        int cpu_usage{};
        int memory_usage{};
        int gpu_usage{};
        int hdd_usage{};
        int cpu_temperature{};
        int gpu_temperature{};
        int hdd_temperature{};
        int main_board_temperature{};
        int cpu_freq{};
    };

    enum ExtendedInfoIndex
    {
        EI_CONFIG_DIR,
        EI_LABEL_TEXT_COLOR,
        EI_VALUE_TEXT_COLOR,
        EI_DRAW_TASKBAR_WND,
        EI_NAIN_WND_NET_SPEED_SHORT_MODE,
        EI_MAIN_WND_SPERATE_WITH_SPACE,
        EI_MAIN_WND_UNIT_BYTE,
        EI_MAIN_WND_UNIT_SELECT,
        EI_MAIN_WND_NOT_SHOW_UNIT,
        EI_MAIN_WND_NOT_SHOW_PERCENT,
        EI_TASKBAR_WND_NET_SPEED_SHORT_MODE,
        EI_TASKBAR_WND_SPERATE_WITH_SPACE,
        EI_TASKBAR_WND_VALUE_RIGHT_ALIGN,
        EI_TASKBAR_WND_NET_SPEED_WIDTH,
        EI_TASKBAR_WND_UNIT_BYTE,
        EI_TASKBAR_WND_UNIT_SELECT,
        EI_TASKBAR_WND_NOT_SHOW_UNIT,
        EI_TASKBAR_WND_NOT_SHOW_PERCENT,
    };

    virtual int GetAPIVersion() const = 0;
    virtual IPluginItem* GetItem(int index) = 0;
    virtual void DataRequired() = 0;
    virtual OptionReturn ShowOptionsDialog(void* hParent)
    {
        TMPluginDetail::IgnoreUnused(hParent);
        return OR_OPTION_CANCELED;
    }
    virtual const wchar_t* GetInfo(PluginInfoIndex index) = 0;
    virtual void OnMonitorInfo(const MonitorInfo& monitor_info)
    {
        TMPluginDetail::IgnoreUnused(monitor_info);
    }
    virtual const wchar_t* GetTooltipInfo() { return L""; }
    virtual void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)
    {
        TMPluginDetail::IgnoreUnused(index, data);
    }
    virtual HICON GetPluginIcon() { return nullptr; }
    virtual int GetCommandCount() { return 0; }
    virtual const wchar_t* GetCommandName(int command_index)
    {
        TMPluginDetail::IgnoreUnused(command_index);
        return nullptr;
    }
    virtual HICON GetCommandIcon(int command_index)
    {
        TMPluginDetail::IgnoreUnused(command_index);
        return nullptr;
    }
    virtual void OnPluginCommand(int command_index, void* hWnd, void* para)
    {
        TMPluginDetail::IgnoreUnused(command_index, hWnd, para);
    }
    virtual bool IsCommandChecked(int command_index)
    {
        TMPluginDetail::IgnoreUnused(command_index);
        return false;
    }
    virtual void OnInitialize(ITrafficMonitor* pApp)
    {
        TMPluginDetail::IgnoreUnused(pApp);
    }
};

class ITrafficMonitor
{
public:
    enum MonitorItem
    {
        MI_UP,
        MI_DOWN,
        MI_CPU,
        MI_MEMORY,
        MI_GPU_USAGE,
        MI_CPU_TEMP,
        MI_GPU_TEMP,
        MI_HDD_TEMP,
        MI_MAIN_BOARD_TEMP,
        MI_HDD_USAGE,
        MI_CPU_FREQ,
        MI_TODAY_UP_TRAFFIC,
        MI_TODAY_DOWN_TRAFFIC
    };

    enum DPIType
    {
        DPI_MAIN_WND,
        DPI_TASKBAR
    };

    virtual int GetAPIVersion() = 0;
    virtual const wchar_t* GetVersion() = 0;
    virtual double GetMonitorValue(MonitorItem item) = 0;
    virtual const wchar_t* GetMonitorValueString(MonitorItem item, int is_main_window) = 0;
    virtual void ShowNotifyMessage(const wchar_t* strMsg) = 0;
    virtual WORD GetLanguageId() const = 0;
    virtual const wchar_t* GetPluginConfigDir() const = 0;
    virtual int GetDPI(DPIType type) const = 0;
    virtual COLORREF GetThemeColor() const = 0;
    virtual const wchar_t* GetStringRes(const wchar_t* key, const wchar_t* section) = 0;
};

extern "C"
{
__declspec(dllexport) ITMPlugin* TMPluginGetInstance();
}
