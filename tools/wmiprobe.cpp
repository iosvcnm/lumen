// 开发辅助：WMI 内置屏亮度读写测试
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wbemidl.h>
#include <stdio.h>

static IWbemServices* g_svc = nullptr;

static bool Connect() {
    IWbemLocator* loc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator, (void**)&loc))) return false;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &g_svc);
    SysFreeString(ns);
    loc->Release();
    if (FAILED(hr)) { wprintf(L"connect 0x%08lX\n", (unsigned long)hr); return false; }
    CoSetProxyBlanket(g_svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return true;
}

static IEnumWbemClassObject* Query(const wchar_t* wql) {
    IEnumWbemClassObject* en = nullptr;
    BSTR lang = SysAllocString(L"WQL");
    BSTR q = SysAllocString(wql);
    HRESULT hr = g_svc->ExecQuery(lang, q, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang); SysFreeString(q);
    return FAILED(hr) ? nullptr : en;
}

static int Read() {
    IEnumWbemClassObject* en = Query(L"SELECT CurrentBrightness FROM WmiMonitorBrightness");
    if (!en) return -1;
    int val = -1;
    IWbemClassObject* obj = nullptr;
    ULONG n = 0;
    if (en->Next(1500, 1, &obj, &n) == S_OK && n == 1) {
        VARIANT v; VariantInit(&v);
        if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr))) val = (int)v.bVal;
        VariantClear(&v);
        obj->Release();
    }
    en->Release();
    return val;
}

static bool Write(int pct) {
    IEnumWbemClassObject* en = Query(L"SELECT * FROM WmiMonitorBrightnessMethods");
    if (!en) { wprintf(L"query methods failed\n"); return false; }

    BSTR path = nullptr;
    IWbemClassObject* obj = nullptr;
    ULONG n = 0;
    if (en->Next(1500, 1, &obj, &n) == S_OK && n == 1) {
        VARIANT v; VariantInit(&v);
        if (SUCCEEDED(obj->Get(L"__PATH", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR)
            path = SysAllocString(v.bstrVal);
        VariantClear(&v);
        obj->Release();
    }
    en->Release();
    if (!path) { wprintf(L"no instance path\n"); return false; }
    wprintf(L"path=%ls\n", path);

    IWbemClassObject* cls = nullptr;
    BSTR cn = SysAllocString(L"WmiMonitorBrightnessMethods");
    HRESULT hr = g_svc->GetObject(cn, 0, nullptr, &cls, nullptr);
    SysFreeString(cn);
    wprintf(L"GetObject 0x%08lX cls=%p\n", (unsigned long)hr, (void*)cls);
    if (FAILED(hr) || !cls) { SysFreeString(path); return false; }

    IWbemClassObject* sig = nullptr;
    hr = cls->GetMethod(L"WmiSetBrightness", 0, &sig, nullptr);
    wprintf(L"GetMethod 0x%08lX sig=%p\n", (unsigned long)hr, (void*)sig);
    cls->Release();
    if (FAILED(hr) || !sig) { SysFreeString(path); return false; }

    sig->BeginEnumeration(WBEM_FLAG_LOCAL_ONLY);
    {
        BSTR pn = nullptr; VARIANT pv; CIMTYPE pt; LONG pf;
        VariantInit(&pv);
        while (sig->Next(0, &pn, &pv, &pt, &pf) == S_OK) {
            wprintf(L"param %ls type=%d\n", pn, (int)pt);
            SysFreeString(pn);
            VariantClear(&pv);
            VariantInit(&pv);
        }
    }
    sig->EndEnumeration();

    IWbemClassObject* in = nullptr;
    hr = sig->SpawnInstance(0, &in);
    sig->Release();
    if (FAILED(hr) || !in) { SysFreeString(path); return false; }

    VARIANT a; VariantInit(&a);
    a.vt = VT_UI1; a.bVal = (BYTE)pct;
    HRESULT hp2 = in->Put(L"Brightness", 0, &a, CIM_UINT8);
    VariantClear(&a);
    wprintf(L"put brightness=0x%08lX\n", (unsigned long)hp2);

    IWbemClassObject* outp = nullptr;
    hr = g_svc->ExecMethod(path, L"WmiSetBrightness", 0, nullptr, in, &outp, nullptr);
    wprintf(L"ExecMethod 0x%08lX outp=%p\n", (unsigned long)hr, (void*)outp);
    if (outp) {
        VARIANT rv; VariantInit(&rv);
        if (SUCCEEDED(outp->Get(L"ReturnValue", 0, &rv, nullptr, nullptr)))
            wprintf(L"ReturnValue=%ld\n", (long)rv.lVal);
        VariantClear(&rv);
        outp->Release();
    }
    in->Release();
    SysFreeString(path);
    return SUCCEEDED(hr);
}

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    if (!Connect()) return 1;
    wprintf(L"before = %d\n", Read());
    if (argc >= 3 && !_wcsicmp(argv[1], L"set")) {
        int v = _wtoi(argv[2]);
        bool ok = Write(v);
        wprintf(L"write %d -> %d\n", v, ok);
        Sleep(300);
        wprintf(L"after = %d\n", Read());
    }
    g_svc->Release();
    CoUninitialize();
    return 0;
}
