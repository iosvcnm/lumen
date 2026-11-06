// 开发辅助：列出 WmiMonitorID 的品牌与型号
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wbemidl.h>
#include <stdio.h>
#include <string>

static IWbemServices* g_svc = nullptr;

static bool Connect() {
    IWbemLocator* loc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator, (void**)&loc))) return false;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &g_svc);
    SysFreeString(ns);
    loc->Release();
    if (FAILED(hr)) return false;
    CoSetProxyBlanket(g_svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return true;
}

static std::wstring U16(IWbemClassObject* o, const wchar_t* prop) {
    VARIANT v;
    VariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(o->Get(prop, 0, &v, nullptr, nullptr)) && (v.vt & VT_ARRAY) && v.parray) {
        LONG lb = 0, ub = -1;
        SafeArrayGetLBound(v.parray, 1, &lb);
        SafeArrayGetUBound(v.parray, 1, &ub);
        USHORT* p = nullptr;
        if (SUCCEEDED(SafeArrayAccessData(v.parray, (void**)&p))) {
            for (LONG i = lb; i <= ub; ++i) if (p[i]) out.push_back((wchar_t)p[i]);
            SafeArrayUnaccessData(v.parray);
        }
    }
    VariantClear(&v);
    return out;
}

int wmain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    if (!Connect()) { wprintf(L"connect failed\n"); return 1; }

    IEnumWbemClassObject* en = nullptr;
    BSTR lang = SysAllocString(L"WQL");
    BSTR q = SysAllocString(L"SELECT InstanceName, UserFriendlyName, ManufacturerName FROM WmiMonitorID");
    HRESULT hr = g_svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang); SysFreeString(q);
    if (FAILED(hr)) { wprintf(L"query 0x%08lX\n", (unsigned long)hr); return 1; }

    IWbemClassObject* o = nullptr;
    ULONG n = 0;
    int i = 0;
    while (en->Next(2000, 1, &o, &n) == S_OK && n == 1) {
        VARIANT iv; VariantInit(&iv);
        std::wstring inst;
        if (SUCCEEDED(o->Get(L"InstanceName", 0, &iv, nullptr, nullptr)) && iv.vt == VT_BSTR)
            inst = iv.bstrVal;
        VariantClear(&iv);
        wprintf(L"[%d] instance=%ls\n", i++, inst.c_str());
        wprintf(L"     friendly=%ls\n", U16(o, L"UserFriendlyName").c_str());
        wprintf(L"     manuf=%ls\n", U16(o, L"ManufacturerName").c_str());
        o->Release();
    }
    en->Release();
    g_svc->Release();
    CoUninitialize();
    return 0;
}
