#include "ProcNetItem.h"

#include <utility>

CProcNetItem::CProcNetItem(std::wstring item_name, std::wstring item_id, std::wstring label_text)
    : m_itemName(std::move(item_name)),
      m_itemId(std::move(item_id)),
      m_labelText(std::move(label_text)),
      m_valueText(L"N/A")
{
}

void CProcNetItem::SetName(std::wstring item_name)
{
    m_itemName = std::move(item_name);
}

void CProcNetItem::SetLabel(std::wstring label_text)
{
    m_labelText = std::move(label_text);
}

void CProcNetItem::SetValue(std::wstring value_text)
{
    m_valueText = std::move(value_text);
}

const wchar_t* CProcNetItem::GetItemName() const
{
    return m_itemName.c_str();
}

const wchar_t* CProcNetItem::GetItemId() const
{
    return m_itemId.c_str();
}

const wchar_t* CProcNetItem::GetItemLableText() const
{
    return m_labelText.c_str();
}

const wchar_t* CProcNetItem::GetItemValueText() const
{
    return m_valueText.c_str();
}

const wchar_t* CProcNetItem::GetItemValueSampleText() const
{
    return L"9999.9 KB/s";
}
