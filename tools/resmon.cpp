// 开发辅助：按 PID 采样进程资源占用（CPU / 内存 / 句柄 / 线程 / GDI+USER 对象）
// 用法：resmon.exe <pid> <间隔毫秒> <采样次数> [csv文件]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

static ULONGLONG FileTimeToU64(const FILETIME& ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: resmon <pid> [intervalMs] [count] [csv]\n"); return 2; }
    DWORD pid = (DWORD)_wtoi(argv[1]);
    int interval = (argc > 2) ? _wtoi(argv[2]) : 500;
    int count = (argc > 3) ? _wtoi(argv[3]) : 40;
    if (interval < 50) interval = 50;

    HANDLE ph = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!ph) ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!ph) { wprintf(L"open fail %lu\n", GetLastError()); return 1; }

    FILE* csv = nullptr;
    if (argc > 4) _wfopen_s(&csv, argv[4], L"w");
    if (csv) fprintf(csv, "elapsed_ms,cpu_pct,cpu_total_ms,ws_kb,private_kb,handles,threads,gdi,user\n");

    wprintf(L"t(ms)  CPU%%   CPUms   WS(KB)  Priv(KB)  Hnd  Thr  GDI  USER\n");
    ULONGLONG t0 = GetTickCount64();
    ULONGLONG prevCpu = 0, prevWall = 0, firstCpu = 0;
    double sumCpu = 0; int nCpu = 0;
    double maxCpu = 0; SIZE_T maxWs = 0, maxPriv = 0;
    DWORD maxHnd = 0, maxThr = 0, maxGdi = 0, maxUser = 0;
    SIZE_T firstWs = 0, firstPriv = 0; DWORD firstHnd = 0, firstThreads = 0, firstGdi = 0, firstUser = 0;

    for (int i = 0; i < count; ++i) {
        FILETIME ct, et, kt, ut;
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        DWORD hnd = 0, threads = 0;
        if (!GetProcessTimes(ph, &ct, &et, &kt, &ut)) break;
        GetProcessMemoryInfo(ph, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));
        GetProcessHandleCount(ph, &hnd);
        threads = pmc.PageFaultCount ? 0 : 0;   // 占位，下面用 Toolhelp 取
        DWORD gdi = GetGuiResources(ph, GR_GDIOBJECTS);
        DWORD user = GetGuiResources(ph, GR_USEROBJECTS);

        // 线程数：用 NtQueryInformationProcess 太重，这里用 Toolhelp 快照
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te{}; te.dwSize = sizeof(te);
                DWORD cnt = 0;
                if (Thread32First(snap, &te)) {
                    do { if (te.th32OwnerProcessID == pid) ++cnt; } while (Thread32Next(snap, &te));
                }
                CloseHandle(snap);
                threads = cnt;
            }
        }

        ULONGLONG cpu = FileTimeToU64(kt) + FileTimeToU64(ut);
        if (i == 0) firstCpu = cpu;
        ULONGLONG wall = GetTickCount64() - t0;
        double pct = 0;
        if (prevWall && wall > prevWall) {
            pct = (double)(cpu - prevCpu) / 10000.0 / (double)(wall - prevWall) * 100.0;
            sumCpu += pct; ++nCpu;
            if (pct > maxCpu) maxCpu = pct;
        }
        prevCpu = cpu; prevWall = wall;

        SIZE_T ws = pmc.WorkingSetSize / 1024, priv = pmc.PrivateUsage / 1024;
        if (i == 0) { firstWs = ws; firstPriv = priv; firstHnd = hnd; firstThreads = threads; firstGdi = gdi; firstUser = user; }
        if (ws > maxWs) maxWs = ws;
        if (priv > maxPriv) maxPriv = priv;
        if (hnd > maxHnd) maxHnd = hnd;
        if (threads > maxThr) maxThr = threads;
        if (gdi > maxGdi) maxGdi = gdi;
        if (user > maxUser) maxUser = user;

        wprintf(L"%5llu  %5.1f  %6llu  %6llu  %8llu  %3lu  %3lu  %3lu  %4lu\n",
                wall, pct, cpu / 10000, (unsigned long long)ws, (unsigned long long)priv,
                hnd, threads, gdi, user);
        if (csv) fprintf(csv, "%llu,%.2f,%llu,%llu,%llu,%lu,%lu,%lu,%lu\n",
                         wall, pct, cpu / 10000, (unsigned long long)ws, (unsigned long long)priv,
                         hnd, threads, gdi, user);
        Sleep(interval);
    }

    wprintf(L"\n--- summary ---\n");
    wprintf(L"samples       : %d\n", nCpu + 1);
    wprintf(L"CPU avg/peak  : %.2f%% / %.2f%%  (single core)\n", nCpu ? sumCpu / nCpu : 0, maxCpu);
    wprintf(L"CPU total     : %llu ms  (window %llu ms, average load %.2f%%)\n",
            prevCpu / 10000, (prevCpu - firstCpu) / 10000,
            prevWall ? (double)(prevCpu - firstCpu) / 10000.0 / prevWall * 100.0 : 0.0);
    wprintf(L"WS  first/peak: %llu / %llu KB  (delta %+lld KB)\n",
            (unsigned long long)firstWs, (unsigned long long)maxWs, (long long)maxWs - (long long)firstWs);
    wprintf(L"Priv first/peak: %llu / %llu KB  (delta %+lld KB)\n",
            (unsigned long long)firstPriv, (unsigned long long)maxPriv, (long long)maxPriv - (long long)firstPriv);
    wprintf(L"Handles first/peak: %lu / %lu\n", firstHnd, maxHnd);
    wprintf(L"Threads first/peak: %lu / %lu\n", firstThreads, maxThr);
    wprintf(L"GDI first/peak: %lu / %lu\n", firstGdi, maxGdi);
    wprintf(L"USER first/peak: %lu / %lu\n", firstUser, maxUser);

    if (csv) fclose(csv);
    CloseHandle(ph);
    return 0;
}
