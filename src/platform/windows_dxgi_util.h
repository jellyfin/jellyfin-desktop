#pragma once
#ifdef _WIN32

#include <windows.h>
#include <dxgi1_6.h>

// Find a DXGI adapter matching the given LUID.
// Returns an AddRef'd IDXGIAdapter1* on success, nullptr if no match.
// Caller must Release() the returned adapter.
inline IDXGIAdapter1* findDxgiAdapterByLuid(const LUID& luid) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)))
        return nullptr;

    IDXGIAdapter1* result = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (memcmp(&desc.AdapterLuid, &luid, sizeof(LUID)) == 0) {
            result = adapter;
            break;
        }
        adapter->Release();
    }
    factory->Release();
    return result;
}

#endif // _WIN32
