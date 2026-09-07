#include "framework.h"
#include "ShellIntegration.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <winrt/base.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>

using Microsoft::WRL::ComPtr;

namespace
{
ComPtr<IDataObject> CreateFileDataObject(const std::wstring& path)
{
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return nullptr;
    ComPtr<IDataObject> dataObject;
    if (FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&dataObject)))) return nullptr;
    return dataObject;
}

// `handler` is captured by value (ComPtr) into the returned std::function, so
// each Open-With entry keeps its own IAssocHandler alive independent of the
// IEnumAssocHandlers that produced it.
bool InvokeAssocHandler(ComPtr<IAssocHandler> handler, const std::wstring& path)
{
    ComPtr<IDataObject> dataObject = CreateFileDataObject(path);
    if (!dataObject) return false;
    return SUCCEEDED(handler->Invoke(dataObject.Get()));
}

bool ShowOpenWithDialog(const std::wstring& path)
{
    OPENASINFO info{};
    info.pcszFile = path.c_str();
    info.pcszClass = nullptr;
    info.oaifInFlags = OAIF_EXEC | OAIF_HIDE_REGISTRATION;
    return SUCCEEDED(SHOpenWithDialog(nullptr, &info));
}
}

std::vector<OpenWithEntry> EnumerateOpenWithHandlers(const std::wstring& path)
{
    std::vector<OpenWithEntry> entries;
    const std::size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos)
    {
        const std::wstring extension = path.substr(dot);
        ComPtr<IEnumAssocHandlers> enumHandlers;
        if (SUCCEEDED(SHAssocEnumHandlers(extension.c_str(), ASSOC_FILTER_RECOMMENDED, &enumHandlers)) && enumHandlers)
        {
            ComPtr<IAssocHandler> handler;
            ULONG fetched = 0;
            while (enumHandlers->Next(1, &handler, &fetched) == S_OK && fetched == 1)
            {
                PWSTR name = nullptr;
                if (SUCCEEDED(handler->GetUIName(&name)) && name)
                {
                    OpenWithEntry entry;
                    entry.displayName = name;
                    CoTaskMemFree(name);
                    entry.invoke = [handler](const std::wstring& filePath) { return InvokeAssocHandler(handler, filePath); };
                    entries.push_back(std::move(entry));
                }
                handler.Reset();
            }
        }
    }
    OpenWithEntry chooseAnother;
    chooseAnother.displayName = L"Choose another app…";
    chooseAnother.invoke = [](const std::wstring& filePath) { return ShowOpenWithDialog(filePath); };
    entries.push_back(std::move(chooseAnother));
    return entries;
}

bool ShowWindowsShare(HWND window, const std::wstring& path, std::wstring& error)
{
    using namespace winrt::Windows::ApplicationModel::DataTransfer;
    using namespace winrt::Windows::Storage;
    try
    {
        auto interop = winrt::get_activation_factory<DataTransferManager, IDataTransferManagerInterop>();
        DataTransferManager manager{ nullptr };
        winrt::check_hresult(interop->GetForWindow(window, winrt::guid_of<DataTransferManager>(), winrt::put_abi(manager)));

        // DataRequested fires when the flyout opens; StorageFile lookup is
        // async, so the handler takes a deferral and completes it once the
        // file (and its share payload) is ready.
        manager.DataRequested([path](DataTransferManager const&, DataRequestedEventArgs const& args) -> winrt::fire_and_forget
        {
            auto request = args.Request();
            auto deferral = request.GetDeferral();
            try
            {
                StorageFile file = co_await StorageFile::GetFileFromPathAsync(path);
                request.Data().Properties().Title(file.Name());
                auto items = winrt::single_threaded_vector<IStorageItem>();
                items.Append(file);
                request.Data().SetStorageItems(items.GetView());
            }
            catch (...)
            {
                request.FailWithDisplayText(L"This file could not be shared.");
            }
            deferral.Complete();
        });

        winrt::check_hresult(interop->ShowShareUIForWindow(window));
        return true;
    }
    catch (const winrt::hresult_error& ex)
    {
        error = ex.message().c_str();
        return false;
    }
}
