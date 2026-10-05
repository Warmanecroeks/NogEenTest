#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

enum class ColorState { Unknown, Green, Orange, Red };

struct Settings {
    int reactionMs = 10;
    int radius = 42;
    int searchRadius = 95;
    int greenMinSat = 95;
    int colorMinSat = 105;
    int debounceGreen = 2;
    int debounceDanger = 1;
};

struct Detector {
    Settings s;
    POINT center{560, 335};
    bool calibrated = false;
};

static std::atomic<bool> running(false);
static std::atomic<bool> shuttingDown(false);
static std::atomic<bool> mouseDown(false);
static std::atomic<int> detected((int)ColorState::Unknown);
static std::atomic<int> frameMs(16);
static std::mutex detMutex;
static Detector detector;

static HWND hStatus, hColor, hMouse, hPos, hStart, hStop, hCal, hDelay;

static std::wstring stateName(ColorState s) {
    switch (s) {
        case ColorState::Green: return L"GREEN";
        case ColorState::Orange: return L"ORANGE";
        case ColorState::Red: return L"RED";
        default: return L"UNKNOWN";
    }
}

static void setMouse(bool down) {
    bool old = mouseDown.load();
    if (old == down) return;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(in));
    mouseDown.store(down);
}

static void releaseMouse() {
    setMouse(false);
}

static ColorState classifyPixel(BYTE r, BYTE g, BYTE b, int minSat) {
    int mx = std::max({(int)r,(int)g,(int)b});
    int mn = std::min({(int)r,(int)g,(int)b});
    int d = mx - mn;
    if (mx < 45 || d < minSat) return ColorState::Unknown;

    double h = 0;
    if (d == 0) h = 0;
    else if (mx == r) h = 60.0 * fmod(((double)(g-b)/d), 6.0);
    else if (mx == g) h = 60.0 * (((double)(b-r)/d) + 2.0);
    else h = 60.0 * (((double)(r-g)/d) + 4.0);
    if (h < 0) h += 360.0;

    // The supplied video shows a teal/green center; accept a broad green/teal band.
    if ((h >= 145 && h <= 205) || (h >= 75 && h <= 145)) return ColorState::Green;
    if (h >= 15 && h < 75) return ColorState::Orange;
    if (h < 15 || h >= 335) return ColorState::Red;
    return ColorState::Unknown;
}

static bool isTarget(ColorState s) {
    return s == ColorState::Green || s == ColorState::Orange || s == ColorState::Red;
}

static ColorState sampleScreen(POINT* found) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    POINT c;
    {
        std::lock_guard<std::mutex> lock(detMutex);
        c = detector.center;
        if (!detector.calibrated) {
            c.x = (int)std::lround(sw * 0.4375); // 560 at 1280
            c.y = (int)std::lround(sh * 0.4653); // 335 at 720
        }
    }

    int R = detector.s.searchRadius;
  int left = std::max(0, static_cast<int>(c.x) - R);
int top = std::max(0, static_cast<int>(c.y) - R);
int w = std::min(static_cast<int>(sw) - left, 2 * R + 1);
int h = std::min(static_cast<int>(sh) - top, 2 * R + 1);
    if (w <= 0 || h <= 0) return ColorState::Unknown;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, left, top, SRCCOPY);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<BYTE> px((size_t)w*h*4);
    GetDIBits(mem, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    // Look for the small saturated colored center. Prefer pixels near the expected center.
    long long sx[4] = {}, sy[4] = {}, cnt[4] = {};
    int bestIdx = -1;
    double bestScore = -1;

    for (int y=0; y<h; ++y) {
        for (int x=0; x<w; ++x) {
            int dx = (x + left) - c.x;
            int dy = (y + top) - c.y;
            if (dx*dx + dy*dy > detector.s.radius*detector.s.radius) continue;
            BYTE b=px[(y*w+x)*4], g=px[(y*w+x)*4+1], r=px[(y*w+x)*4+2];
            int mx=std::max({(int)r,(int)g,(int)b}), mn=std::min({(int)r,(int)g,(int)b});
            int sat=mx-mn;
            if (sat < detector.s.colorMinSat || mx < 45) continue;
            ColorState cs=classifyPixel(r,g,b,detector.s.colorMinSat);
            int idx=(int)cs;
            if (!isTarget(cs)) continue;
            sx[idx]+=x+left; sy[idx]+=y+top; cnt[idx]++;
        }
    }

    for (int i=1;i<=3;i++) {
        if (!cnt[i]) continue;
        double dx=(double)sx[i]/cnt[i]-c.x;
        double dy=(double)sy[i]/cnt[i]-c.y;
        double dist=std::sqrt(dx*dx+dy*dy);
        double score=cnt[i] / (1.0 + dist*0.05);
        if (score > bestScore) { bestScore=score; bestIdx=i; }
    }

    if (bestIdx < 0 || cnt[bestIdx] < 3) return ColorState::Unknown;
    found->x=(LONG)(sx[bestIdx]/cnt[bestIdx]);
    found->y=(LONG)(sy[bestIdx]/cnt[bestIdx]);
    return (ColorState)bestIdx;
}

static void detectionLoop() {
    ColorState last = ColorState::Unknown;
    int greenCount=0, dangerCount=0;
    while (!shuttingDown.load()) {
        if (!running.load()) {
            releaseMouse();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        HWND fg=GetForegroundWindow();
        wchar_t title[256]{};
        GetWindowTextW(fg,title,256);
        std::wstring t=title;
        // Avoid sending input into another application if user alt-tabs.
        if (t.find(L"Grand Theft Auto V") == std::wstring::npos &&
            t.find(L"FiveM") == std::wstring::npos) {
            releaseMouse();
            detected.store((int)ColorState::Unknown);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        POINT p{};
        ColorState now=sampleScreen(&p);
        detected.store((int)now);

        if (now == ColorState::Green) { greenCount++; dangerCount=0; }
        else if (now == ColorState::Orange || now == ColorState::Red) { dangerCount++; greenCount=0; }
        else { greenCount=0; dangerCount=0; }

        if (greenCount >= detector.s.debounceGreen) {
            if (!mouseDown.load()) {
                if (detector.s.reactionMs > 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(detector.s.reactionMs));
                setMouse(true);
            }
        } else if (dangerCount >= detector.s.debounceDanger) {
            releaseMouse();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    releaseMouse();
}

static void updateUI() {
    if (!hStatus) return;
    bool on=running.load();
    SetWindowTextW(hStatus, on ? L"ON" : L"OFF");
    ColorState s=(ColorState)detected.load();
    SetWindowTextW(hColor, stateName(s).c_str());
    SetWindowTextW(hMouse, mouseDown.load()?L"DOWN":L"UP");
    POINT p; { std::lock_guard<std::mutex> lock(detMutex); p=detector.center; }
    wchar_t buf[128]; swprintf(buf,128,L"%ld, %ld",p.x,p.y);
    SetWindowTextW(hPos,buf);
}

static void setRun(bool v) {
    running.store(v);
    if (!v) releaseMouse();
    updateUI();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
    case WM_HOTKEY:
        if (wp==1) setRun(!running.load());
        else if (wp==2) setRun(false);
        else if (wp==3) {
            POINT p; GetCursorPos(&p);
            { std::lock_guard<std::mutex> lock(detMutex); detector.center=p; detector.calibrated=true; }
            updateUI();
        }
        return 0;
    case WM_COMMAND:
        if ((HWND)lp==hStart) setRun(true);
        else if ((HWND)lp==hStop) setRun(false);
        else if ((HWND)lp==hCal) {
            POINT p; GetCursorPos(&p);
            { std::lock_guard<std::mutex> lock(detMutex); detector.center=p; detector.calibrated=true; }
            updateUI();
        }
        return 0;
    case WM_TIMER:
        updateUI();
        return 0;
    case WM_CLOSE:
        setRun(false);
        shuttingDown.store(true);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        releaseMouse();
        UnregisterHotKey(hwnd,1); UnregisterHotKey(hwnd,2); UnregisterHotKey(hwnd,3);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}

static HWND label(HWND parent, const wchar_t* text, int x,int y,int w,int h) {
    return CreateWindowW(L"STATIC",text,WS_CHILD|WS_VISIBLE,x,y,w,h,parent,nullptr,GetModuleHandleW(nullptr),nullptr);
}

int WINAPI wWinMain(HINSTANCE hInst,HINSTANCE,LPWSTR,int) {
    WNDCLASSW wc{}; wc.lpfnWndProc=WndProc; wc.hInstance=hInst; wc.lpszClassName=L"MomentumFishing";
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    RegisterClassW(&wc);

    HWND hwnd=CreateWindowW(wc.lpszClassName,L"MomentumRP Fishing Macro",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT,CW_USEDEFAULT,430,300,nullptr,nullptr,hInst,nullptr);
    if(!hwnd) return 1;

    label(hwnd,L"MomentumRP Fishing Macro",20,18,350,28);
    label(hwnd,L"Status:",20,58,90,22); hStatus=label(hwnd,L"OFF",120,58,120,22);
    label(hwnd,L"Detected:",20,84,90,22); hColor=label(hwnd,L"UNKNOWN",120,84,120,22);
    label(hwnd,L"Mouse:",20,110,90,22); hMouse=label(hwnd,L"UP",120,110,120,22);
    label(hwnd,L"Detection point:",20,136,100,22); hPos=label(hwnd,L"",120,136,120,22);

    hStart=CreateWindowW(L"BUTTON",L"Start / F6",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,20,175,115,34,hwnd,nullptr,hInst,nullptr);
    hStop=CreateWindowW(L"BUTTON",L"STOP / F7",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,145,175,115,34,hwnd,nullptr,hInst,nullptr);
    hCal=CreateWindowW(L"BUTTON",L"Calibrate / F8",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,270,175,120,34,hwnd,nullptr,hInst,nullptr);

    label(hwnd,L"F6  Start/Stop    F7  Emergency stop    F8  Set point under mouse",20,220,380,40);

    RegisterHotKey(hwnd,1,MOD_NOREPEAT,VK_F6);
    RegisterHotKey(hwnd,2,MOD_NOREPEAT,VK_F7);
    RegisterHotKey(hwnd,3,MOD_NOREPEAT,VK_F8);
    SetTimer(hwnd,1,100,nullptr);

    ShowWindow(hwnd,SW_SHOW);
    UpdateWindow(hwnd);

    std::thread worker(detectionLoop);

    MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)>0) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    shuttingDown.store(true);
    running.store(false);
    releaseMouse();
    if(worker.joinable()) worker.join();
    return 0;
}
