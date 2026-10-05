/* ============================================================================
   DesktopBeautify  ——  Windows 11 极简桌面美化小工具（原生 Win32 C）
   ----------------------------------------------------------------------------
   功能：
     1. 隐藏/显示全部桌面图标（走 shell 官方 0x7402 切换，状态自动落到 HideIcons）
     2. 隐藏时任务栏一并自动隐藏；按住空格时任务栏上滑显示
     3. 按住空格在桌面聚焦时预览图标，带「透明 -> 可见」淡入动画
     4. 任务栏完全透明（去掉白条）
     5. 桌面右键菜单项「显示/隐藏桌面图标与任务栏」，文字随状态变化
     6. 开机自启、完整安装/卸载、应急恢复，绝不把用户锁在无图标状态
   命令行：--tray(默认) --toggle --show --hide --restore --install --uninstall
   编译：  zig cc -target x86_64-windows-gnu -O2 -s -o DesktopBeautify.exe DesktopBeautify.c -luser32 -lshell32 -lgdi32 -lole32
   ============================================================================ */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <iso646.h>
#include <stdio.h>
#include <stdarg.h>

/* ---------------- 常量 ---------------- */
#define WM_TRAYICON      (WM_APP + 1)
#define IDT_POLL         1
#define IDT_TRAYANIM     4
#define IDT_TRAYDEMO     5
#define IDT_TRAYSHOW     6
#define IDT_ICONFADE     7
#define IDT_ICONCACHE    8
#define IDT_ICONHANDOVER 9
/* 实测（2560x1440、81 个桌面图标）：按住空格后
     - 任务栏从 ShowWindow 到真正画上屏幕约 50ms（XAML/DirectComposition 合成）
     - 桌面图标从 ShowWindow 到真正画出来约 270ms（需要重绘全部图标）
   若两条命令一起发，任务栏会比图标早约 220ms 出现，肉眼明显"不同时出现"。
   因此先发图标，延迟本值后再发任务栏，使两者同时呈现。 */
#define PEEK_TRAY_DELAY_MS 200
#define IDM_TOGGLE       101
#define IDM_TASKBAR      102
#define IDM_TRANS        103
#define IDM_AUTO         104
#define IDM_INSTALL      105
#define IDM_EXIT         106

#define GWL_EXSTYLE_      (-20)
#define WS_EX_LAYERED_    0x00080000
#define CMD_TOGGLE_ICONS  0x7402

#define ABM_GETSTATE_     4
#define ABM_SETSTATE_     10
#define ABS_AUTOHIDE_     1
#define ABS_ALWAYSONTOP_  2

#define WCA_ACCENT_POLICY_ 19
#define ACCENT_DISABLED_   0
#define ACCENT_TRANSPARENT_GRADIENT_ 2

#define PEEK_DELAY_MS    70
#define TRAYANIM_STEPS   8
#define TRAYANIM_MS      160

static const wchar_t *VERB_KEY = L"Software\\Classes\\DesktopBackground\\shell\\DesktopBeautify";
static const wchar_t *RUN_KEY  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *RUN_NAME = L"DesktopBeautify";

/* ---------------- 全局状态 ---------------- */
static HINSTANCE g_hInst;
static HWND      g_hwnd;
static HWND      g_listView;
static UINT      g_wmTaskbarCreated;
static BOOL      g_peek = FALSE;
static BOOL      g_iconsWereVisible = TRUE;
static BOOL      g_transparent = FALSE;   /* 任务栏透明默认关闭：
                                             Win11 的任务栏背景由 XAML/DComp 绘制，强调策略
                                             对该层无效；真正全透明只能往 explorer 注入
                                             XAML 诊断 DLL —— 代价与风险都不成比例，故不做。 */
static int       g_transMode = 0;      /* 0=关闭 1=清晰 2=模糊 3=亚克力 4=含子窗口 */
static BOOL      g_accentDirty = FALSE;/* 是否已施加过非零强调策略（避免无谓调用拖慢预览） */
static DWORD     g_peekStart = 0;      /* 预览开始时刻（防止按键抬起丢失导致一直显示） */
static BOOL      g_trayShowPending = FALSE; /* 本次预览中任务栏是否还在等延迟显示 */
static int       g_pollMs = 200;
static NOTIFYICONDATAW g_nid;
static wchar_t   g_exeDir[MAX_PATH];
static wchar_t   g_exePath[MAX_PATH];
static wchar_t   g_logPath[MAX_PATH];

/* ---- 前向声明（这些函数在文件后面定义） ---- */
static BOOL SetAutostartTask(BOOL on);
static void RemoveLegacyRunKey(void);
static BOOL MenuInstalled(void);
static BOOL CreateMainWindow(void);
static void PumpSleep(DWORD ms);
static void SetTaskbarTransparent(BOOL on);

/* SetWindowCompositionAttribute 未文档化，动态加载 */
typedef struct { int AccentState; int AccentFlags; int GradientColor; int AnimationId; } ACCENT_POLICY;
typedef struct { int Attribute; PVOID Data; SIZE_T SizeOfData; } WCA_DATA;
typedef BOOL (WINAPI *PFN_SETWCA)(HWND, WCA_DATA *);
static PFN_SETWCA GetSetWCA(void)
{
    static PFN_SETWCA fn = NULL;
    if (!fn)
    {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) fn = (PFN_SETWCA)GetProcAddress(u, "SetWindowCompositionAttribute");
    }
    return fn;
}

/* ---------------- 日志（纯 ASCII，避免编码问题） ---------------- */
static void Log(const char *fmt, ...)
{
    FILE *f = _wfopen(g_logPath, L"a");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02d-%02d %02d:%02d:%02d.%03d  ", st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ---------------- 窗口查找 ---------------- */
/* 用静态变量传递枚举结果，避免把 lParam 同时当输入输出 */
static HWND g_enumFound;
static const wchar_t *g_enumWant;
static BOOL CALLBACK EnumTopProc2(HWND h, LPARAM l)
{
    wchar_t cls[256];
    (void)l;
    GetClassNameW(h, cls, 256);
    if (_wcsicmp(cls, g_enumWant) == 0) { g_enumFound = h; return FALSE; }
    return TRUE;
}
static HWND TopLevelByClass(const wchar_t *cls)
{
    g_enumFound = NULL; g_enumWant = cls;
    EnumWindows(EnumTopProc2, 0);
    return g_enumFound;
}

static BOOL CALLBACK EnumChildProc(HWND h, LPARAM l)
{
    wchar_t cls[256];
    (void)l;
    GetClassNameW(h, cls, 256);
    if (_wcsicmp(cls, g_enumWant) == 0) { g_enumFound = h; return FALSE; }
    return TRUE;
}

static BOOL CALLBACK EnumTopForDefView(HWND h, LPARAM l)
{
    HWND d;
    (void)l;
    d = FindWindowExW(h, NULL, L"SHELLDLL_DefView", NULL);
    if (d) { g_enumFound = d; return FALSE; }
    return TRUE;
}

/* 定位桌面图标视图：GetShellWindow 主锚点 -> 全局枚举兜底 */
static HWND FindDefView(void)
{
    HWND shell = GetShellWindow();
    if (shell)
    {
        HWND d = FindWindowExW(shell, NULL, L"SHELLDLL_DefView", NULL);
        if (d) return d;
    }
    g_enumFound = NULL;
    EnumWindows(EnumTopForDefView, 0);
    return g_enumFound;
}

/* 实测：单层 FindWindowEx 找 SysListView32 会失败，必须递归枚举 */
static HWND FindChildByClass(HWND parent, const wchar_t *cls)
{
    if (!parent) return NULL;
    g_enumFound = NULL; g_enumWant = cls;
    EnumChildWindows(parent, EnumChildProc, 0);
    return g_enumFound;
}

static HWND ListView(void)
{
    HWND d = FindDefView();
    return d ? FindChildByClass(d, L"SysListView32") : NULL;
}

/* ---------------- 多显示器兼容 ----------------
   多显示器时 Windows 可能给每个显示器各建一套 WorkerW + SHELLDLL_DefView +
   SysListView32。只找一个会漏掉其它屏幕的图标（隐藏不干净）。
   所以这里枚举【全部】桌面图标视图；隐藏/显示对全部生效，
   而淡入动画用其中最大的那个（通常就是主屏）。 */
static int EnumAllListViews(HWND *out, int maxOut)
{
    int n = 0;
    HWND h, shell;
    wchar_t cls[64];
    if (!out or maxOut <= 0) return 0;

    shell = GetShellWindow();
    if (shell)
    {
        HWND d = FindChildByClass(shell, L"SHELLDLL_DefView");
        HWND lv = d ? FindChildByClass(d, L"SysListView32") : NULL;
        if (lv) out[n++] = lv;
    }
    for (h = GetTopWindow(NULL); h and n < maxOut; h = GetWindow(h, GW_HWNDNEXT))
    {
        HWND d, lv;
        int i, dup = 0;
        cls[0] = 0; GetClassNameW(h, cls, 64);
        if (_wcsicmp(cls, L"WorkerW") and _wcsicmp(cls, L"Progman")) continue;
        d = FindChildByClass(h, L"SHELLDLL_DefView");
        lv = d ? FindChildByClass(d, L"SysListView32") : NULL;
        if (!lv) continue;
        for (i = 0; i < n; i++) if (out[i] == lv) { dup = 1; break; }
        if (!dup) out[n++] = lv;
    }
    return n;
}

/* 对【全部】屏幕的图标视图统一显示/隐藏 */
static void ClearIconsLayered(HWND lv);      /* 前置声明：定义在后面 */
static void ShowAllListViews(BOOL show)
{
    HWND lvs[8];
    int n = EnumAllListViews(lvs, 8), i;
    for (i = 0; i < n; i++)
    {
        if (show)
        {
            ClearIconsLayered(lvs[i]);
            if (!IsWindowVisible(lvs[i])) ShowWindowAsync(lvs[i], SW_SHOW);
        }
        else if (IsWindowVisible(lvs[i])) ShowWindowAsync(lvs[i], SW_HIDE);
    }
}

/* ---------------- 图标显示/隐藏 ---------------- */
static BOOL IconsVisible(void)
{
    HWND lvs[8];
    int n = EnumAllListViews(lvs, 8), i;
    for (i = 0; i < n; i++)
        if (IsWindowVisible(lvs[i])) return TRUE;
    return FALSE;
}

/* 安全兜底：把图标窗口的不透明度强制拉满并摘掉分层属性。
   用于任何“要显示图标”的路径，避免上一次预览残留 alpha=0 导致“可见但全透明”。 */
static void ForceIconsOpaqueOn(HWND lv)
{
    LONG_PTR ex;
    if (!lv) return;
    ex = GetWindowLongPtrW(lv, GWL_EXSTYLE_);
    if (ex & WS_EX_LAYERED_)     /* 只有残留分层属性时才需要清理（正常路径不会走到） */
    {
        SetLayeredWindowAttributes(lv, 0, 255, LWA_ALPHA);
        SetWindowLongPtrW(lv, GWL_EXSTYLE_, ex & ~((LONG_PTR)WS_EX_LAYERED_));
    }
    if (!IsWindowVisible(lv)) ShowWindowAsync(lv, SW_SHOW);
}

/* 只清理残留的分层属性，不显示窗口。图标淡入路径要用它：
   淡入期间真 ListView 必须保持隐藏，否则真图标会盖掉淡入效果。 */
static void ClearIconsLayered(HWND lv)
{
    LONG_PTR ex;
    if (!lv) return;
    ex = GetWindowLongPtrW(lv, GWL_EXSTYLE_);
    if (ex & WS_EX_LAYERED_)
    {
        SetLayeredWindowAttributes(lv, 0, 255, LWA_ALPHA);
        SetWindowLongPtrW(lv, GWL_EXSTYLE_, ex & ~((LONG_PTR)WS_EX_LAYERED_));
    }
}

static void ForceIconsOpaque(void)
{
    HWND lvs[8];
    int n = EnumAllListViews(lvs, 8), i;
    for (i = 0; i < n; i++) ForceIconsOpaqueOn(lvs[i]);
}

/* ---------- 图标显示/隐藏 ----------
   设计要点（实测教训）：
   0x7402 是"取反"语义，而窗口可见性与 shell 的内部状态可能脱节，于是"读可见性
   再取反"会翻错方向（实测 --hide 失败）。因此改为：
     1) 以注册表 HideIcons 作为 shell 持久状态的基准；只有它和目标不一致时才发 0x7402
        （这样 shell 的内部状态与缓存值都会落到目标上）；
     2) 视觉状态一律用绝对操作强制（ShowWindow/不透明度），不依赖取反；
     3) 最后校验注册表，必要时显式写入。 */
static DWORD ReadHideIcons(void)
{
    HKEY k;
    DWORD v = 0xFFFFFFFF, sz = sizeof(v), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return 0xFFFFFFFF;
    RegQueryValueExW(k, L"HideIcons", NULL, &type, (BYTE *)&v, &sz);
    RegCloseKey(k);
    return v;
}

static void WriteHideIcons(DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    RegSetValueExW(k, L"HideIcons", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
    RegCloseKey(k);
    SHChangeNotify(SHCNE_ASSOCCHANGED, 0, NULL, NULL);
    Log("WriteHideIcons(%lu)", (unsigned long)v);
}

static BOOL SetIconsVisible(BOOL visible)
{
    HWND d = FindDefView();
    DWORD want = visible ? 0 : 1;
    DWORD cur;
    int i;
    if (!d) { Log("SetIconsVisible: DefView not found"); return FALSE; }

    cur = ReadHideIcons();
    if (cur != 0xFFFFFFFF && cur != want)
    {
        DWORD_PTR r = 0;
        SetLastError(0);
        SendMessageTimeoutW(d, WM_COMMAND, (WPARAM)CMD_TOGGLE_ICONS, 0, SMTO_ABORTIFHUNG, 2000, &r);
        Log("SetIconsVisible(%d): sent 0x7402 (HideIcons was %lu) rc=%Id err=%lu",
            visible, (unsigned long)cur, (LRESULT)r, (unsigned long)GetLastError());
        Sleep(150);
    }

    /* 视觉状态用绝对操作强制（对全部屏幕） */
    if (visible) ForceIconsOpaque();              /* 含 SW_SHOW + 不透明度拉满 */
    else ShowAllListViews(FALSE);

    /* 收敛校验：注册表与视觉都必须是目标状态 */
    for (i = 0; i < 5; i++)
    {
        DWORD nowReg = ReadHideIcons();
        BOOL nowVis = IconsVisible();
        if ((nowReg == want || nowReg == 0xFFFFFFFF) && nowVis == visible) break;
        if (nowReg != 0xFFFFFFFF && nowReg != want)
        {
            DWORD_PTR r = 0;
            SendMessageTimeoutW(d, WM_COMMAND, (WPARAM)CMD_TOGGLE_ICONS, 0, SMTO_ABORTIFHUNG, 2000, &r);
            Sleep(150);
        }
        if (visible) ForceIconsOpaque();
        else ShowAllListViews(FALSE);
    }
    if (ReadHideIcons() != want) WriteHideIcons(want);

    Log("SetIconsVisible(%d) -> visible=%d HideIcons=%lu", visible, IconsVisible(),
        (unsigned long)ReadHideIcons());
    return IconsVisible() == visible;
}

/* ---------------- 任务栏 ----------------
   实测结论（本机 Win11 25H2 26200，中完整性）：
     * SHAppBarMessage(ABM_SETSTATE) 完全无效——任务栏位置/可见性/注册表都不变；
     * SetWindowPos 移动或改变任务栏高度被 shell 忽略；
     * AnimateWindow 跨进程调用返回 FALSE；
     * ShowWindow 可以真正隐藏/显示任务栏  ✔
     * SetWindowRgn 可以裁剪任务栏，逐帧改变裁剪高度 = 从底边向上"长"出来  ✔
   因此：隐藏用 ShowWindow，上滑动画用 SetWindowRgn 逐帧放大裁剪区域。 */
static BOOL g_taskbarHidden = FALSE;   /* 期望的持久状态（缓存，仅用于日志） */
static BOOL g_trayWasHiddenAtPeek = FALSE;

static HWND TrayWnd(void) { return TopLevelByClass(L"Shell_TrayWnd"); }

/* 句柄缓存：预览是高频热路径，每次都枚举窗口会带来几十毫秒开销，
   既拖慢响应也会让图标与任务栏出现不同步。句柄只在 explorer 重启后失效，
   收到 TaskbarCreated 时清空缓存即可。 */
static HWND g_cachedLV = NULL;
static HWND g_cachedTray = NULL;
/* 取"最大的那个"桌面图标视图当作动画主体（多显示器时通常就是主屏）。
   隐藏/显示走 ShowAllListViews，覆盖全部屏幕。 */
static HWND CachedLV(void)
{
    if (!g_cachedLV || !IsWindow(g_cachedLV))
    {
        HWND lvs[8];
        int n = EnumAllListViews(lvs, 8), i, best = -1;
        long area, ba = -1;
        RECT r;
        for (i = 0; i < n; i++)
        {
            if (!GetWindowRect(lvs[i], &r)) continue;
            area = (long)(r.right - r.left) * (long)(r.bottom - r.top);
            if (area > ba) { ba = area; best = i; }
        }
        g_cachedLV = (best >= 0) ? lvs[best] : NULL;
    }
    return g_cachedLV;
}
static HWND CachedTray(void)
{
    if (!g_cachedTray || !IsWindow(g_cachedTray)) g_cachedTray = TrayWnd();
    return g_cachedTray;
}

/* 任务栏是否应当隐藏 —— 永远以图标状态（shell 的 HideIcons）为准，而不是本地变量。
   原因：右键菜单项触发的是另一个进程（--toggle），常驻进程里的变量会过期。 */
static BOOL TaskbarShouldBeHidden(void) { return !IconsVisible(); }

static BOOL TaskbarIsHiddenNow(void)
{
    HWND t = TrayWnd();
    return (t != NULL) && !IsWindowVisible(t);
}

/* visibleHeight: 0..全高。>=全高 或 <0 表示清除裁剪区域 */
static BOOL g_regionSet = FALSE;
static void TaskbarRegion(HWND t, int visibleHeight)
{
    RECT rc;
    int h;
    HRGN rgn;
    if (!t) return;
    if (!GetWindowRect(t, &rc)) return;
    h = rc.bottom - rc.top;
    if (h <= 0) h = 48;
    if (visibleHeight >= h || visibleHeight < 0)
    {
        if (g_regionSet) { SetWindowRgn(t, NULL, TRUE); g_regionSet = FALSE; }
        return;
    }
    if (visibleHeight < 1) visibleHeight = 1;   /* 区域不能为空 */
    rgn = CreateRectRgn(0, h - visibleHeight, rc.right - rc.left, h);
    if (SetWindowRgn(t, rgn, TRUE)) g_regionSet = TRUE;
    else DeleteObject(rgn);                     /* 成功后区域归系统所有，不能删 */
}

/* ------------------------------------------------------------------
   任务栏【整条平移】动画
   本机实测结论（Win11 25H2）：
     - 普通 SetWindowPos 会在 1ms 内被 shell 弹回：它在自己的
       TrayUI::WndProc 里处理 WM_WINDOWPOSCHANGING 时把位置改回去。
     - 加上 SWP_NOSENDCHANGING 后该消息根本不发出，shell 无从干预，
       位置可稳定保持 —— 于是我们可以自己逐帧平移整条任务栏。
   安全前提：任何异常路径都必须能把任务栏送回"正常位置"，
   绝不能把它留在屏幕外。
------------------------------------------------------------------- */
#define SLIDE_FRAMES    16
#define SLIDE_INTERVAL  15          /* ms/帧，全程约 240ms */

static int  g_animFromY = 0;
static int  g_animToY = 0;
static int  g_animStep = 0;
static BOOL g_animHideAfter = FALSE;
static BOOL g_animActive = FALSE;

/* 任务栏"正常位置"缓存（底边贴住显示器下边缘时的矩形） */
static int  g_tbLeft = 0, g_tbNormalTop = 0, g_tbHeight = 48;
static BOOL g_tbRectKnown = FALSE;

static double EaseOutCubic(double t)
{
    double u = 1.0 - t;
    return 1.0 - u * u * u;
}

/* 只有看起来处于正常位置时才更新缓存 */
static void CaptureTrayNormalRect(HWND t)
{
    RECT rc;
    MONITORINFO mi;
    HMONITOR mon;
    if (!t || !GetWindowRect(t, &rc)) return;
    mon = MonitorFromWindow(t, MONITOR_DEFAULTTONEAREST);
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(mon, &mi)) return;
    if (rc.bottom >= mi.rcMonitor.bottom - 1 && rc.bottom <= mi.rcMonitor.bottom + 1)
    {
        g_tbLeft = rc.left;
        g_tbNormalTop = rc.top;
        g_tbHeight = rc.bottom - rc.top;
        g_tbRectKnown = TRUE;
    }
}

/* 唯一的移动入口：必须带 SWP_NOSENDCHANGING，否则会被 shell 立刻弹回 */
static void TrayMoveTo(HWND t, int y)
{
    if (!t) return;
    SetWindowPos(t, NULL, g_tbLeft, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
}

/* 安全兜底：把任务栏送回正常位置并清除裁剪区域 */
static void TaskbarNormalize(void)
{
    HWND t = TrayWnd();
    if (!t) return;
    TaskbarRegion(t, -1);
    if (!g_tbRectKnown) CaptureTrayNormalRect(t);
    if (g_tbRectKnown) TrayMoveTo(t, g_tbNormalTop);
}

static void SlideTick(void)
{
    HWND t = TrayWnd();
    double p, e;
    int y;
    if (!t || !g_tbRectKnown)
    {
        KillTimer(g_hwnd, IDT_TRAYANIM);
        g_animActive = FALSE;
        return;
    }
    g_animStep++;
    if (g_animStep >= SLIDE_FRAMES)
    {
        KillTimer(g_hwnd, IDT_TRAYANIM);
        g_animActive = FALSE;
        TrayMoveTo(t, g_animToY);
        if (g_animHideAfter) ShowWindow(t, SW_HIDE);
        Log("taskbar slide done (to=%d hideAfter=%d)", g_animToY, g_animHideAfter);
        return;
    }
    p = (double)g_animStep / (double)SLIDE_FRAMES;
    e = EaseOutCubic(p);
    y = g_animFromY + (int)((g_animToY - g_animFromY) * e + 0.5);
    TrayMoveTo(t, y);
}

static void StartSlide(int fromY, int toY, BOOL hideAfter)
{
    g_animFromY = fromY;
    g_animToY = toY;
    g_animStep = 0;
    g_animHideAfter = hideAfter;
    g_animActive = TRUE;
    TrayMoveTo(TrayWnd(), fromY);
    SetTimer(g_hwnd, IDT_TRAYANIM, SLIDE_INTERVAL, NULL);
}

/* 显示：整条从屏幕下边缘升起来 */
static void TaskbarSlideUp(void)
{
    HWND t = TrayWnd();
    RECT rc;
    int fromY;
    if (!t) return;
    TaskbarRegion(t, -1);
    if (!IsWindowVisible(t)) ShowWindow(t, SW_SHOW);
    CaptureTrayNormalRect(t);
    if (!g_tbRectKnown) { Log("slide up: rect unknown, instant show"); return; }
    SetTaskbarTransparent(g_transparent);
    fromY = g_tbNormalTop + g_tbHeight;                       /* 默认从屏幕外开始 */
    if (GetWindowRect(t, &rc) && rc.top > g_tbNormalTop)      /* 若上次动画被打断，接着当前位置继续 */
        fromY = rc.top;
    StartSlide(fromY, g_tbNormalTop, FALSE);
    Log("taskbar slide up: %d -> %d", fromY, g_tbNormalTop);
}

/* 隐藏：整条滑下去，滑完再隐藏窗口 */
static void TaskbarSlideDown(void)
{
    HWND t = TrayWnd();
    RECT rc;
    int fromY;
    if (!t) return;
    CaptureTrayNormalRect(t);
    if (!g_tbRectKnown) { ShowWindow(t, SW_HIDE); return; }
    fromY = g_tbNormalTop;
    if (GetWindowRect(t, &rc) && rc.top > g_tbNormalTop) fromY = rc.top;
    StartSlide(fromY, g_tbNormalTop + g_tbHeight, TRUE);
    Log("taskbar slide down: %d -> %d", fromY, g_tbNormalTop + g_tbHeight);
}

/* 立即显示（切换/恢复等非动画路径） */
static void TaskbarShow(void)
{
    HWND t = TrayWnd();
    if (!t) return;
    KillTimer(g_hwnd, IDT_TRAYANIM);
    g_animActive = FALSE;
    TaskbarRegion(t, -1);
    if (!IsWindowVisible(t)) ShowWindow(t, SW_SHOW);
    CaptureTrayNormalRect(t);
    if (g_tbRectKnown) TrayMoveTo(t, g_tbNormalTop);
    SetTaskbarTransparent(g_transparent);
    Log("taskbar shown (instant)");
}

static void TaskbarHide(void)
{
    HWND t = TrayWnd();
    if (!t) return;
    KillTimer(g_hwnd, IDT_TRAYANIM);
    g_animActive = FALSE;
    TaskbarRegion(t, -1);
    CaptureTrayNormalRect(t);
    if (g_tbRectKnown) TrayMoveTo(t, g_tbNormalTop);
    ShowWindow(t, SW_HIDE);
    Log("taskbar hidden (instant)");
}

static void SetTaskbarHidden(BOOL hidden)
{
    HWND t = TrayWnd();
    g_taskbarHidden = hidden;
    if (!t) { Log("SetTaskbarHidden: no Shell_TrayWnd"); return; }
    KillTimer(g_hwnd, IDT_TRAYANIM);
    g_animActive = FALSE;
    TaskbarRegion(t, -1);
    if (!hidden)
    {
        if (!IsWindowVisible(t)) ShowWindow(t, SW_SHOW);
        CaptureTrayNormalRect(t);
        if (g_tbRectKnown) TrayMoveTo(t, g_tbNormalTop);
        SetTaskbarTransparent(g_transparent);
    }
    else
    {
        CaptureTrayNormalRect(t);
        if (g_tbRectKnown) TrayMoveTo(t, g_tbNormalTop);
        ShowWindow(t, SW_HIDE);
    }
    Log("taskbar hidden = %d (visible=%d)", hidden, IsWindowVisible(t));
}

/* 看门狗：处于"应隐藏"状态时，若 shell 又把任务栏显示出来就再藏回去；
   并确保任务栏没被留在错位/屏幕外的位置（防止动画中途崩溃把人锁在外面）。 */
static void TaskbarWatchdog(void)
{
    HWND t;
    RECT rc;
    if (g_animActive) return;      /* 动画进行中不插手 */
    if (g_peek) return;
    if (!TaskbarShouldBeHidden()) return;
    t = TrayWnd();
    if (!t) return;
    if (IsWindowVisible(t))
    {
        Log("watchdog: taskbar reappeared, hiding again");
        TaskbarRegion(t, -1);
        ShowWindow(t, SW_HIDE);
        return;
    }
    if (g_tbRectKnown && GetWindowRect(t, &rc) && rc.top != g_tbNormalTop)
    {
        Log("watchdog: taskbar stuck at %d (normal %d), restoring", rc.top, g_tbNormalTop);
        TrayMoveTo(t, g_tbNormalTop);
    }
}

/* 当前进程的完整性级别。低于 Medium 时无法给 explorer 发窗口消息（UIPI），
   程序会静默失效——启动时检测并明确告警，而不是让用户面对一个没反应的工具。 */
static DWORD RidOfProcess(HANDLE h)
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

static DWORD GetIntegrityRid(void) { return RidOfProcess(GetCurrentProcess()); }

/* 是否运行在 AppContainer（沙箱容器）里：这类进程的 HKCU\Software\Classes 是被
   虚拟化隔离的，写真实注册表会得到 ACCESS_DENIED，给 explorer 发消息也会被拦。 */
static BOOL IsInAppContainer(void)
{
    HANDLE tok = NULL;
    DWORD isAc = 0, len = 0;
    BOOL r = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
    {
        if (GetTokenInformation(tok, TokenIsAppContainer, &isAc, sizeof(isAc), &len)) r = (isAc != 0);
        CloseHandle(tok);
    }
    return r;
}

static const char *RidText(DWORD rid)
{
    if (rid == 0xFFFFFFFF) return "denied";
    if (rid >= SECURITY_MANDATORY_SYSTEM_RID) return "System";
    if (rid >= SECURITY_MANDATORY_HIGH_RID) return "High";
    if (rid >= SECURITY_MANDATORY_MEDIUM_RID) return "Medium";
    if (rid >= SECURITY_MANDATORY_LOW_RID) return "LOW";
    return "Untrusted";
}

/* 把自身与各级父进程的完整性写进日志——用来定位"降权发生在哪一层" */
static void LogIdentityChain(void)
{
    DWORD pid = GetCurrentProcessId();
    int depth;
    Log("---- identity chain ----");
    Log("  appContainer=%d  selfRid=0x%04lX (%s)", IsInAppContainer(),
        (unsigned long)GetIntegrityRid(), RidText(GetIntegrityRid()));
    for (depth = 0; depth < 5 && pid; depth++)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe;
        DWORD ppid = 0, rid;
        BOOL found = FALSE;
        wchar_t name[MAX_PATH] = L"?";
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
        rid = RidOfProcess(hp);
        if (hp) CloseHandle(hp);
        {
            char nm[128];
            WideCharToMultiByte(CP_ACP, 0, name, -1, nm, sizeof(nm), NULL, NULL);
            Log("  %-30s pid=%-7lu rid=0x%04lX (%s)", nm, (unsigned long)pid,
                (unsigned long)rid, RidText(rid));
        }
        pid = ppid;
    }
    Log("------------------------");
}

/* Win11 的任务栏背景由 XAML/DirectComposition 子窗口绘制，不是 Shell_TrayWnd 自己画的。
   所以模式 4 会把强调策略同时施加到真正绘制背景的那两个子窗口上。 */
static BOOL CALLBACK AccentChildProc(HWND h, LPARAM l)
{
    PFN_SETWCA fn = GetSetWCA();
    WCA_DATA wd;
    if (!fn) return TRUE;
    ZeroMemory(&wd, sizeof(wd));
    wd.Attribute = WCA_ACCENT_POLICY_;
    wd.Data = (PVOID)l;
    wd.SizeOfData = sizeof(ACCENT_POLICY);
    fn(h, &wd);
    return TRUE;
}

static HWND FindChildByClassName(HWND parent, const wchar_t *cls)
{
    HWND found = NULL;
    g_enumFound = NULL;
    g_enumWant = cls;
    EnumChildWindows(parent, EnumChildProc, 0);
    found = g_enumFound;
    return found;
}

static void SetTaskbarTransparent(BOOL on)
{
    HWND t = TopLevelByClass(L"Shell_TrayWnd");
    ACCENT_POLICY ap;
    WCA_DATA wd;
    PFN_SETWCA fn = GetSetWCA();
    if (!t || !fn) { Log("transparent: no tray or no API"); return; }
    if ((!on || g_transMode == 0) && !g_accentDirty) return;   /* 本来就没设过，不必多此一举 */
    ZeroMemory(&ap, sizeof(ap));
    if (!on || g_transMode == 0)
    {
        ap.AccentState = 0;                 /* ACCENT_DISABLED */
        ap.GradientColor = 0;
        g_accentDirty = FALSE;
    }
    else if (g_transMode == 1)
    {
        ap.AccentState = 2;                 /* 全透明 */
        ap.GradientColor = 0x00000000;
    }
    else if (g_transMode == 2)
    {
        ap.AccentState = 3;                 /* 模糊 */
        ap.GradientColor = 0x00000000;
    }
    else
    {
        ap.AccentState = 4;                 /* 亚克力 */
        ap.GradientColor = 0x01000000;
        g_accentDirty = TRUE;
    }
    if (g_transMode >= 1 && g_transMode <= 3) g_accentDirty = TRUE;
    ap.AccentFlags = 0;
    ZeroMemory(&wd, sizeof(wd));
    wd.Attribute = WCA_ACCENT_POLICY_;
    wd.Data = &ap;
    wd.SizeOfData = sizeof(ap);
    fn(t, &wd);

    if (on && g_transMode == 4)
    {
        /* 模式 4：同时施加到真正绘制背景的 XAML/DComp 子窗口 */
        HWND c1 = FindChildByClassName(t, L"Windows.UI.Composition.DesktopWindowContentBridge");
        HWND c2 = FindChildByClassName(t, L"Windows.UI.Core.CoreWindow");
        if (c1) AccentChildProc(c1, (LPARAM)&ap);
        if (c2) AccentChildProc(c2, (LPARAM)&ap);
    }
    Log("taskbar transparency: on=%d mode=%d state=%d", on, g_transMode, ap.AccentState);
}

/* 深色任务栏等系统级设置改动已按用户要求取消（任务栏透明暂不做）。 */

static const char *TransModeName(void)
{
    switch (g_transMode)
    {
    case 0: return "off";
    case 1: return "clear";
    case 2: return "blur";
    case 3: return "acrylic";
    default: return "children";
    }
}

static void SaveTransMode(void)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\DesktopBeautify", 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS)
    {
        DWORD v = (DWORD)g_transMode;
        RegSetValueExW(k, L"TransparentMode", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
}

static void LoadTransMode(void)
{
    HKEY k;
    DWORD v = 0, sz = sizeof(v), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\DesktopBeautify", 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(k, L"TransparentMode", NULL, &type, (BYTE *)&v, &sz) != ERROR_SUCCESS) v = 0;
        RegCloseKey(k);
    }
    g_transMode = (int)(v % 5);
    g_transparent = (g_transMode != 0);
}

/* ---------------- 桌面是否聚焦 ---------------- */
static BOOL DesktopFocused(void)
{
    HWND fg = GetForegroundWindow();
    HWND shell;
    wchar_t cls[256];
    if (!fg) return FALSE;
    shell = GetShellWindow();
    if (shell && GetAncestor(fg, GA_ROOT) == shell) return TRUE;
    GetClassNameW(fg, cls, 256);
    return (_wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0 ||
            _wcsicmp(cls, L"SHELLDLL_DefView") == 0 || _wcsicmp(cls, L"SysListView32") == 0);
}

/* ---------------- 注册表：右键菜单 / 自启 ---------------- */
static void SyncMenuText(void)
{
    HKEY k;
    const wchar_t *want = IconsVisible() ? L"隐藏桌面图标与任务栏" : L"显示桌面图标与任务栏";
    if (RegOpenKeyExW(HKEY_CURRENT_USER, VERB_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegSetValueExW(k, L"MUIVerb", 0, REG_SZ, (const BYTE *)want, (DWORD)((wcslen(want) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    SHChangeNotify(SHCNE_ASSOCCHANGED, 0, NULL, NULL);
    Log("menu text synced");
}

static BOOL InstallMenu(void)
{
    HKEY k = NULL, c = NULL;
    DWORD disp = 0;
    LONG rc, rc2;
    BOOL ok = FALSE;
    const wchar_t *txt = IconsVisible() ? L"隐藏桌面图标与任务栏" : L"显示桌面图标与任务栏";
    wchar_t cmd[MAX_PATH + 32];
    rc = RegCreateKeyExW(HKEY_CURRENT_USER, VERB_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, &disp);
    if (rc != ERROR_SUCCESS)
    {
        Log("InstallMenu: RegCreateKeyEx(verb) FAILED rc=%ld", (long)rc);
        return FALSE;
    }
    RegSetValueExW(k, L"MUIVerb", 0, REG_SZ, (const BYTE *)txt, (DWORD)((wcslen(txt) + 1) * sizeof(wchar_t)));
    RegSetValueExW(k, L"Position", 0, REG_SZ, (const BYTE *)L"Bottom", 7 * sizeof(wchar_t));
    rc2 = RegCreateKeyExW(k, L"command", 0, NULL, 0, KEY_WRITE, NULL, &c, &disp);
    if (rc2 == ERROR_SUCCESS)
    {
        _snwprintf(cmd, MAX_PATH + 31, L"\"%s\" --toggle", g_exePath);
        RegSetValueExW(c, L"", 0, REG_SZ, (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
        RegCloseKey(c);
        ok = TRUE;
    }
    else Log("InstallMenu: RegCreateKeyEx(command) FAILED rc=%ld", (long)rc2);
    RegCloseKey(k);
    SHChangeNotify(SHCNE_ASSOCCHANGED, 0, NULL, NULL);
    Log("InstallMenu %s (disp=%lu)", ok ? "done" : "FAILED", (unsigned long)disp);
    return ok;
}

static void UninstallAll(void)
{
    RegDeleteTreeW(HKEY_CURRENT_USER, VERB_KEY);
    RemoveLegacyRunKey();
    SetAutostartTask(FALSE);
    SHChangeNotify(SHCNE_ASSOCCHANGED, 0, NULL, NULL);
    Log("uninstalled");
}

static BOOL MenuInstalled(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, VERB_KEY, 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS)
    {
        RegCloseKey(k);
        return TRUE;
    }
    return FALSE;
}

/* ---------- 计划任务（替代 Run 键）----------
   为什么不用 HKCU\...\Run：开机时由 explorer 通过 shell 启动，未签名程序会被
   SmartScreen 拦截（"Windows 已保护你的电脑"），导致每次开机都启动失败。
   计划任务由任务计划服务直接 CreateProcess，不经过 shell，因此不会被拦。 */
static BOOL RunAndWait(const wchar_t *exe, wchar_t *args, DWORD timeoutMs, DWORD *exitCode)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD rc = 1;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(exe, args, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
    {
        Log("RunAndWait: CreateProcess(%ls) failed err=%lu", exe, (unsigned long)GetLastError());
        return FALSE;
    }
    WaitForSingleObject(pi.hProcess, timeoutMs);
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exitCode) *exitCode = rc;
    return TRUE;
}

#define TASK_NAME L"DesktopBeautify"

static BOOL SetAutostartTask(BOOL on)
{
    wchar_t sys[MAX_PATH], args[1400];
    DWORD rc = 1;
    BOOL ok;
    GetSystemDirectoryW(sys, MAX_PATH);
    wcscat(sys, L"\\schtasks.exe");
    if (on)
        _snwprintf(args, 1399,
            L"/create /tn \"" TASK_NAME L"\" /tr \"\\\"%s\\\" --tray\" /sc onlogon /rl limited /f",
            g_exePath);
    else
        _snwprintf(args, 1399, L"/delete /tn \"" TASK_NAME L"\" /f");
    RunAndWait(sys, args, 15000, &rc);
    ok = (rc == 0);
    Log("SetAutostartTask(%d) -> schtasks rc=%lu ok=%d", on, (unsigned long)rc, ok);

    if (on && !ok)
    {
        /* 计划任务需要管理员权限；拿不到就退回 Run 键。
           注意：Run 键是开机时由 explorer 通过 shell 启动的，未签名程序可能被
           SmartScreen 拦（届时手动双击一下即可）。 */
        HKEY rk;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &rk) == ERROR_SUCCESS)
        {
            wchar_t cmd[MAX_PATH + 32];
            _snwprintf(cmd, MAX_PATH + 31, L"\"%s\" --tray", g_exePath);
            ok = (RegSetValueExW(rk, RUN_NAME, 0, REG_SZ, (const BYTE *)cmd,
                                 (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
            RegCloseKey(rk);
            Log("autostart: fell back to Run key -> %d", ok);
        }
    }
    if (!on) RemoveLegacyRunKey();
    return ok;
}

static BOOL AutostartOn(void)
{
    wchar_t sys[MAX_PATH], args[256];
    DWORD rc = 1;
    BOOL byTask, byRun = FALSE;
    HKEY rk;
    GetSystemDirectoryW(sys, MAX_PATH);
    wcscat(sys, L"\\schtasks.exe");
    _snwprintf(args, 255, L"/query /tn \"" TASK_NAME L"\"");
    RunAndWait(sys, args, 10000, &rc);
    byTask = (rc == 0);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &rk) == ERROR_SUCCESS)
    {
        byRun = (RegQueryValueExW(rk, RUN_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
        RegCloseKey(rk);
    }
    return byTask || byRun;
}

/* 兼容旧版本：清掉曾经的 Run 键项 */
static void RemoveLegacyRunKey(void)
{
    HKEY rk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &rk) == ERROR_SUCCESS)
    {
        RegDeleteValueW(rk, RUN_NAME);
        RegCloseKey(rk);
    }
}

/* ---------------- 统一状态切换 ---------------- */
static void ApplyHidden(BOOL hidden)
{
    SetIconsVisible(!hidden);
    SetTaskbarHidden(hidden);
    SyncMenuText();
}

/* ---------------- 淡入淡出 ---------------- */

/* ================= 图标淡入：自绘逐像素透明覆盖层 =================
   原理（v10/v11 实测验证，两个数字都是量出来的）：
   · 桌面 ListView 的真实表面 = "图标叠在纯黑上"，所以 PrintWindow 印出来的
     就是已经预乘好的像素：max(R,G,B) 就是覆盖率，纯黑处就是背景。
   · 我们自己开一个【最顶层 + 逐像素透明】的覆盖层，只画图标那些像素 ——
     壁纸一个像素都不被覆盖（实测：所有 alpha 下壁纸样本变化 = 噪声水平）。
   · 淡入 = 每帧只改 BLENDFUNCTION.SourceConstantAlpha 一个数字，零像素运算。
   · 真 ListView 同时在覆盖层背后重绘（约 270ms），淡入结束（约 256ms）时它
     已经画好，撤掉覆盖层即无缝交接 —— 顺手把那 270ms 重绘延迟也藏掉了。
   任何一步失败都直接跳过淡入走原来的"直接显示"，功能不受影响。 */

#define ICONFADE_ALPHA_TH  8      /* ≥8 视为图标像素（背景是精确纯黑，实测） */
#define ICONFADE_INTERVAL  16
#define ICONFADE_FRAMES    10     /* ≈250ms，与任务栏上滑 240ms 同步 */
#define ICONFADE_MAX_MS    1500   /* 保险丝：幕布无论如何不能活过 1.5 秒 */
#define ICONCACHE_MAXAGE_MS 120000 /* 松手时若素材比这还旧，就在那里补抓一次 */
#define ICONCACHE_MIN_INTERVAL_MS 2000 /* 重抓节流：2 秒内不重复抓，防连点空格反复抓图 */
#define ICONCACHE_CONTENT_MIN    8 /* 黑底抓图里"有内容"的判定：≥8 就算（背景是精确纯黑） */
#define ICONFADE_HOLD_FRAMES     5  /* 淡入到满后的保持帧数（等真图标在背后画完） */
#define ICONHANDOVER_MS        320  /* 显示真 ListView 后等它重绘完的时间 */

static HWND    g_ovlWnd;
static HBITMAP g_ovlBmp;
static HDC     g_ovlMemDC;
static int     g_ovlX, g_ovlY, g_ovlW, g_ovlH;
static int     g_ovlAlpha, g_ovlStep;
static int     g_ovlHold;
static DWORD   g_ovlStart;
static BOOL    g_ovlActive;
static BOOL    g_ovlCacheValid;
static HRGN    g_ovlRgn;

static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

/* 区域裁剪用的黑名单：这些是系统的"壳层"窗口（覆盖全屏但其实是透明的），
   绝不能拿来挖我们的区域，否则会把整个幕布挖空（实测 Windows.UI.Core.CoreWindow
   就是 2560x1440 的可见窗口，一挖就全没了）。 */
static BOOL IsShellOverlayClass(const wchar_t *cls)
{
    static const wchar_t *bl[] = {
        L"Progman", L"WorkerW", L"SHELLDLL_DefView", L"SysListView32",
        L"Windows.UI.Core.CoreWindow", L"CEF-OSC-WIDGET", L"ShellHandwritingCanvas",
        L"TabletModeCoverWindow", L"ApplicationFrameWindow", L"XamlExplorerHostIslandWindow",
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Composition.DesktopWindowContentBridge",
        L"DesktopBeautifyIconCurtain"
    };
    int i;
    for (i = 0; i < (int)(sizeof(bl)/sizeof(bl[0])); i++)
        if (_wcsicmp(cls, bl[i]) == 0) return TRUE;
    return FALSE;
}

/* 覆盖层区域 = 图标包围盒 − 真正压在桌面上的应用窗口 */
static BOOL CALLBACK OvRegionProc(HWND h, LPARAM l)
{
    wchar_t cls[64];
    RECT r;
    HRGN tmp;
    (void)l;
    if (h == g_ovlWnd or !IsWindowVisible(h) or IsIconic(h)) return TRUE;
    cls[0] = 0; GetClassNameW(h, cls, 64);
    if (IsShellOverlayClass(cls)) return TRUE;          /* 壳层窗口不参与挖除 */
    {
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(h, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    }
    if (!GetWindowRect(h, &r)) return TRUE;
    if (r.right - r.left <= 0 or r.bottom - r.top <= 0) return TRUE;
    tmp = CreateRectRgn(r.left - g_ovlX, r.top - g_ovlY, r.right - g_ovlX, r.bottom - g_ovlY);
    CombineRgn(g_ovlRgn, g_ovlRgn, tmp, RGN_DIFF);
    DeleteObject(tmp);
    return TRUE;
}

static void OverlayDestroy(void)
{
    KillTimer(g_hwnd, IDT_ICONFADE);
    if (g_ovlWnd) { DestroyWindow(g_ovlWnd); g_ovlWnd = NULL; }
    if (g_ovlMemDC) { DeleteDC(g_ovlMemDC); g_ovlMemDC = NULL; }
    if (g_ovlRgn) { DeleteObject(g_ovlRgn); g_ovlRgn = NULL; }
    g_ovlActive = FALSE;
}

/* 抓一次图标素材：两次抓屏相减。
   A = 图标可见时的那块桌面（颜色来源），B = 图标隐藏后同一块（蒙版来源）。
   只有图标像素在两者之间会变 → 差值即精确蒙版；壁纸、窗口、任务栏都会自动抵消。
   这样既不需要 PrintWindow（实测它会把壁纸也印进来），也不需要跨进程枚举图标矩形。 */
static DWORD g_ovlCacheStamp;         /* 素材抓取时刻：超过 ICONCACHE_MAXAGE_MS 就重抓 */



/* 清掉桌面图标的"选中"状态。
   为什么必须清：被选中的图标会被 shell 画一个【半透明高亮框】。这个框在
   PrintWindow 抓图（黑底）里会变成深色像素，被亮度映射当成"图标内容"，
   于是淡入动画里就出现一个突兀的黑方块。
   在"图标本来就看不见"的时刻清掉选中，用户完全察觉不到，
   下一次抓出来的素材就是干净的。
   用 LVM_SETITEMSTATE + 跨进程内存块（标准做法），iItem=-1 表示作用于全部项。 */
static void ClearIconSelection(void)
{
    HWND lv = CachedLV();
    LVITEMW li, *remote;
    DWORD pid = 0;
    HANDLE hp;
    DWORD_PTR res = 0;
    if (!lv or !IsWindow(lv)) return;
    GetWindowThreadProcessId(lv, &pid);
    if (!pid) return;
    hp = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!hp) { Log("clear selection: OpenProcess failed %lu", GetLastError()); return; }
    remote = (LVITEMW *)VirtualAllocEx(hp, NULL, sizeof(LVITEMW), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { CloseHandle(hp); Log("clear selection: VirtualAllocEx failed"); return; }
    ZeroMemory(&li, sizeof(li));
    li.mask = LVIF_STATE;
    li.state = 0;
    li.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    if (WriteProcessMemory(hp, remote, &li, sizeof(li), NULL))
    {
        SendMessageTimeoutW(lv, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)remote,
                            SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, &res);
        Log("icon fade: selection cleared");
    }
    VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
    CloseHandle(hp);
}

/* 抓一次图标素材。必须在图标【可见】时调用。
   用 PrintWindow 印桌面 ListView 自己的表面 —— 那是「图标叠在纯黑上」，
   完全不含壁纸，因此不受 Wallpaper Engine 动画干扰。
   （抓屏相减那条路已实测否决：350ms 内壁纸自身就有 9.7% 的像素变化超过阈值。）

   注意：不要尝试"换背景色抓两张解方程"。实测 PrintWindow 带
   PW_RENDERFULLCONTENT 时用的是 DWM 重定向表面快照，第二次调用会直接返回
   同一张缓存 → c2 == c1 → 解出 alpha=255 → 整屏被不透明地盖住（桌面变黑）。
   这个坑已经踩过，绝不能再走。 */
static BOOL IconCachePassA(void)
{
    HWND lv = CachedLV();
    RECT wr;
    int w, h, x, y, bx0, by0, bx1, by1;
    HDC hs, mem;
    HBITMAP dib;
    HGDIOBJ oldObj = NULL;
    void *bits = NULL;
    BITMAPINFO bi;
    BOOL ok = FALSE;
    DWORD t0 = 0;

    /* 节流：刚抓过就不重复抓；除此之外【每次都重抓】——
       这样用户挪动过图标之后，下一次预览用的就是新位置，
       不会出现"动画里的图标位置和真实位置对不上"。 */
    if (g_ovlCacheValid and (GetTickCount() - g_ovlCacheStamp) < ICONCACHE_MIN_INTERVAL_MS) return TRUE;
    if (!lv or !IsWindow(lv) or !IsWindowVisible(lv)) return FALSE;
    if (!GetWindowRect(lv, &wr)) return FALSE;
    w = wr.right - wr.left; h = wr.bottom - wr.top;
    if (w < 64 or h < 64 or w > 16384 or h > 16384) return FALSE;

    hs = GetDC(NULL);
    mem = CreateCompatibleDC(hs);
    t0 = GetTickCount();          /* 计时：抓一次素材到底花多久，用实测数字说话 */
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dib = CreateDIBSection(hs, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (dib and bits)
    {
        oldObj = SelectObject(mem, dib);
        memset(bits, 0, (size_t)w * h * 4);
        if (PrintWindow(lv, mem, 2))
        {
            GdiFlush();
            bx0 = w; by0 = h; bx1 = -1; by1 = -1;
            {
                int i, total = w * h;
                int *colCnt = (int *)calloc((size_t)w, sizeof(int));
                int *rowCnt = (int *)calloc((size_t)h, sizeof(int));
                long lit = 0;
                long hz[6];      /* 直方图：0 / 1-2 / 3-5 / 6-15 / 16-45 / 46+ */
                ZeroMemory(hz, sizeof(hz));
                for (i = 0; i < total; i++)
                {
                    const BYTE *p = (const BYTE *)bits + (size_t)i * 4;
                    int m = p[0] > p[1] ? p[0] : p[1];
                    int px, py;
                    if (p[2] > m) m = p[2];
                    if (m == 0) hz[0]++;
                    else if (m <= 2) hz[1]++;
                    else if (m <= 5) hz[2]++;
                    else if (m <= 15) hz[3]++;
                    else if (m <= 45) hz[4]++;
                    else hz[5]++;
                    if (m < ICONCACHE_CONTENT_MIN) continue;
                    px = i % w; py = i / w;
                    lit++;
                    if (colCnt) colCnt[px]++;
                    if (rowCnt) rowCnt[py]++;
                    if (px < bx0) bx0 = px;
                    if (px > bx1) bx1 = px;
                    if (py < by0) by0 = py;
                    if (py > by1) by1 = py;
                }
                Log("icon fade: histogram m=0:%ld 1-2:%ld 3-5:%ld 6-15:%ld 16-45:%ld 46+:%ld",
                    hz[0], hz[1], hz[2], hz[3], hz[4], hz[5]);
                /* 密度裁剪：剔掉零散噪点，包围盒收紧到真正成列成行的图标区 */
                if (colCnt and rowCnt and bx1 >= bx0 and by1 >= by0)
                {
                    int colTh = (int)((by1 - by0 + 1) * 0.03) + 1;
                    int rowTh = (int)((bx1 - bx0 + 1) * 0.03) + 1;
                    for (x = 0; x < w; x++) if (colCnt[x] >= colTh) { bx0 = x; break; }
                    for (x = w - 1; x >= 0; x--) if (colCnt[x] >= colTh) { bx1 = x; break; }
                    for (y = 0; y < h; y++) if (rowCnt[y] >= rowTh) { by0 = y; break; }
                    for (y = h - 1; y >= 0; y--) if (rowCnt[y] >= rowTh) { by1 = y; break; }
                }
                if (colCnt) free(colCnt);
                if (rowCnt) free(rowCnt);
                Log("icon fade: printwindow lit=%ld px, bbox=%d,%d - %d,%d", lit, bx0, by0, bx1, by1);
            }
            if (bx1 >= bx0 and by1 >= by0 and (bx1 - bx0 + 1) * (by1 - by0 + 1) > 400)
            {
                int cw = bx1 - bx0 + 1, ch = by1 - by0 + 1;
                void *cbits = NULL;
                BITMAPINFO cbi;
                long opaque = 0;
                ZeroMemory(&cbi, sizeof(cbi));
                cbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                cbi.bmiHeader.biWidth = cw;
                cbi.bmiHeader.biHeight = -ch;
                cbi.bmiHeader.biPlanes = 1;
                cbi.bmiHeader.biBitCount = 32;
                cbi.bmiHeader.biCompression = BI_RGB;
                if (g_ovlBmp) { DeleteObject(g_ovlBmp); g_ovlBmp = NULL; }
                g_ovlBmp = CreateDIBSection(hs, &cbi, DIB_RGB_COLORS, &cbits, NULL, 0);
                if (g_ovlBmp and cbits)
                {
                    for (y = 0; y < ch; y++)
                        for (x = 0; x < cw; x++)
                        {
                            const BYTE *src = (const BYTE *)bits + ((size_t)(by0 + y) * w + (bx0 + x)) * 4;
                            BYTE *dst = (BYTE *)cbits + ((size_t)y * cw + x) * 4;
                            int m = src[0] > src[1] ? src[0] : src[1];
                            int al;
                            if (src[2] > m) m = src[2];
                            /* 实测（全屏直方图）：背景是【精确的 0】（3,549,071 px，96.6%），
                               m=1-5 全屏只有 409 px —— 所以这个软阈值是安全的。
                               亮度 21 以上一律完全不透明：深色图标（微信等）的深色部分
                               若只画到 67%，交接时真图标一出来就会"跳一下"。 */
                            al = (m - 6) * 18;
                            if (al < 0) al = 0;
                            if (al > 255) al = 255;
                            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = (BYTE)al;
                            if (al) opaque++;
                        }
                    if (opaque > (long)cw * ch / 2)
                    {
                        /* 保险丝：超过一半像素"不透明"说明抓图退化了（例如整整一块
                           黑底被算成不透明）。宁可不要淡入，也绝不能把一大块不透明
                           内容盖到桌面上 —— 桌面变黑那次就是这么来的。正常值约 10~25%。 */
                        Log("icon fade: FUSE opaque=%ld/%ld (%.0f%%) too high -> fade disabled",
                            opaque, (long)cw * ch, 100.0 * opaque / ((double)cw * ch));
                        DeleteObject(g_ovlBmp);
                        g_ovlBmp = NULL;
                    }
                    else
                    {
                        g_ovlX = wr.left + bx0;
                        g_ovlY = wr.top + by0;
                        g_ovlW = cw; g_ovlH = ch;
                        g_ovlCacheStamp = GetTickCount();
                        Log("icon fade: cached %dx%d at %d,%d (%ld px, %.1f%%), bitmap %ld KB, took %lu ms",
                            cw, ch, g_ovlX, g_ovlY, opaque, 100.0 * opaque / ((double)cw * ch),
                            (long)((size_t)cw * ch * 4 / 1024), (unsigned long)(GetTickCount() - t0));
                        ok = TRUE;
                    }
                }
            }
            else Log("icon fade: bbox empty, fade disabled");
        }
        else Log("icon fade: PrintWindow failed (%lu)", GetLastError());
        SelectObject(mem, oldObj);
        DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(NULL, hs);
    g_ovlCacheValid = ok;
    return ok;
}

/* B 段：抓屏相减方案已废弃 —— 实测 350ms 内 WE 壁纸自身就有 9.7% 的像素变化超过阈值，
   差值无法区分"图标"和"壁纸动画"。现在改用 PrintWindow（见 IconCachePassA）。 */
static BOOL IconCachePassB(void) { return g_ovlCacheValid; }


static BOOL OverlayCreate(void)
{
    WNDCLASSW wc;
    HINSTANCE hi = GetModuleHandleW(NULL);
    HDC hs;
    if (g_ovlWnd) return TRUE;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"DesktopBeautifyIconCurtain";
    if (!RegisterClassW(&wc) and GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    { Log("icon fade: RegisterClass failed %lu", GetLastError()); return FALSE; }
    g_ovlWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED_,
        L"DesktopBeautifyIconCurtain", L"", WS_POPUP,
        g_ovlX, g_ovlY, g_ovlW, g_ovlH, NULL, NULL, hi, NULL);
    if (!g_ovlWnd) { Log("icon fade: CreateWindowEx failed %lu", GetLastError()); return FALSE; }
    hs = GetDC(NULL);
    g_ovlMemDC = CreateCompatibleDC(hs);
    ReleaseDC(NULL, hs);
    if (!g_ovlMemDC) { OverlayDestroy(); return FALSE; }
    SelectObject(g_ovlMemDC, g_ovlBmp);
    /* 挖掉压在桌面上的可见窗口，只盖真正露出来的桌面部分 */
    g_ovlRgn = CreateRectRgn(0, 0, g_ovlW, g_ovlH);
    if (g_ovlRgn) { EnumWindows(OvRegionProc, 0); SetWindowRgn(g_ovlWnd, g_ovlRgn, TRUE); }
    return TRUE;
}

static BOOL OverlayBlit(int alpha)
{
    HDC hs;
    POINT src, dst;
    SIZE sz;
    BLENDFUNCTION bf;
    BOOL ok;
    if (!g_ovlWnd or !g_ovlMemDC) return FALSE;
    hs = GetDC(NULL);
    src.x = 0; src.y = 0;
    dst.x = g_ovlX; dst.y = g_ovlY;
    sz.cx = g_ovlW; sz.cy = g_ovlH;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = (BYTE)(alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha));
    bf.AlphaFormat = AC_SRC_ALPHA;
    ok = UpdateLayeredWindow(g_ovlWnd, hs, &dst, &sz, g_ovlMemDC, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, hs);
    return ok;
}

/* 淡入开始：先建幕布并 alpha=0 显示（此时屏幕 = 纯壁纸，图标不可见），
   然后逐帧提高 alpha；同时调用方已经把真 ListView 显示出来在背后重绘。 */
static BOOL IconFadeStart(void)
{
    if (!g_ovlCacheValid or !g_ovlBmp) return FALSE;
    if (!OverlayCreate()) return FALSE;
    ShowWindow(g_ovlWnd, SW_SHOWNOACTIVATE);
    if (!OverlayBlit(0))
    {
        Log("icon fade: first blit failed %lu", GetLastError());
        OverlayDestroy();
        return FALSE;
    }
    g_ovlAlpha = 0;
    g_ovlStep = (255 + ICONFADE_FRAMES - 1) / ICONFADE_FRAMES;
    g_ovlStart = GetTickCount();
    g_ovlActive = TRUE;
    SetTimer(g_hwnd, IDT_ICONFADE, ICONFADE_INTERVAL, NULL);
    return TRUE;
}

static void IconFadeTick(void)
{
    if (!g_ovlActive) { KillTimer(g_hwnd, IDT_ICONFADE); return; }
    if (GetTickCount() - g_ovlStart > ICONFADE_MAX_MS)
    {   /* 保险丝 */
        Log("icon fade: TIMEOUT, removing curtain");
        OverlayDestroy();
        return;
    }
    if (g_ovlAlpha < 255)
    {
        g_ovlAlpha += g_ovlStep;
        if (g_ovlAlpha >= 255)
        {
            g_ovlAlpha = 255;
            OverlayBlit(255);
            g_ovlHold = ICONFADE_HOLD_FRAMES;
            Log("icon fade: full in %lu ms, holding", (unsigned long)(GetTickCount() - g_ovlStart));
            return;
        }
        OverlayBlit(g_ovlAlpha);
        return;
    }
    if (g_ovlHold > 0) { g_ovlHold--; return; }
    /* 交接：到这一步才把真 ListView 显示出来。幕布此刻是满不透明，正好
       盖着它慢慢重绘（约 270ms），画完再撤幕布 —— 交接无痕。
       多显示器：全部屏幕一起显示（幕布只盖主屏，其它屏直接出现）。 */
    {
        ShowAllListViews(TRUE);
        Log("icon fade: handover -> showing the real listview behind the curtain");
        KillTimer(g_hwnd, IDT_ICONFADE);
        SetTimer(g_hwnd, IDT_ICONHANDOVER, ICONHANDOVER_MS, NULL);
    }
}

static void StartPeek(void)
{
    HWND lv, t;
    g_peek = TRUE;
    g_peekStart = GetTickCount();

    /* 关键：一次性取好两个句柄（走缓存，不做枚举），然后连续执行两次"显示"，
       中间不做任何耗时工作，保证图标与任务栏同时出现。 */
    lv = CachedLV();
    t = CachedTray();
    g_iconsWereVisible = (lv != NULL) && IsWindowVisible(lv);
    g_trayWasHiddenAtPeek = (t != NULL) && !IsWindowVisible(t);

    /* 图标：立即显示（Explorer 要重绘 81 个图标，约 270ms 才真正画出来）
       任务栏：同时开始【整条上滑】动画（约 240ms）。
       两者并行 —— 等任务栏滑到位时图标刚好画完，自然对齐。
       不再需要"延后 200ms 显示任务栏"那种补偿常数。 */
    if (!g_iconsWereVisible && lv)
    {
        /* 图标此刻是隐藏的 —— 顺手清掉上次遗留的选中状态，用户看不见，
           但能保证后面交接时抓出来的素材里没有选中高亮框。 */
        ClearIconSelection();
        if (IconFadeStart())
        {
            /* 幕布已就位（alpha=0），真 ListView 暂时保持隐藏：
               先让用户看到"图标由浅入深浮现"，淡入满了再交接真图标。
               所有屏幕都先清掉残留分层属性，交接时统一显示。 */
            HWND lvs[8];
            int n = EnumAllListViews(lvs, 8), i;
            for (i = 0; i < n; i++) ClearIconsLayered(lvs[i]);
        }
        else
        {
            /* 没有素材：老路径直接显示（全部屏幕），功能不降级 */
            ForceIconsOpaque();
            Log("icon fade: skipped (no cached icons yet)");
        }
    }
    if (g_trayWasHiddenAtPeek && t)
        TaskbarSlideUp();

    Log("peek start, iconsWereVisible=%d trayWasHidden=%d",
        g_iconsWereVisible, g_trayWasHiddenAtPeek);
}

static void EndPeek(void)
{
    HWND lv, t;
    g_peek = FALSE;
    KillTimer(g_hwnd, IDT_TRAYSHOW);
    g_trayShowPending = FALSE;
    OverlayDestroy();                    /* 提前松手时立刻撤幕布，绝不留在屏幕上 */
    lv = CachedLV();
    t = CachedTray();
    if (!g_iconsWereVisible && lv)
    {
        /* 抓素材前先清掉选中状态：否则被选中图标的高亮框会被抓进素材、
           在淡入动画里变成一个黑方块。此刻图标马上要隐藏，用户看不见这次清除。 */
        ClearIconSelection();
        /* 正常情况素材由"交接时刻"重抓（那里位置最权威且不拖延隐藏）；
           只有素材缺失或太旧时才在这里补抓一次（会让图标多留约 0.1 秒）。 */
        if (!g_ovlCacheValid or (GetTickCount() - g_ovlCacheStamp) > ICONCACHE_MAXAGE_MS)
            IconCachePassA();
        ShowAllListViews(FALSE);          /* 全部屏幕一起隐藏 */
    }
    if (g_trayWasHiddenAtPeek && t) TaskbarSlideDown();
    Log("peek end");
}

/* ---------------- 托盘菜单 ---------------- */
static void ShowTrayMenu(void)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    AppendMenuW(m, MF_STRING, IDM_TOGGLE, IconsVisible() ? L"隐藏图标与任务栏" : L"显示图标与任务栏");
    {
        static const wchar_t *modeName[5] = { L"关闭", L"清晰", L"模糊", L"亚克力", L"含子窗口" };
        wchar_t buf[64];
        _snwprintf(buf, 63, L"任务栏透明：%ls  (点击切换)", modeName[g_transMode % 5]);
        AppendMenuW(m, MF_STRING, IDM_TRANS, buf);
    }
    AppendMenuW(m, MF_STRING | (AutostartOn() ? MF_CHECKED : 0), IDM_AUTO, L"开机自启");
    AppendMenuW(m, MF_STRING, IDM_INSTALL, L"重新注册右键菜单项");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出并恢复图标与任务栏");
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ---------------- 窗口过程 ---------------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_wmTaskbarCreated && msg == g_wmTaskbarCreated)
    {
        Log("TaskbarCreated: explorer restarted, reapplying");
        g_cachedLV = NULL; g_cachedTray = NULL;      /* 句柄缓存失效 */
        g_regionSet = FALSE;
        g_tbRectKnown = FALSE;                       /* 任务栏被重建，位置缓存失效 */
        KillTimer(hwnd, IDT_TRAYANIM);
        g_animActive = FALSE;
        g_listView = ListView();
        CaptureTrayNormalRect(TrayWnd());
        SetTaskbarTransparent(g_transparent);
        SyncMenuText();
        return 0;
    }
    switch (msg)
    {
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowTrayMenu();
        else if (LOWORD(lp) == WM_LBUTTONUP) ApplyHidden(IconsVisible());
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDM_TOGGLE:  ApplyHidden(IconsVisible()); break;
        case IDM_TASKBAR: SetTaskbarHidden(!g_taskbarHidden); break;
        case IDM_TRANS:
            g_transMode = (g_transMode + 1) % 5;
            g_transparent = (g_transMode != 0);
            SaveTransMode();
            SetTaskbarTransparent(TRUE);
            Log("transparency mode -> %s", TransModeName());
            /* 让用户立刻看到效果：显示任务栏 2.5 秒 */
            TaskbarShow();
            SetTimer(hwnd, IDT_TRAYDEMO, 2500, NULL);
            break;
        case IDM_AUTO:    SetAutostartTask(!AutostartOn()); break;
        case IDM_INSTALL: InstallMenu(); break;
        case IDM_EXIT:    DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_TIMER:
        if (wp == IDT_POLL)
        {
            BOOL focused = DesktopFocused();
            int want = focused ? 12 : 200;
            BOOL down;
            static int ticks = 0;
            static DWORD lastWatch = 0;
            DWORD now;
            if (want != g_pollMs) { g_pollMs = want; SetTimer(hwnd, IDT_POLL, want, NULL); }
            down = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
            if (down && !g_peek && focused) StartPeek();
            else if (!down && g_peek) EndPeek();
            else if (g_peek && (GetTickCount() - g_peekStart) > 15000)
            {
                /* 保险丝：万一"抬起"事件丢失（合成按键、焦点切换等），
                   预览会一直停在显示状态。超过 15 秒强制结束，绝不把用户留在那里。 */
                Log("peek TIMEOUT after %lu ms -> forcing end",
                    (unsigned long)(GetTickCount() - g_peekStart));
                EndPeek();
            }
            ticks++;
            if (ticks == 10)
                Log("poll loop alive: tick=10 focused=%d spaceDown=%d iconsVisible=%d",
                    focused, down, IconsVisible());
            /* 看门狗：约每秒一次，与是否聚焦无关 */
            now = GetTickCount();
            if (now - lastWatch >= 1000) { lastWatch = now; TaskbarWatchdog(); }
        }
        else if (wp == IDT_TRAYANIM)
        {
            SlideTick();
        }
        else if (wp == IDT_ICONFADE)
        {
            IconFadeTick();
        }
        else if (wp == IDT_ICONHANDOVER)
        {
            KillTimer(hwnd, IDT_ICONHANDOVER);
            Log("icon fade: handover done, curtain removed");
            OverlayDestroy();
            /* 每次交接都重抓素材：此刻真图标已画好且可见，位置最权威。
               于是用户挪动过图标之后，下一次预览就是新位置，不会再对不上。
               内部有 2 秒节流，连点空格不会反复抓图。 */
            ClearIconSelection();      /* 抓之前清选中：否则选中框会被抓成黑方块 */
            IconCachePassA();
        }
        else if (wp == IDT_ICONCACHE)
        {
            KillTimer(hwnd, IDT_ICONCACHE);
            if (!g_peek) IconCachePassB();     /* 图标已隐藏，差值即精确蒙版 */
        }
        else if (wp == IDT_TRAYSHOW)
        {
            /* 图标的延迟显示：此时图标刚好要画出来，任务栏现在显示即可与它同时呈现 */
            HWND t;
            KillTimer(hwnd, IDT_TRAYSHOW);
            if (g_peek && g_trayShowPending)
            {
                g_trayShowPending = FALSE;
                t = CachedTray();
                if (t && !IsWindowVisible(t))
                {
                    if (g_regionSet) TaskbarRegion(t, -1);
                    ShowWindowAsync(t, SW_SHOW);
                    if (g_accentDirty) SetTaskbarTransparent(g_transparent);
                    Log("tray shown after %d ms delay", PEEK_TRAY_DELAY_MS);
                }
            }
        }
        else if (wp == IDT_TRAYDEMO)
        {
            KillTimer(hwnd, IDT_TRAYDEMO);
            if (TaskbarShouldBeHidden() && !g_peek) TaskbarHide();
        }
        return 0;

    case WM_DESTROY:
        Log("exit: restoring icons and taskbar");
        KillTimer(hwnd, IDT_POLL);
        KillTimer(hwnd, IDT_TRAYANIM);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        /* 关键安全步骤：若退出时正处于预览淡入/淡出中，图标可能停在“可见但全透明”。
           先把不透明度拉满并摘掉分层属性，再恢复显示，确保绝不会留下看不见的图标。 */
        SetTaskbarHidden(FALSE);      /* 清掉裁剪区域并显示任务栏 */
        SetIconsVisible(TRUE);
        g_transparent = FALSE;
        SetTaskbarTransparent(FALSE);
        SyncMenuText();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 高 DPI 感知：让托盘菜单在 2K/混合 DPI 屏幕上清晰不发虚 */
static void EnableDpiAwareness(void)
{
    typedef BOOL (WINAPI *PFN_SPDAC)(HANDLE);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    PFN_SPDAC fn = u ? (PFN_SPDAC)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (fn) { if (fn((HANDLE)(LONG_PTR)-4)) return; }   /* PER_MONITOR_AWARE_V2 */
    SetProcessDPIAware();
}

/* ---------------- 自检：把关键状态写进日志，便于远程排错 ---------------- */
static void DumpState(const char *tag)
{
    HWND d = FindDefView();
    HWND lv = ListView();
    HWND t = TopLevelByClass(L"Shell_TrayWnd");
    HKEY k;
    DWORD hid = 0xFFFFFFFF, sz = sizeof(hid), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS)
    {
        RegQueryValueExW(k, L"HideIcons", NULL, &type, (BYTE *)&hid, &sz);
        RegCloseKey(k);
    }
    Log("[%s] DefView=%p ListView=%p iconsVisible=%d HideIcons=%lu tray=%p trayVisible=%d foregroundDesktop=%d",
        tag, (void *)d, (void *)lv, (lv && IsWindowVisible(lv)), (unsigned long)hid,
        (void *)t, (t && IsWindowVisible(t)), DesktopFocused());
}

static void SelfTest(void)
{
    BOOL startVisible;
    Log("===== SELFTEST START =====");
    if (!CreateMainWindow())
        Log("WARNING: main window could not be created; animation steps will not run");
    DumpState("before");
    startVisible = IconsVisible();

    Log("step1: hide icons ...");
    SetIconsVisible(FALSE);
    PumpSleep(600);
    DumpState("after-hide");

    Log("step2: show icons ...");
    SetIconsVisible(TRUE);
    PumpSleep(600);
    DumpState("after-show");

    Log("step3: restore original icon state (%d) ...", startVisible);
    SetIconsVisible(startVisible);
    PumpSleep(600);
    DumpState("after-restore");

    Log("step3: taskbar hide via ShowWindow ...");
    SetTaskbarHidden(TRUE);
    PumpSleep(600);
    Log("       trayVisible = %d (expect 0)", IsWindowVisible(TrayWnd()));

    Log("step4: taskbar show ...");
    TaskbarShow();
    PumpSleep(600);
    Log("       trayVisible = %d (expect 1)", IsWindowVisible(TrayWnd()));

    Log("step4b: taskbar hide ...");
    TaskbarHide();
    PumpSleep(600);
    Log("       trayVisible = %d (expect 0)", IsWindowVisible(TrayWnd()));

    Log("step4c: restore taskbar ...");
    SetTaskbarHidden(FALSE);
    PumpSleep(400);
    Log("       trayVisible = %d (expect 1)", IsWindowVisible(TrayWnd()));

    Log("step5: transparency api available = %d", GetSetWCA() != NULL);
    SetTaskbarTransparent(TRUE);
    PumpSleep(300);
    Log("step5: transparent applied (visual check needed)");
    SetTaskbarTransparent(FALSE);
    PumpSleep(300);
    Log("step5: transparency restored");

    Log("step6: listView layered state now = 0x%08lX",
        (unsigned long)GetWindowLongPtrW(ListView(), GWL_EXSTYLE_));
    Log("===== SELFTEST END =====");
}

static BOOL CreateMainWindow(void)
{
    WNDCLASSEXW wc;
    if (g_hwnd && IsWindow(g_hwnd)) return TRUE;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"DesktopBeautifyWnd";
    if (!RegisterClassExW(&wc)) { Log("RegisterClassEx failed err=%lu", (unsigned long)GetLastError()); return FALSE; }
    /* 不可见的顶层窗口：用于托盘图标、定时器与接收 TaskbarCreated 广播 */
    g_hwnd = CreateWindowExW(0, L"DesktopBeautifyWnd", L"DesktopBeautify", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, g_hInst, NULL);
    if (!g_hwnd) { Log("CreateWindowEx failed err=%lu", (unsigned long)GetLastError()); return FALSE; }
    return TRUE;
}

/* 等待期间抽空处理消息队列——让定时器驱动的动画在自检里也能真正跑起来 */
static void PumpSleep(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG msg;
    while (GetTickCount() < end)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

static void InitPaths(void)
{
    wchar_t *p;
    GetModuleFileNameW(NULL, g_exePath, MAX_PATH);
    wcsncpy(g_exeDir, g_exePath, MAX_PATH);
    p = wcsrchr(g_exeDir, L'\\');
    if (p) *p = 0;
    _snwprintf(g_logPath, MAX_PATH - 1, L"%s\\DesktopBeautify.log", g_exeDir);
}

static int RunTray(void)
{
    MSG msg;
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"DesktopBeautify_SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        Log("another instance is running, exit");
        return 0;
    }

    /* 启动自检：低完整性（沙盒 / AppContainer）下 UIPI 会拦掉所有 shell 控制，
       与其让用户面对一个“点了没反应”的工具，不如明确告警。
       （编译时定义 NO_INTEGRITY_CHECK 可跳过，仅用于开发期测试启动路径） */
#ifndef NO_INTEGRITY_CHECK
    {
        DWORD rid = GetIntegrityRid();
        Log("integrity RID = 0x%04lX", (unsigned long)rid);
        if (rid != 0 && rid < SECURITY_MANDATORY_MEDIUM_RID)
        {
            Log("ABORT: below medium integrity, UIPI will block shell control");
            {
                wchar_t msg[1200];
                _snwprintf(msg, 1100,
                    L"\u672C\u7A0B\u5E8F\u9700\u8981\u4EE5\u666E\u901A\u7528\u6237\u6743\u9650\u8FD0\u884C\u3002\n\n"
                    L"Detected integrity level: LOW (sandboxed / restricted).\n"
                    L"Windows blocks low-integrity processes from controlling the\n"
                    L"desktop icons and the taskbar, so the program cannot work here.\n\n"
                    L"\u8BF7\u7528\u300C\u6587\u4EF6\u8D44\u6E90\u7BA1\u7406\u5668\u300D\u6253\u5F00\u4E0B\u9762\u7684\u6587\u4EF6\u5939\uFF0C\u53CC\u51FB\u8FD0\u884C\uFF1A\n\n%s",
                    g_exePath);
                MessageBoxW(NULL, msg, L"DesktopBeautify", MB_ICONWARNING | MB_SETFOREGROUND);
            }
            return 2;
        }
    }
#else
    Log("integrity check skipped (NO_INTEGRITY_CHECK)");
#endif

    if (!CreateMainWindow()) return 1;

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcsncpy(g_nid.szTip, L"桌面美化 (按住空格预览)", 127);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid))
        Log("Shell_NotifyIcon(NIM_ADD) FAILED err=%lu", (unsigned long)GetLastError());
    else
        Log("tray icon added");

    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_listView = ListView();
    g_cachedLV = g_listView;          /* 预热句柄缓存：避免"第一次按住空格"还要付枚举开销 */
    g_cachedTray = TrayWnd();
    CaptureTrayNormalRect(g_cachedTray);   /* 记录任务栏正常位置（平移动画的基准） */
    TaskbarNormalize();                    /* 清掉上次异常退出可能残留的区域/错位 */
    LoadTransMode();
    SetTaskbarTransparent(g_transparent);

    /* 首次运行（还没有右键菜单项）：自动完成安装并进入隐藏状态。
       这样即使用户从不以 --install 方式启动（比如由计划任务直接拉起），也能自动装好。 */
    if (!MenuInstalled())
    {
        Log("first run: installing context menu + autostart task, then hiding");
        InstallMenu();
        SetAutostartTask(TRUE);
        SetIconsVisible(FALSE);
    }

    /* 让任务栏与图标状态保持一致 */
    SetTaskbarHidden(!IconsVisible());
    SyncMenuText();
    SetTimer(g_hwnd, IDT_POLL, g_pollMs, NULL);
    Log("tray started: hwnd=%p listView=%p iconsVisible=%d trayHiddenNow=%d",
        g_hwnd, g_listView, IconsVisible(), TaskbarIsHiddenNow());

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) CloseHandle(mutex);
    Log("message loop ended");
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    wchar_t *cmd = NULL;
    int argc = 0;
    wchar_t **argv;
    (void)hPrev; (void)lpCmdLine; (void)nCmdShow;
    g_hInst = hInst;
    InitPaths();
    EnableDpiAwareness();
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) cmd = argv[1];

    /* 日志超过 256KB 就重开，避免无限增长 */
    {
        HANDLE hf = CreateFileW(g_logPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER sz;
            if (GetFileSizeEx(hf, &sz) && sz.QuadPart > (256 * 1024))
            {
                CloseHandle(hf);
                DeleteFileW(g_logPath);
            }
            else CloseHandle(hf);
        }
    }
    {
        char cbuf[128] = "(tray)";
        if (cmd) WideCharToMultiByte(CP_ACP, 0, cmd, -1, cbuf, sizeof(cbuf), NULL, NULL);
        Log("========== RUN %s ==========", cbuf);
    }
    LogIdentityChain();

    if (cmd && _wcsicmp(cmd, L"--selftest") == 0)
    {
        SelfTest();
        return 0;
    }
    if (cmd && _wcsicmp(cmd, L"--peektest") == 0)
    {
        /* 诊断用：直接跑一次真实的 StartPeek/EndPeek 时序，不依赖桌面是否聚焦，
           便于用外部抓帧工具测量"图标与任务栏真正画上屏幕"的时刻差。
           可选参数：--peektest <等待毫秒>，给抓帧工具留出准备时间。 */
        DWORD pre = 1200;
        if (argc > 2 && argv[2] && argv[2][0]) pre = (DWORD)_wtoi(argv[2]);
        if (pre < 200) pre = 200;
        if (pre > 20000) pre = 20000;
        if (!CreateMainWindow()) { Log("peektest: no window"); return 1; }
        LoadTransMode();
        CachedLV(); CachedTray();
        Log("peektest: waiting %lu ms before StartPeek", (unsigned long)pre);
        PumpSleep(pre);
        Log("peektest: StartPeek now");
        StartPeek();
        PumpSleep(2500);
        Log("peektest: EndPeek now");
        EndPeek();
        PumpSleep(400);
        Log("peektest: done");
        return 0;
    }
    if (cmd && (_wcsicmp(cmd, L"--toggle") == 0 || _wcsicmp(cmd, L"--show") == 0 || _wcsicmp(cmd, L"--hide") == 0))
    {
        BOOL hidden, ok;
        /* 切换：图标当前可见 -> 隐藏；当前隐藏 -> 显示 */
        if (_wcsicmp(cmd, L"--toggle") == 0) hidden = IconsVisible();
        else hidden = (_wcsicmp(cmd, L"--hide") == 0);
        ApplyHidden(hidden);
        ok = (IconsVisible() == !hidden);
        Log("cmd %ls -> hidden=%d ok=%d", cmd, hidden, ok);
        return ok ? 0 : 4;
    }
    if (cmd && _wcsicmp(cmd, L"--restore") == 0)
    {
        SetIconsVisible(TRUE);
        SetTaskbarHidden(FALSE);
        SetTaskbarTransparent(FALSE);
        SyncMenuText();
        Log("cmd --restore done");
        return 0;
    }
    if (cmd && _wcsicmp(cmd, L"--install") == 0)
    {
        BOOL ok = InstallMenu();
        SetAutostartTask(TRUE);
        Log("cmd --install done, ok=%d", ok);
        return ok ? 0 : 3;
    }
    if (cmd && _wcsicmp(cmd, L"--uninstall") == 0)
    {
        SetIconsVisible(TRUE);
        SetTaskbarHidden(FALSE);
        SetTaskbarTransparent(FALSE);
        UninstallAll();
        Log("cmd --uninstall done");
        return 0;
    }
    return RunTray();
}
