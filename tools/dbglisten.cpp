// 开发辅助：OutputDebugString 监听（DBWIN 协议），抓 lumen 的滚动诊断
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int wmain(int argc, wchar_t** argv) {
    DWORD ms = (argc > 1) ? _wtoi(argv[1]) : 15000;
    HANDLE bufReady = CreateEventW(nullptr, FALSE, TRUE, L"Local\\DBWIN_BUFFER_READY");
    HANDLE dataReady = CreateEventW(nullptr, FALSE, FALSE, L"Local\\DBWIN_DATA_READY");
    HANDLE map = CreateFileMappingW((HANDLE)-1, nullptr, PAGE_READWRITE, 0, 4096, L"Local\\DBWIN_BUFFER");
    if (!bufReady || !dataReady || !map) { wprintf(L"dbwin init fail\n"); return 1; }
    char* buf = (char*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 4096);
    DWORD pid = GetCurrentProcessId();
    SetEvent(bufReady);
    DWORD start = GetTickCount();
    while (GetTickCount() - start < (DWORD)ms) {
        DWORD w = WaitForSingleObject(dataReady, 200);
        if (w != WAIT_OBJECT_0) continue;
        DWORD srcPid = *(DWORD*)buf;
        char* text = buf + 4;
        if (srcPid != pid) { SetEvent(bufReady); continue; }
        wchar_t wbuf[512];
        MultiByteToWideChar(CP_ACP, 0, text, -1, wbuf, 512);
        wprintf(L"%ls\n", wbuf);
        fflush(stdout);
        SetEvent(bufReady);
    }
    return 0;
}
