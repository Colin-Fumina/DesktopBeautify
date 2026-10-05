/* envcheck.c —— 环境体检：打印自身与各级父进程的完整性级别、是否在沙箱容器里
   用途：一眼定位"启动环境是否被降权"，并说明它会导致哪些功能失效。
   编译：zig cc -target x86_64-windows-gnu -O2 -municode -o envcheck.exe envcheck.c -ladvapi32
   输出：控制台（用户可见）+ 同目录 envcheck.txt（便于远程读取） */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

static const char *RidName(DWORD rid)
{
    if (rid == 0xFFFFFFFF) return "denied";
    if (rid >= SECURITY_MANDATORY_SYSTEM_RID) return "System";
    if (rid >= SECURITY_MANDATORY_HIGH_RID)   return "High";
    if (rid >= SECURITY_MANDATORY_MEDIUM_RID) return "Medium  <-- 正常";
    if (rid >= SECURITY_MANDATORY_LOW_RID)    return "LOW     <-- 被降权";
    return "Untrusted";
}

static DWORD RidOf(HANDLE h)
{
    HANDLE tok = NULL;
    DWORD len = 0, rid = 0xFFFFFFFF;
    TOKEN_MANDATORY_LABEL *tml;
    if (!h) return rid;
    if (!OpenProcessToken(h, TOKEN_QUERY, &tok)) return rid;
    GetTokenInformation(tok, TokenIntegrityLevel, NULL, 0, &len);
    tml = (TOKEN_MANDATORY_LABEL *)HeapAlloc(GetProcessHeap(), 0, len ? len : 64);
    if (tml && GetTokenInformation(tok, TokenIntegrityLevel, tml, len, &len))
        rid = *GetSidSubAuthority(tml->Label.Sid, (DWORD)(*GetSidSubAuthorityCount(tml->Label.Sid) - 1));
    if (tml) HeapFree(GetProcessHeap(), 0, tml);
    CloseHandle(tok);
    return rid;
}

static BOOL InAppContainer(void)
{
    HANDLE tok = NULL; DWORD v = 0, len = 0; BOOL r = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
    {
        if (GetTokenInformation(tok, TokenIsAppContainer, &v, sizeof(v), &len)) r = (v != 0);
        CloseHandle(tok);
    }
    return r;
}

/* 测试能否写 HKCU\Software\Classes —— 这是右键菜单注册所必需的 */
static BOOL CanWriteClasses(void)
{
    HKEY k = NULL;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Classes\\DesktopBackground\\shell\\__EnvCheckProbe",
        0, NULL, 0, KEY_WRITE, NULL, &k, NULL);
    if (k) RegCloseKey(k);
    if (rc == ERROR_SUCCESS)
    {
        RegDeleteTreeW(HKEY_CURRENT_USER,
            L"Software\\Classes\\DesktopBackground\\shell\\__EnvCheckProbe");
        return TRUE;
    }
    return FALSE;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t outPath[MAX_PATH];
    FILE *f;
    DWORD pid = GetCurrentProcessId();
    DWORD selfRid = RidOf(GetCurrentProcess());
    BOOL canReg = CanWriteClasses();
    int depth, problems = 0;
    const wchar_t *tag = (argc > 1) ? argv[1] : L"(none)";

    SetConsoleOutputCP(65001);   /* 让控制台按 UTF-8 解释我们的输出 */
    setvbuf(stdout, NULL, _IONBF, 0);

    wcscpy(outPath, argv[0]);
    {
        wchar_t *p = wcsrchr(outPath, L'\\');
        if (p) *(p + 1) = 0;
    }
    wcscat(outPath, L"envcheck.txt");
    f = _wfopen(outPath, L"a");

    #define OUT(...) do { printf(__VA_ARGS__); if (f) { fprintf(f, __VA_ARGS__); } } while (0)

    OUT("\n");
    OUT("================ 环境体检 (tag=%ls) ================\n", tag);
    OUT("  AppContainer(沙箱容器) = %s\n", InAppContainer() ? "是" : "否");
    OUT("  可写 HKCU\\Software\\Classes = %s\n", canReg ? "是" : "否 <-- 右键菜单注册会失败");
    OUT("  进程链（名字 / PID / 完整性）:\n");

    for (depth = 0; depth < 6 && pid; depth++)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe;
        DWORD ppid = 0, rid;
        BOOL found = FALSE;
        wchar_t name[MAX_PATH] = L"?";
        char nm[128];
        HANDLE hp;
        if (snap == INVALID_HANDLE_VALUE) break;
        ZeroMemory(&pe, sizeof(pe));
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                if (pe.th32ProcessID == pid)
                {
                    wcsncpy(name, pe.szExeFile, MAX_PATH - 1);
                    name[MAX_PATH - 1] = 0;
                    ppid = pe.th32ParentProcessID;
                    found = TRUE;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        if (!found) break;
        hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        rid = RidOf(hp);
        if (hp) CloseHandle(hp);
        WideCharToMultiByte(CP_ACP, 0, name, -1, nm, sizeof(nm), NULL, NULL);
        OUT("    %-30s pid=%-7lu %s\n", nm, (unsigned long)pid, RidName(rid));
        if (depth == 0 && rid < SECURITY_MANDATORY_MEDIUM_RID) problems++;
        pid = ppid;
    }

    OUT("  ------------------------------------------------\n");
    if (selfRid < SECURITY_MANDATORY_MEDIUM_RID)
    {
        OUT("  结论：当前环境【无法运行桌面美化】。\n");
        OUT("        原因：Windows 禁止低完整性进程控制桌面图标/任务栏，\n");
        OUT("              也禁止写入 HKCU\\Software\\Classes。\n");
        OUT("        解决：在【文件资源管理器】里双击 安装.cmd，\n");
        OUT("              不要从浏览器/聊天窗口/助手面板里点开它。\n");
    }
    else
    {
        OUT("  结论：环境正常，可以正常运行桌面美化。\n");
    }
    OUT("====================================================\n\n");
    if (f) fclose(f);
    #undef OUT
    return problems ? 3 : 0;
}
