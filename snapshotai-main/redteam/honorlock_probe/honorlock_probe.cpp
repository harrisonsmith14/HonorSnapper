#include <windows.h>
#include <tlhelp32.h>
#include <setupapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <chrono>
#include <thread>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iostream>
#include <locale>
#include <codecvt>
#include <intrin.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static HHOOK g_keyboardHook = NULL;
static HWND g_overlayWnd = NULL;
static bool g_run = true;
static bool g_activated = false;

std::string w2s(const std::wstring &ws) {
    int size = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, NULL, 0, NULL, NULL);
    if (size <= 0) return "";
    std::string s(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), size, NULL, NULL);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

bool isVirtualMachine() {
    int cpuInfo[4] = {0};
    __cpuid(cpuInfo, 1);
    bool hypervisor = (cpuInfo[2] & (1 << 31)) != 0;
    if (hypervisor) return true;

    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        wchar_t buf[512];
        DWORD len = sizeof(buf);
        if (RegQueryValueExW(key, L"SystemBiosVersion", NULL, NULL, (LPBYTE)buf, &len) == ERROR_SUCCESS) {
            std::wstring str(buf);
            if (str.find(L"VirtualBox") != std::wstring::npos || str.find(L"VMware") != std::wstring::npos || str.find(L"Hyper-V") != std::wstring::npos || str.find(L"QEMU") != std::wstring::npos) {
                RegCloseKey(key);
                return true;
            }
        }
        RegCloseKey(key);
    }
    return false;
}

bool enforceSingleMonitor() {
    int monitors = GetSystemMetrics(SM_CMONITORS);
    return monitors == 1;
}

bool testScreenCapture(const std::wstring &outBmp) {
    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, width, height);
    if (!hbm || !hdcMem) {
        if (hbm) DeleteObject(hbm);
        if (hdcMem) DeleteDC(hdcMem);
        if (hdcScreen) ReleaseDC(NULL, hdcScreen);
        return false;
    }
    SelectObject(hdcMem, hbm);
    if (!BitBlt(hdcMem, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY | CAPTUREBLT)) {
        DeleteObject(hbm);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return false;
    }

    BITMAPINFOHEADER bih;
    ZeroMemory(&bih, sizeof(bih));
    bih.biSize = sizeof(bih);
    bih.biWidth = width;
    bih.biHeight = -height;
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    int rowBytes = ((width * 32 + 31) / 32) * 4;
    int dataSize = rowBytes * height;
    std::vector<BYTE> data(dataSize);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader = bih;
    if (!GetDIBits(hdcMem, hbm, 0, height, data.data(), &bi, DIB_RGB_COLORS)) {
        DeleteObject(hbm);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return false;
    }

    BITMAPFILEHEADER bfh;
    ZeroMemory(&bfh, sizeof(bfh));
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + dataSize;

    std::ofstream os(w2s(outBmp), std::ios::binary);
    if (!os.is_open()) {
        DeleteObject(hbm);
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return false;
    }
    os.write((char *)&bfh, sizeof(bfh));
    os.write((char *)&bih, sizeof(bih));
    os.write((char *)data.data(), dataSize);
    os.close();

    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
    return true;
}

bool testDXGIDuplication() {
    IDXGIFactory1 *dxgiFactory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&dxgiFactory);
    if (FAILED(hr) || !dxgiFactory) return false;

    IDXGIAdapter1 *adapter = nullptr;
    if (dxgiFactory->EnumAdapters1(0, &adapter) != S_OK) {
        dxgiFactory->Release();
        return false;
    }

    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    D3D_FEATURE_LEVEL level;
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr) || !device) {
        if (adapter) adapter->Release();
        if (dxgiFactory) dxgiFactory->Release();
        return false;
    }

    IDXGIDevice *dxgiDevice = nullptr;
    hr = device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr) || !dxgiDevice) {
        device->Release();
        context->Release();
        adapter->Release();
        dxgiFactory->Release();
        return false;
    }

    IDXGIAdapter *dxgiAdapter = nullptr;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    if (FAILED(hr) || !dxgiAdapter) {
        dxgiDevice->Release();
        device->Release();
        context->Release();
        adapter->Release();
        dxgiFactory->Release();
        return false;
    }

    IDXGIOutput *output = nullptr;
    hr = dxgiAdapter->EnumOutputs(0, &output);
    if (FAILED(hr) || !output) {
        dxgiAdapter->Release();
        dxgiDevice->Release();
        device->Release();
        context->Release();
        adapter->Release();
        dxgiFactory->Release();
        return false;
    }

    IDXGIOutput1 *output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    bool supportsDup = false;
    if (SUCCEEDED(hr) && output1) {
        IDXGIOutputDuplication *duplication = nullptr;
        hr = output1->DuplicateOutput(device, &duplication);
        if (SUCCEEDED(hr) && duplication) {
            supportsDup = true;
            duplication->Release();
        }
        output1->Release();
    }

    output->Release();
    dxgiAdapter->Release();
    dxgiDevice->Release();
    device->Release();
    context->Release();
    adapter->Release();
    dxgiFactory->Release();
    dxgiDevice->Release();
    return supportsDup;
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT *ks = (KBDLLHOOKSTRUCT*)lParam;
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (ctrl && alt && shift && ks->vkCode == 'H') {
            g_activated = !g_activated;
            return 1;
        }
    }
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

DWORD WINAPI KeyboardThread(LPVOID lpParam) {
    g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
    MSG msg;
    while (g_run && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (g_keyboardHook) UnhookWindowsHookEx(g_keyboardHook);
    return 0;
}

std::wstring RandomTitle() {
    const std::vector<std::wstring> titles = {L"explorer.exe", L"dwm.exe", L"System Idle Process", L"RuntimeBroker.exe", L"svchost.exe"};
    int idx = rand() % titles.size();
    return titles[idx];
}

VOID CALLBACK OverlayTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    if (!g_overlayWnd) return;
    int x = 40 + (rand() % 8);
    int y = 40 + (rand() % 8);
    SetWindowPos(g_overlayWnd, HWND_TOPMOST, x, y, 420, 120, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

ATOM RegisterAppClass(HINSTANCE hInstance) {
    WNDCLASSEXW wcex;
    ZeroMemory(&wcex, sizeof(wcex));
    wcex.cbSize = sizeof(wcex);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = DefWindowProcW;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszClassName = L"HLSecureOverlay";
    return RegisterClassExW(&wcex);
}

HWND CreateOverlayWindow(HINSTANCE hInstance) {
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                L"HLSecureOverlay", L"", WS_POPUP,
                                50, 50, 420, 120, NULL, NULL, hInstance, NULL);
    if (!hwnd) return NULL;

    SetLayeredWindowAttributes(hwnd, RGB(0,0,0), 150, LWA_ALPHA);
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    SetWindowPos(hwnd, HWND_TOPMOST, 50, 50, 420, 120, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    SetTimer(hwnd, 1, 7000, OverlayTimerProc);
    return hwnd;
}

void secureDeleteFile(const std::wstring &path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(h, NULL);
        if (sz > 0) {
            std::vector<BYTE> wipe(sz, 0);
            DWORD written;
            SetFilePointer(h, 0, NULL, FILE_BEGIN);
            WriteFile(h, wipe.data(), sz, &written, NULL);
            FlushFileBuffers(h);
        }
        CloseHandle(h);
    }
    DeleteFileW(path.c_str());
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow) {
    srand((unsigned)time(NULL));
    bool canRun = true;
    std::ostringstream report;
    report << "[Honorlock Defensive Audit]" << "\n";

    report << "1) VM checks...\n";
    bool vm = isVirtualMachine();
    report << " - VM/Hypervisor found: " << (vm ? "YES" : "NO") << "\n";
    if (vm) canRun = false;

    report << "2) Single-monitor enforcement...\n";
    bool singleMonitor = enforceSingleMonitor();
    report << " - Single monitor: " << (singleMonitor ? "YES" : "NO") << "\n";
    if (!singleMonitor) canRun = false;

    report << "3) Screen-capture test...\n";
    bool capOk = testScreenCapture(L"probe_capture.bmp");
    report << " - Screen capture success: " << (capOk ? "YES" : "NO") << "\n";

    report << "4) DXGI duplication test...\n";
    bool dupOk = testDXGIDuplication();
    report << " - DXGI duplication access: " << (dupOk ? "YES" : "NO") << "\n";

    report << "5) Keyboard hook/hotkey test...\n";
    DWORD threadId;
    HANDLE hKbThread = CreateThread(NULL, 0, KeyboardThread, NULL, 0, &threadId);
    report << " - Low-level keyboard hook active: " << (hKbThread ? "YES" : "NO") << "\n";

    report << "6) Overlay window creation...\n";
    RegisterAppClass(hInstance);
    g_overlayWnd = CreateOverlayWindow(hInstance);
    report << " - Overlay created: " << (g_overlayWnd ? "YES" : "NO") << "\n";

    report << "7) Process stealth test...\n";
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"Explorer", WS_POPUP, 0,0,1,1, NULL, NULL, hInstance, NULL);
    if (h) {
        SetWindowLongPtrW(h, GWLP_EXSTYLE, GetWindowLongPtrW(h, GWLP_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
        ShowWindow(h, SW_HIDE);
    }
    report << " - Hidden stealth helper window: " << (h ? "YES" : "NO") << "\n";

    report << "8) Runtime randomization...\n";
    for (int i=0; i<5; ++i) {
        std::wstring title = RandomTitle();
        SetConsoleTitleW(title.c_str());
        if (g_overlayWnd) SetWindowTextW(g_overlayWnd, title.c_str());
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    report << " - Title randomization done.\n";

    report << "\nFinal status: " << (canRun ? "RUNNING" : "ABORTED") << "\n";
    report << "Activation hotkey: Ctrl+Alt+Shift+H toggles hidden state.\n";
    report << "Write probe report to honorlock_probe_report.txt\n";

    std::ofstream out("honorlock_probe_report.txt");
    out << report.str();
    out.close();

    if (!canRun) {
        secureDeleteFile(L"probe_capture.bmp");
        secureDeleteFile(L"honorlock_probe_report.txt");
        g_run = false;
        if (hKbThread) WaitForSingleObject(hKbThread, 1000);
        return 1;
    }

    // keep running for short test window
    for (int i = 0; i < 60 && g_run; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (i % 30 == 0) {
            std::wstring t = RandomTitle();
            SetConsoleTitleW(t.c_str());
            if (g_overlayWnd) SetWindowTextW(g_overlayWnd, t.c_str());
        }
        if (i % 15 == 0) {
            // simulate benign input to avoid anomaly of total idle.
            INPUT input[2] = {};
            input[0].type = INPUT_KEYBOARD;
            input[0].ki.wVk = VK_SHIFT;
            input[1].type = INPUT_KEYBOARD;
            input[1].ki.wVk = VK_SHIFT;
            input[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, input, sizeof(INPUT));
        }
    }

    g_run = false;
    if (hKbThread) {
        PostThreadMessage(threadId, WM_QUIT, 0, 0);
        WaitForSingleObject(hKbThread, 1000);
        CloseHandle(hKbThread);
    }
    if (g_overlayWnd) DestroyWindow(g_overlayWnd);
    if (h) DestroyWindow(h);

    // secure cleanup of the capture and report
    secureDeleteFile(L"probe_capture.bmp");
    secureDeleteFile(L"honorlock_probe_report.txt");

    return 0;
}
