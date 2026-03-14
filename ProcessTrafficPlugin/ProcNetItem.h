#pragma once

#include <string>

#include "PluginInterface.h"

class CProcNetItem final : public IPluginItem
{
public:
    CProcNetItem(std::wstring item_name, std::wstring item_id, std::wstring label_text);

    void SetName(std::wstring item_name);
    void SetLabel(std::wstring label_text);
    void SetValue(std::wstring value_text);

    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;

private:
    std::wstring m_itemName;
    std::wstring m_itemId;
    std::wstring m_labelText;
    std::wstring m_valueText;
};
