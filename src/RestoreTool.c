/* ============================================================================
   RestoreTool.c —— 独立保底程序（与主程序不共用任何代码）
   作用：一键把系统恢复到“正常形态”：
         1. 结束所有 DesktopBeautify 进程
         2. 桌面图标强制可见且不透明
         3. 任务栏取消自动隐藏、取消透明
         4. 删除开机自启与右键菜单注册项
   特点：零第三方依赖、不读配置文件、不依赖主程序、失败也不会让情况更糟。
   编译：zig cc -target x86_64-windows-gnu -O2 -s "-Wl,--subsystem,windows" -o Restore.exe RestoreTool.c -luser32 -lshell32
   ============================================================================ */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#define CMD_TOGGLE_ICONS 0x7402
#define ABM_SETSTATE_    10
#define ABS_ALWAYSONTOP_ 2
#define WCA_ACCENT_POLICY_ 19
#define ACCENT_DISABLED_ 0
#define GWL_EXSTYLE_     (-20)
#define WS_EX_LAYERED_   0x00080000

typedef struct { int AccentState; int AccentFlags; int GradientColor; int AnimationId; } ACCENT_POLICY;
typedef struct { int Attribute; PVOID Data; SIZE_T SizeOfData; } WCA_DATA;
typedef BOOL (WINAPI *PFN_SETWCA)(HWND, WCA_DATA *);

static HWND g_found;
static const wchar_t *g_want;

static BOOL CALLBACK EnumTopCB(HWND h, LPARAM l)
{
    wchar_t cls[256];
    (void)l;
    GetClassNameW(h, cls, 256);
    if (_wcsicmp(cls, g_want) == 0) { g_found = h; return FALSE; }
    return TRUE;
}
static BOOL CALLBACK EnumChildCB(HWND h, LPARAM l)
{
    wchar_t cls[256];
    (void)l;
    GetClassNameW(h, cls, 256);
    if (_wcsicmp(cls, g_want) == 0) { g_found = h; return FALSE; }
    return TRUE;
}
static BOOL CALLBACK EnumDefViewCB(HWND h, LPARAM l)
{
    HWND d;
    (void)l;
    d = FindWindowExW(h, NULL, L"SHELLDLL_DefView", NULL);
    if (d) { g_found = d; return FALSE; }
    return TRUE;
}

static HWND TopLevelByClass(const wchar_t *cls)
{ g_found = NULL; g_want = cls; EnumWindows(EnumTopCB, 0); return g_found; }

static HWND FindDefView(void)
{
    HWND shell = GetShellWindow();
    if (shell)
    {
        HWND d = FindWindowExW(shell, NULL, L"SHELLDLL_DefView", NULL);
        if (d) return d;
    }
    g_found = NULL;
    EnumWindows(EnumDefViewCB, 0);
    return g_found;
}
static HWND FindChildByClass(HWND parent, const wchar_t *cls)
{
    if (!parent) return NULL;
    g_found = NULL; g_want = cls;
    EnumChildWindows(parent, EnumChildCB, 0);
    return g_found;
}

/* 结束所有同名进程（不含自己） */
static int KillProcesses(const wchar_t *exeName)
{
    HANDLE snap;
    PROCESSENTRY32W pe;
    int killed = 0;
    DWORD myPid = GetCurrentProcessId();
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (pe.th32ProcessID != myPid && _wcsicmp(pe.szExeFile, exeName) == 0)
            {
                HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (p)
                {
                    if (TerminateProcess(p, 0)) killed++;
                    CloseHandle(p);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return killed;
}

static void RestoreIcons(void)
{
    HWND d = FindDefView();
    HWND lv;
    LONG_PTR ex;
    DWORD_PTR r = 0;
    HKEY k;
    DWORD zero = 0;
    if (!d) return;
    lv = FindChildByClass(d, L"SysListView32");
    if (!lv) return;
    /* 先确保不透明：清掉可能残留的分层属性 */
    SetLayeredWindowAttributes(lv, 0, 255, LWA_ALPHA);
    ex = GetWindowLongPtrW(lv, GWL_EXSTYLE_);
    ex &= ~((LONG_PTR)WS_EX_LAYERED_);
    SetWindowLongPtrW(lv, GWL_EXSTYLE_, ex);
    if (!IsWindowVisible(lv))
        SendMessageTimeoutW(d, WM_COMMAND, (WPARAM)CMD_TOGGLE_ICONS, 0, SMTO_ABORTIFHUNG, 2000, &r);
    if (!IsWindowVisible(lv)) ShowWindow(lv, SW_SHOW);
    /* 同时把持久状态也改回“显示”，这样重启之后也一定是显示状态 */
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(k, L"HideIcons", 0, REG_DWORD, (const BYTE *)&zero, sizeof(zero));
        RegCloseKey(k);
    }
}

static void RestoreTaskbar(void)
{
    HWND t = TopLevelByClass(L"Shell_TrayWnd");
    APPBARDATA abd;
    ACCENT_POLICY ap;
    WCA_DATA wd;
    PFN_SETWCA fn;
    HMODULE u;
    if (!t) return;
    /* 关键：任务栏可能是被 ShowWindow(SW_HIDE) 藏起来的，必须显式恢复显示；
       并清掉可能残留的裁剪区域（上滑动画用 SetWindowRgn 做的）。 */
    SetWindowRgn(t, NULL, TRUE);
    ShowWindow(t, SW_SHOW);

    /* 另一种残留：整条平移动画中途崩溃会把任务栏留在屏幕外/错位。
       这里按"底边贴合显示器下边缘"把它摆回正常位置。
       必须带 SWP_NOSENDCHANGING —— shell 会在 WM_WINDOWPOSCHANGING 里
       把位置改回去，普通 SetWindowPos 会被它 1ms 内弹回。 */
    {
        RECT rc;
        MONITORINFO mi;
        HMONITOR mon;
        if (GetWindowRect(t, &rc))
        {
            mon = MonitorFromWindow(t, MONITOR_DEFAULTTONEAREST);
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfo(mon, &mi))
            {
                int h = rc.bottom - rc.top;
                if (h <= 0) h = 48;
                SetWindowPos(t, NULL, rc.left, mi.rcMonitor.bottom - h, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
            }
        }
    }

    ZeroMemory(&abd, sizeof(abd));
    abd.cbSize = sizeof(abd);
    abd.hWnd = t;
    abd.lParam = (LPARAM)ABS_ALWAYSONTOP_;
    SHAppBarMessage(ABM_SETSTATE_, &abd);

    u = GetModuleHandleW(L"user32.dll");
    fn = u ? (PFN_SETWCA)GetProcAddress(u, "SetWindowCompositionAttribute") : NULL;
    if (fn)
    {
        ZeroMemory(&ap, sizeof(ap));
        ap.AccentState = ACCENT_DISABLED_;
        ZeroMemory(&wd, sizeof(wd));
        wd.Attribute = WCA_ACCENT_POLICY_;
        wd.Data = &ap;
        wd.SizeOfData = sizeof(ap);
        fn(t, &wd);
    }
}

static void CleanRegistry(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &k) == ERROR_SUCCESS)
    {
        RegDeleteValueW(k, L"DesktopBeautify");
        RegCloseKey(k);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\DesktopBackground\\shell\\DesktopBeautify");
    SHChangeNotify(SHCNE_ASSOCCHANGED, 0, NULL, NULL);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    int killed;
    HWND lv;
    BOOL silent = FALSE;
    (void)hInst; (void)hPrev; (void)nCmdShow;

    /* --silent：不弹提示框（供脚本/自动化调用） */
    {
        const wchar_t *cl = GetCommandLineW();
        if (cl && (wcsstr(cl, L"--silent") != NULL || wcsstr(cl, L"/silent") != NULL)) silent = TRUE;
    }

    killed = KillProcesses(L"DesktopBeautify.exe");
    Sleep(400);
    RestoreIcons();
    Sleep(300);
    RestoreTaskbar();
    CleanRegistry();
    Sleep(200);

    lv = FindChildByClass(FindDefView(), L"SysListView32");
    if (!silent)
        MessageBoxW(NULL,
            L"\u5DF2\u5C1D\u8BD5\u6062\u590D\u5230\u6B63\u5E38\u72B6\u6001\u3002\n"
            L"Restore attempted:\n\n"
            L"  - Desktop icons  : visible and opaque\n"
            L"  - Taskbar        : shown, auto-hide off, transparency off\n"
            L"  - Autostart + context menu entry : removed\n\n"
            L"\u82E5\u56FE\u6807\u4ECD\u4E0D\u53EF\u89C1\uFF1A\u53F3\u952E\u684C\u9762 -> \u67E5\u770B -> \u663E\u793A\u684C\u9762\u56FE\u6807\n"
            L"\u82E5\u4EFB\u52A1\u680F\u4ECD\u5F02\u5E38\uFF1ACtrl+Shift+Esc -> \u91CD\u542F Windows \u8D44\u6E90\u7BA1\u7406\u5668",
            L"DesktopBeautify \u6062\u590D\u5DE5\u5177",
            MB_ICONINFORMATION | MB_SETFOREGROUND);
    (void)killed; (void)lv;
    return 0;
}
