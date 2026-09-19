
#include <Windows.h>
#include <Shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "../include/SharedState.h"
#include "Canvas.h"
#include "GameLink.h"
#include "Settings.h"
#include "Theme.h"

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "User32.lib")

using crime_trainer::GameLink;
using crime_trainer::kFeatureSlots;
namespace metric = theme::metric;

namespace {

constexpr UINT_PTR kPollTimer = 1;
constexpr UINT_PTR kFrameTimer = 2;
constexpr UINT kTrayIconId = 0xC71A;
constexpr int kHotkeyToggleWindow = 100;
constexpr int kHotkeyFeatureBase = 200;
constexpr wchar_t kWindowClass[] = L"CrimeTrainerConsole";
constexpr wchar_t kInstanceMutex[] = L"Local\\CrimeTrainer_SingleInstance";
constexpr wchar_t kVersion[] = L"v0.5";

struct FeatureDef {
    int slot; // index into SharedState's feature members
    const wchar_t* title;
    const wchar_t* detail;
    const wchar_t* hotkey;
    UINT vk;
};

struct GroupDef {
    const wchar_t* title;
    int first;
    int count;
};

// Порядок на экране. slot - это индекс в общей памяти, так что список можно
// перетасовать как угодно, не трогая IPC.
constexpr std::array<FeatureDef, kFeatureSlots> kFeatures{{
    { 1, L"Полиция", L"Вызовы, подкрепление, погоня и полицейские тревоги", L"CTRL+F1", VK_F1 },
    { 2, L"Гражданские", L"Кражи, шум, чужая территория и агрессия игнорируются", L"CTRL+F2", VK_F2 },
    { 3, L"Камеры", L"Детект и alarm-события охранных камер", L"CTRL+F3", VK_F3 },
    { 4, L"Датчики и сигнализации", L"Motion/laser сенсоры, сейфы, кейпады, авто", L"CTRL+F4", VK_F4 },
    { 7, L"Бесшумность", L"Шаги, прыжки и инструменты не дают шума вообще", L"CTRL+F5", VK_F5 },
    { 8, L"Бесконечная выносливость", L"Бег и лазание не расходуют стамину", L"CTRL+F6", VK_F6 },
    { 9, L"Без урона", L"Полученный урон откатывается обратно в ноль", L"CTRL+F7", VK_F7 },
    { 10, L"Замедление времени", L"Сутки в 8 раз длиннее, NPC живут обычной жизнью", L"CTRL+F8", VK_F8 },
    { 0, L"Остановка времени", L"Часы стоят, но и распорядок дня NPC встаёт вместе с ними", L"CTRL+F9", VK_F9 },
    { 5, L"Бесконечные патроны", L"Боезапас пополняется по мере расхода", L"CTRL+F10", VK_F10 },
    { 6, L"Прочность предметов", L"Отмычки и инструменты не получают износ", L"CTRL+F11", VK_F11 },
}};

constexpr std::array<GroupDef, 4> kGroups{{
    { L"ОБНАРУЖЕНИЕ", 0, 5 },
    { L"ИГРОК", 5, 2 },
    { L"ВРЕМЯ", 7, 2 },
    { L"СНАРЯЖЕНИЕ", 9, 2 },
}};

struct Layout {
    int width{};
    int height{};
    RectF panel;
    RectF header;
    RectF closeButton;
    RectF status;
    RectF footer;
    RectF allOff;
    RectF quit;
    std::array<RectF, kGroups.size()> groupHeaders{};
    std::array<RectF, kFeatureSlots> rows{};
    std::array<RectF, kFeatureSlots> toggles{};
};

HWND g_window = nullptr;
HANDLE g_instanceMutex = nullptr;

Canvas g_canvas;
Layout g_layout;
std::vector<std::uint8_t> g_windowMask;
float g_scale = 1.0f;

GameLink g_link;
crime_trainer::Settings g_settings;
std::array<bool, kFeatureSlots> g_desired{};

bool g_visible = true;
bool g_hotkeysRegistered = false;
int g_hoverRow = -1;
int g_focusRow = 0;
bool g_keyboardFocus = false;
bool g_hoverClose = false;
bool g_hoverAllOff = false;
bool g_hoverQuit = false;
bool g_dirty = true;
bool g_frameTimerActive = false;
UINT g_pollInterval = 0;

enum class ExitPhase { None, Unloading, Failed };
ExitPhase g_exitPhase = ExitPhase::None;
std::wstring g_banner;

std::array<float, kFeatureSlots> g_knob{};
std::array<float, kFeatureSlots> g_rowGlow{};
float g_pulse = 0.0f;
ULONGLONG g_connectingUntil = 0;
ULONGLONG g_nextPulseFrame = 0;

bool g_trayIconAdded = false;
ULONGLONG g_trayRemoveAt = 0;

HFONT g_fontBrand = nullptr;
HFONT g_fontMicro = nullptr;
HFONT g_fontRow = nullptr;
HFONT g_fontDetail = nullptr;
HFONT g_fontStatus = nullptr;
HFONT g_fontClock = nullptr;
HFONT g_fontButton = nullptr;
HFONT g_fontChip = nullptr;

UINT QueryDpi(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));

    if (getDpiForWindow && window) {
        const UINT dpi = getDpiForWindow(window);
        if (dpi != 0) return dpi;
    }
    HDC screen = GetDC(nullptr);
    const UINT dpi = screen ? static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX)) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    return dpi == 0 ? 96 : dpi;
}

void EnableDpiAwareness() {
    using SetContextFn = BOOL(WINAPI*)(HANDLE);
    auto setContext = reinterpret_cast<SetContextFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    // -4 это DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2. Пишу числом, чтобы не
    // требовать свежий Windows SDK.
    if (setContext && setContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) return;
    SetProcessDPIAware();
}

float S(float logical) {
    return logical * g_scale;
}

// Таймер на 60 Гц крутится, только пока есть что анимировать. Оверлею поверх
// игры незачем просыпаться каждые 16 мс ради того, чтобы выяснить, что ничего
// не изменилось.
void Invalidate() {
    g_dirty = true;
    if (g_window && !g_frameTimerActive) {
        SetTimer(g_window, kFrameTimer, 16, nullptr);
        g_frameTimerActive = true;
    }
}

void StopFrames() {
    if (!g_window || !g_frameTimerActive) return;
    KillTimer(g_window, kFrameTimer);
    g_frameTimerActive = false;
}

HFONT MakeFont(float logicalHeight, int weight, const wchar_t* family = L"Segoe UI") {
    return CreateFontW(-static_cast<int>(S(logicalHeight) + 0.5f), 0, 0, 0, weight, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
}

bool FontExists(const wchar_t* family) {
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    LOGFONTW request{};
    wcsncpy_s(request.lfFaceName, family, _TRUNCATE);
    request.lfCharSet = DEFAULT_CHARSET;
    bool found = false;
    EnumFontFamiliesExW(
        screen, &request,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM param) -> int {
            *reinterpret_cast<bool*>(param) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, screen);
    return found;
}

void DestroyFonts() {
    for (HFONT* font : { &g_fontBrand, &g_fontMicro, &g_fontRow, &g_fontDetail, &g_fontStatus,
                         &g_fontClock, &g_fontButton, &g_fontChip }) {
        if (*font) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
}

void CreateFonts() {
    DestroyFonts();
    // Cascadia есть на свежих Windows, Consolas - запасной вариант.
    static const wchar_t* mono = FontExists(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";

    g_fontBrand = MakeFont(14.0f, FW_BOLD);
    g_fontMicro = MakeFont(10.0f, FW_SEMIBOLD);
    g_fontRow = MakeFont(14.5f, FW_MEDIUM);
    g_fontDetail = MakeFont(11.5f, FW_NORMAL);
    g_fontStatus = MakeFont(14.0f, FW_SEMIBOLD);
    g_fontClock = MakeFont(26.0f, FW_SEMIBOLD, mono);
    g_fontButton = MakeFont(11.5f, FW_SEMIBOLD);
    g_fontChip = MakeFont(10.0f, FW_SEMIBOLD, mono);
}

RECT ToRect(const RectF& rect) {
    return RECT{ static_cast<LONG>(std::lround(rect.x)), static_cast<LONG>(std::lround(rect.y)),
                 static_cast<LONG>(std::lround(rect.Right())),
                 static_cast<LONG>(std::lround(rect.Bottom())) };
}

int ExtraFor(float letterSpacing) {
    return static_cast<int>(std::lround(S(letterSpacing)));
}

// DT_CALCRECT не учитывает SetTextCharacterExtra, который GDI потом добавляет
// при отрисовке. Из-за этого разреженный текст меряется короче, чем рисуется,
// и вылезает за свой прямоугольник. Поэтому считаю ширину сам.
float MeasureText(const wchar_t* text, HFONT font, float letterSpacing = 0.0f) {
    if (!text || !*text) return 0.0f;
    HDC dc = g_canvas.Dc();
    if (!dc) return 0.0f;

    const auto length = static_cast<int>(wcslen(text));
    HGDIOBJ previous = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text, length, &size);
    SelectObject(dc, previous);
    return static_cast<float>(size.cx) + static_cast<float>(ExtraFor(letterSpacing) * length);
}

void Label(const wchar_t* text, const RectF& bounds, HFONT font, Color color, UINT format,
           float letterSpacing = 0.0f) {
    if (!text || !*text) return;
    HDC dc = g_canvas.Dc();
    if (!dc) return;

    RectF box = bounds;
    UINT effective = format;

    // Выравнивание вправо и по центру делаю сам: GDI выровнял бы по своей
    // заниженной ширине, и хвост уехал бы за край.
    if (letterSpacing != 0.0f && (format & (DT_RIGHT | DT_CENTER)) != 0) {
        const float width = MeasureText(text, font, letterSpacing);
        if (format & DT_RIGHT) {
            box.x = bounds.Right() - width;
        } else {
            box.x = bounds.x + (bounds.w - width) * 0.5f;
        }
        box.w = width;
        effective = (format & ~(DT_RIGHT | DT_CENTER)) | DT_LEFT;
    }

    HGDIOBJ previous = SelectObject(dc, font);
    SetTextColor(dc, ToColorRef(color));
    SetTextCharacterExtra(dc, ExtraFor(letterSpacing));
    RECT rect = ToRect(box);
    DrawTextW(dc, text, -1, &rect, effective | DT_NOPREFIX);
    SetTextCharacterExtra(dc, 0);
    SelectObject(dc, previous);
}

constexpr UINT kLeftLine = DT_LEFT | DT_SINGLELINE | DT_VCENTER;
constexpr UINT kRightLine = DT_RIGHT | DT_SINGLELINE | DT_VCENTER;
constexpr UINT kCentreLine = DT_CENTER | DT_SINGLELINE | DT_VCENTER;

Layout BuildLayout() {
    Layout layout{};
    const float shadow = S(metric::kShadow);
    const float panelWidth = S(metric::kPanelWidth);
    const float pad = S(metric::kPad);

    float y = 0.0f;
    layout.header = RectF{ 0.0f, 0.0f, panelWidth, S(metric::kHeaderHeight) };
    y += layout.header.h;

    const float closeSize = S(28.0f);
    layout.closeButton = RectF{ panelWidth - pad - closeSize,
                                (layout.header.h - closeSize) * 0.5f, closeSize, closeSize };

    y += S(10.0f);
    layout.status = RectF{ pad, y, panelWidth - pad * 2.0f, S(metric::kStatusHeight) };
    y += layout.status.h + S(14.0f);

    const float rowWidth = panelWidth - pad * 2.0f;
    for (std::size_t g = 0; g < kGroups.size(); ++g) {
        layout.groupHeaders[g] = RectF{ pad, y, rowWidth, S(metric::kGroupHeader) };
        y += layout.groupHeaders[g].h;

        for (int i = 0; i < kGroups[g].count; ++i) {
            const int index = kGroups[g].first + i;
            layout.rows[static_cast<std::size_t>(index)] =
                RectF{ pad, y, rowWidth, S(metric::kRowHeight) };

            const float trackW = S(metric::kTrackWidth);
            const float trackH = S(metric::kTrackHeight);
            layout.toggles[static_cast<std::size_t>(index)] =
                RectF{ pad + rowWidth - trackW, y + (S(metric::kRowHeight) - trackH) * 0.5f,
                       trackW, trackH };
            y += S(metric::kRowHeight);
        }
        if (g + 1 < kGroups.size()) y += S(metric::kGroupGap);
    }

    y += S(10.0f);
    layout.footer = RectF{ 0.0f, y, panelWidth, S(metric::kFooterHeight) };
    y += layout.footer.h;

    const float buttonH = S(30.0f);
    const float buttonY = layout.footer.y + (layout.footer.h - buttonH) * 0.5f;
    const float quitW = S(104.0f);
    const float allOffW = S(96.0f);
    layout.quit = RectF{ panelWidth - pad - quitW, buttonY, quitW, buttonH };
    layout.allOff = RectF{ layout.quit.x - S(8.0f) - allOffW, buttonY, allOffW, buttonH };

    layout.panel = RectF{ shadow, shadow, panelWidth, y };
    layout.width = static_cast<int>(std::ceil(panelWidth + shadow * 2.0f));
    layout.height = static_cast<int>(std::ceil(y + shadow * 2.0f));

    // Всё выше считалось в координатах панели. Сдвигаю один раз тут, чтобы
    // отрисовка и попадания мышью не помнили про отступ под тень.
    const auto shift = [&](RectF& rect) {
        rect.x += shadow;
        rect.y += shadow;
    };
    shift(layout.header);
    shift(layout.closeButton);
    shift(layout.status);
    shift(layout.footer);
    shift(layout.allOff);
    shift(layout.quit);
    for (auto& rect : layout.groupHeaders) shift(rect);
    for (auto& rect : layout.rows) shift(rect);
    for (auto& rect : layout.toggles) shift(rect);
    return layout;
}

float ComputeScale(HWND window) {
    const float dpiScale = static_cast<float>(QueryDpi(window)) / 96.0f;

    // Окно высокое и фиксированное. На ноуте с высоким DPI честный масштаб
    // сделал бы его выше экрана, поэтому ограничиваю рабочей областью.
    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) return dpiScale;

    const float available = static_cast<float>(work.bottom - work.top) - 48.0f;
    float logicalHeight = metric::kHeaderHeight + 10.0f + metric::kStatusHeight + 14.0f +
                          10.0f + metric::kFooterHeight + metric::kShadow * 2.0f;
    for (std::size_t g = 0; g < kGroups.size(); ++g) {
        logicalHeight += metric::kGroupHeader + metric::kRowHeight * kGroups[g].count;
        if (g + 1 < kGroups.size()) logicalHeight += metric::kGroupGap;
    }

    const float maxScale = available / logicalHeight;
    return std::clamp(std::min(dpiScale, maxScale), 0.75f, 3.0f);
}

void Rebuild(HWND window) {
    g_scale = ComputeScale(window);
    CreateFonts();
    g_layout = BuildLayout();
    g_canvas.Resize(g_layout.width, g_layout.height);
    g_windowMask = BuildWindowMask(g_layout.width, g_layout.height, g_layout.panel,
                                   S(metric::kPanelRadius), S(18.0f), S(6.0f), 0.55f);
    SetWindowPos(window, nullptr, 0, 0, g_layout.width, g_layout.height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Invalidate();
}

void RemoveTrayIcon() {
    if (!g_trayIconAdded) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    g_trayIconAdded = false;
}

void Notify(const wchar_t* message) {
    RemoveTrayIcon();

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_INFO;
    data.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    wcscpy_s(data.szTip, L"Crime Trainer");
    wcscpy_s(data.szInfoTitle, L"Crime Trainer");
    wcsncpy_s(data.szInfo, message, _TRUNCATE);
    data.dwInfoFlags = NIIF_INFO;

    if (Shell_NotifyIconW(NIM_ADD, &data)) {
        g_trayIconAdded = true;
        g_trayRemoveAt = GetTickCount64() + 6000;
    }
}

int SlotOf(int row) {
    return kFeatures[static_cast<std::size_t>(row)].slot;
}

bool Desired(int row) {
    return g_desired[static_cast<std::size_t>(SlotOf(row))];
}

LONG SupportOf(int row) {
    return g_link.FeatureSupport(SlotOf(row));
}

bool Applied(int row) {
    return g_link.FeatureApplied(SlotOf(row));
}

void PushDesiredToHook() {
    if (!g_link.Connected() || g_exitPhase != ExitPhase::None) return;
    for (int slot = 0; slot < kFeatureSlots; ++slot) {
        g_link.SetFeatureRequested(slot, g_desired[static_cast<std::size_t>(slot)]);
    }
}

void PersistFeatures() {
    g_settings.features = g_desired;
    crime_trainer::SaveSettings(g_settings);
}

void ToggleRow(int row) {
    if (row < 0 || row >= kFeatureSlots || g_exitPhase != ExitPhase::None) return;
    const std::size_t slot = static_cast<std::size_t>(SlotOf(row));
    g_desired[slot] = !g_desired[slot];
    PushDesiredToHook();
    PersistFeatures();
    Invalidate();
}

void DisarmAll() {
    if (g_exitPhase != ExitPhase::None) return;
    g_desired.fill(false);
    PushDesiredToHook();
    PersistFeatures();
    Invalidate();
}

void DrawPill(const RectF& rect, Color fill, Color border, float radius) {
    g_canvas.FillRoundRect(rect, radius, fill);
    if (border != fill) g_canvas.StrokeRoundRect(rect, radius, S(1.0f), border);
}

void DrawHeader() {
    const RectF& header = g_layout.header;
    const float pad = S(metric::kPad);

    // Вместо иконки просто янтарная полоска - не зависит от шрифтов в системе.
    const float markW = S(3.0f);
    const float markH = S(16.0f);
    g_canvas.FillRoundRect(RectF{ header.x + pad, header.y + (header.h - markH) * 0.5f, markW, markH },
                           markW * 0.5f, theme::kAccent);

    const float textX = header.x + pad + markW + S(11.0f);
    const wchar_t* brand = L"CRIME TRAINER";
    Label(brand, RectF{ textX, header.y, header.w, header.h }, g_fontBrand, theme::kText, kLeftLine,
          1.6f);

    const float brandWidth = MeasureText(brand, g_fontBrand, 1.6f);
    const float pillW = MeasureText(kVersion, g_fontMicro, 0.5f) + S(16.0f);
    const float pillH = S(18.0f);
    const RectF pill{ textX + brandWidth + S(10.0f), header.y + (header.h - pillH) * 0.5f, pillW,
                      pillH };
    g_canvas.FillRoundRect(pill, pillH * 0.5f, theme::kAccentWash);
    Label(kVersion, pill, g_fontMicro, theme::kAccent, kCentreLine, 0.5f);

    // Крестик рисую двумя линиями, а не символом: так он одинаковый на любом
    // масштабе и не зависит от того, как шрифт рисует знак умножения.
    const RectF& close = g_layout.closeButton;
    if (g_hoverClose) g_canvas.FillRoundRect(close, S(8.0f), theme::kRowHover);
    const Color glyph = g_hoverClose ? theme::kText : theme::kTextDim;
    const float cx = close.x + close.w * 0.5f;
    const float cy = close.y + close.h * 0.5f;
    const float arm = S(4.5f);
    g_canvas.StrokeLine(cx - arm, cy - arm, cx + arm, cy + arm, S(1.4f), glyph);
    g_canvas.StrokeLine(cx - arm, cy + arm, cx + arm, cy - arm, S(1.4f), glyph);

    g_canvas.FillRect(RectF{ header.x, header.Bottom() - S(1.0f), header.w, S(1.0f) },
                      theme::kLineSoft);
}

void DrawStatusCard() {
    const RectF& card = g_layout.status;
    g_canvas.FillRoundRect(card, S(12.0f), theme::kRaise);
    g_canvas.StrokeRoundRect(card, S(12.0f), S(1.0f), theme::kLine);

    const bool connected = g_link.Connected();
    const bool ready = g_link.HookReady();

    const Color dotColor = ready ? theme::kLive : (connected ? theme::kWarn : theme::kAlert);
    const float dotX = card.x + S(24.0f);
    const float dotY = card.y + S(28.0f);

    // Пока подключается - ореол вокруг точки, видно без чтения подписи.
    if (connected && !ready) {
        const float wave = 0.5f + 0.5f * std::sin(g_pulse * 3.2f);
        g_canvas.FillCircle(dotX, dotY, S(6.0f) + S(5.0f) * wave, dotColor, 0.18f * (1.0f - wave));
    } else if (ready) {
        g_canvas.FillCircle(dotX, dotY, S(9.0f), dotColor, 0.14f);
    }
    g_canvas.FillCircle(dotX, dotY, S(4.5f), dotColor);

    const wchar_t* title = ready      ? L"ПОДКЛЮЧЕНО"
                           : connected ? L"ЗАГРУЗКА HOOK"
                                       : L"ОЖИДАНИЕ ИГРЫ";
    const float textX = card.x + S(42.0f);
    const float textW = card.w - S(42.0f) - S(150.0f);

    Label(title, RectF{ textX, card.y + S(17.0f), textW, S(20.0f) }, g_fontStatus, theme::kText,
          kLeftLine, 1.2f);

    std::wstring process = crime_trainer::kGameProcessName;
    if (connected) process += L"   ·   PID " + std::to_wstring(g_link.Pid());
    Label(process.c_str(), RectF{ textX, card.y + S(38.0f), textW, S(18.0f) }, g_fontDetail,
          theme::kTextDim, kLeftLine | DT_END_ELLIPSIS);

    std::wstring detail = g_link.Status();
    if (!connected) detail = L"Запусти Crime Simulator — трейнер подключится сам";
    Label(detail.c_str(), RectF{ textX, card.y + S(58.0f), textW, S(18.0f) }, g_fontDetail,
          theme::kTextFaint, kLeftLine | DT_END_ELLIPSIS);

    // Справа - игровые часы и сколько хуков нашлось в этой сборке.
    const float rightEdge = card.Right() - S(22.0f);
    const RectF microRect{ rightEdge - S(150.0f), card.y + S(14.0f), S(150.0f), S(13.0f) };
    Label(L"ВРЕМЯ В ИГРЕ", microRect, g_fontMicro, theme::kTextFaint, kRightLine, 1.4f);

    wchar_t clock[16] = L"··:··";
    const int hour = g_link.Hour();
    const int minute = g_link.Minute();
    const bool clockLive = hour >= 0 && minute >= 0;
    if (clockLive) swprintf_s(clock, L"%02d:%02d", hour, minute);
    Label(clock, RectF{ rightEdge - S(150.0f), card.y + S(29.0f), S(150.0f), S(32.0f) }, g_fontClock,
          clockLive ? theme::kText : theme::kTextFaint, kRightLine);

    const long patches = g_link.ResolvedPatches();
    std::wstring hooks = ready ? (L"HOOKS " + std::to_wstring(patches)) : L"HOOKS --";
    Label(hooks.c_str(), RectF{ rightEdge - S(150.0f), card.y + S(63.0f), S(150.0f), S(14.0f) },
          g_fontChip, theme::kTextFaint, kRightLine);
}

void DrawToggle(const RectF& track, float knob, bool pending) {
    const Color trackColor = MixColor(theme::kTrack, theme::kAccent, knob);
    g_canvas.FillRoundRect(track, track.h * 0.5f, trackColor);

    // Включено, но ещё не применилось - только обводка. Третий цвет путал бы.
    if (pending) {
        g_canvas.StrokeRoundRect(track.Inset(S(1.5f)), track.h * 0.5f, S(1.4f), theme::kWarn, 0.9f);
    }

    const float diameter = S(metric::kKnobDiameter);
    const float inset = (track.h - diameter) * 0.5f;
    const float travel = track.w - diameter - inset * 2.0f;
    const float cx = track.x + inset + diameter * 0.5f + travel * knob;
    g_canvas.FillCircle(cx, track.y + track.h * 0.5f, diameter * 0.5f, theme::kKnob);
}

void DrawRow(int index) {
    const auto slot = static_cast<std::size_t>(index);
    const RectF& row = g_layout.rows[slot];
    const FeatureDef& feature = kFeatures[slot];
    const float glow = g_rowGlow[slot];
    const bool armed = Desired(index);
    const LONG support = SupportOf(index);
    const bool unsupported = support == crime_trainer::kSupportUnsupported;
    const bool pending = armed && g_link.HookReady() && !Applied(index) && !unsupported;

    const RectF band{ row.x - S(10.0f), row.y + S(2.0f), row.w + S(20.0f), row.h - S(4.0f) };
    if (glow > 0.01f) g_canvas.FillRoundRect(band, S(10.0f), theme::kRowHover, glow);
    if (g_keyboardFocus && g_focusRow == index) {
        g_canvas.StrokeRoundRect(band, S(10.0f), S(1.0f), theme::kAccentDeep, 0.9f);
    }

    // У включённых строк слева вырастает полоска - заметно боковым зрением.
    const float railAlpha = g_knob[slot];
    if (railAlpha > 0.01f) {
        const float railH = S(20.0f) * (0.6f + 0.4f * railAlpha);
        g_canvas.FillRoundRect(
            RectF{ band.x, row.y + (row.h - railH) * 0.5f, S(3.0f), railH }, S(1.5f), theme::kAccent,
            railAlpha);
    }

    const float titleX = row.x + S(6.0f);
    const Color titleColor = unsupported ? theme::kTextFaint
                                         : (armed ? theme::kText : theme::kTextDim);
    Label(feature.title, RectF{ titleX, row.y + S(6.0f), row.w * 0.62f, S(18.0f) }, g_fontRow,
          titleColor, kLeftLine);

    const float titleWidth = MeasureText(feature.title, g_fontRow);
    float chipX = titleX + titleWidth + S(10.0f);

    const auto chip = [&](const wchar_t* text, Color color) {
        const float width = MeasureText(text, g_fontMicro, 0.8f) + S(14.0f);
        const float height = S(15.0f);
        const RectF rect{ chipX, row.y + S(7.0f), width, height };
        g_canvas.FillRoundRect(rect, height * 0.5f, color, 0.14f);
        Label(text, rect, g_fontMicro, color, kCentreLine, 0.8f);
        chipX += width + S(6.0f);
    };

    if (unsupported) {
        chip(L"НЕТ В СБОРКЕ", theme::kAlert);
    } else if (pending && support == crime_trainer::kSupportWaiting) {
        chip(L"ЖДЁТ МИР", theme::kWarn);
    }

    Label(feature.detail, RectF{ titleX, row.y + S(24.0f), row.w - S(70.0f), S(15.0f) },
          g_fontDetail, theme::kTextFaint, kLeftLine | DT_END_ELLIPSIS);

    // Подсказка по горячей клавише только под курсором, чтобы не шуметь.
    if (glow > 0.05f) {
        Label(feature.hotkey,
              RectF{ g_layout.toggles[slot].x - S(90.0f), row.y, S(80.0f), row.h }, g_fontChip,
              theme::kTextFaint, kRightLine);
    }

    DrawToggle(g_layout.toggles[slot], g_knob[slot], pending);
}

void DrawGroups() {
    for (std::size_t g = 0; g < kGroups.size(); ++g) {
        const RectF& header = g_layout.groupHeaders[g];
        constexpr UINT kGroupLine = DT_SINGLELINE | DT_BOTTOM;
        Label(kGroups[g].title, RectF{ header.x + S(6.0f), header.y, header.w, header.h - S(7.0f) },
              g_fontMicro, theme::kTextFaint, DT_LEFT | kGroupLine, 1.8f);

        int armed = 0;
        for (int i = 0; i < kGroups[g].count; ++i) {
            if (Desired(kGroups[g].first + i)) ++armed;
        }
        const std::wstring count =
            std::to_wstring(armed) + L"/" + std::to_wstring(kGroups[g].count);
        Label(count.c_str(), RectF{ header.x, header.y, header.w, header.h - S(7.0f) }, g_fontChip,
              armed > 0 ? theme::kAccent : theme::kTextFaint, DT_RIGHT | kGroupLine);
    }

    // Разделители рисую до подложек строк: тогда строка под курсором как бы
    // приподнимается, а не перечёркивается линией.
    for (std::size_t g = 0; g < kGroups.size(); ++g) {
        for (int i = 1; i < kGroups[g].count; ++i) {
            const RectF& row = g_layout.rows[static_cast<std::size_t>(kGroups[g].first + i)];
            g_canvas.FillRect(RectF{ row.x + S(6.0f), row.y, row.w - S(12.0f), S(1.0f) },
                              theme::kLineSoft);
        }
    }

    for (int i = 0; i < kFeatureSlots; ++i) DrawRow(i);
}

void DrawButton(const RectF& rect, const wchar_t* text, bool hovered, Color textColor,
                Color hoverFill) {
    g_canvas.FillRoundRect(rect, S(8.0f), hovered ? hoverFill : theme::kRaise);
    g_canvas.StrokeRoundRect(rect, S(8.0f), S(1.0f), hovered ? textColor : theme::kLine,
                             hovered ? 0.55f : 1.0f);
    Label(text, rect, g_fontButton, hovered ? textColor : theme::kTextDim, kCentreLine, 0.8f);
}

void DrawFooter() {
    const RectF& footer = g_layout.footer;
    g_canvas.FillRect(RectF{ footer.x, footer.y, footer.w, S(1.0f) }, theme::kLineSoft);

    const float pad = S(metric::kPad);

    if (g_exitPhase == ExitPhase::Unloading) {
        Label(L"Восстановление состояний и выгрузка hook...",
              RectF{ footer.x + pad, footer.y, footer.w - pad * 2.0f, footer.h }, g_fontButton,
              theme::kTextDim, kLeftLine);

        const RectF bar{ footer.x + pad, footer.Bottom() - S(14.0f), footer.w - pad * 2.0f,
                         S(2.0f) };
        g_canvas.FillRoundRect(bar, S(1.0f), theme::kLine);
        const float phase = std::fmod(g_pulse * 0.55f, 1.0f);
        const float width = bar.w * 0.28f;
        g_canvas.FillRoundRect(RectF{ bar.x + (bar.w + width) * phase - width, bar.y, width, bar.h },
                               S(1.0f), theme::kAccent);
        return;
    }

    if (!g_banner.empty()) {
        Label(g_banner.c_str(), RectF{ footer.x + pad, footer.y, g_layout.allOff.x - footer.x - pad * 2.0f,
                                       footer.h },
              g_fontButton, theme::kAlert, kLeftLine | DT_END_ELLIPSIS);
    } else {
        const wchar_t* hint = g_hotkeysRegistered
                                  ? L"INS — скрыть окно     CTRL+F1…F11 — функции"
                                  : L"Часть горячих клавиш занята другой программой";
        Label(hint, RectF{ footer.x + pad, footer.y, g_layout.allOff.x - footer.x - pad * 2.0f,
                           footer.h },
              g_fontChip, g_hotkeysRegistered ? theme::kTextFaint : theme::kWarn,
              kLeftLine | DT_END_ELLIPSIS);
    }

    DrawButton(g_layout.allOff, L"ВЫКЛ. ВСЁ", g_hoverAllOff, theme::kText, theme::kRowHover);
    DrawButton(g_layout.quit, L"ЗАВЕРШИТЬ", g_hoverQuit, theme::kAlert,
               MakeColor(0x2A, 0x16, 0x1A));
}

void Present() {
    if (!g_canvas.Dc()) return;

    g_canvas.Clear(theme::kVoid);
    // Еле заметный градиент сверху, иначе большая плоская панель выглядит мёртвой.
    g_canvas.FillRoundRectGradient(g_layout.panel, S(metric::kPanelRadius), theme::kPanelTop,
                                   theme::kPanel, S(150.0f));

    DrawHeader();
    DrawStatusCard();
    DrawGroups();
    DrawFooter();

    g_canvas.StrokeRoundRect(g_layout.panel, S(metric::kPanelRadius), S(1.0f), theme::kLine, 0.7f);
    g_canvas.ApplyAlphaMask(g_windowMask);

    HDC screen = GetDC(nullptr);
    SIZE size{ g_canvas.Width(), g_canvas.Height() };
    POINT source{ 0, 0 };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_window, screen, nullptr, &size, g_canvas.Dc(), &source, 0, &blend,
                        ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    g_dirty = false;
}

// Плавное приближение к цели. Порог обязателен: без него значение подходит к
// цели бесконечно, и таймер кадров не выключится никогда.
bool Ease(float& value, float target) {
    constexpr float kRate = 0.26f;
    constexpr float kSettle = 0.004f;

    if (std::fabs(target - value) < kSettle) {
        value = target;
        return false;
    }
    value += (target - value) * kRate;
    return true;
}

struct Animation {
    bool repaint{}; // something changed that is worth presenting this frame
    bool active{};  // keep the frame pump running
};

Animation Advance() {
    Animation anim;

    for (int i = 0; i < kFeatureSlots; ++i) {
        const auto slot = static_cast<std::size_t>(i);
        if (Ease(g_knob[slot], Desired(i) ? 1.0f : 0.0f)) anim.repaint = true;
        if (Ease(g_rowGlow[slot], g_hoverRow == i ? 1.0f : 0.0f)) anim.repaint = true;
    }
    anim.active = anim.repaint;

    // Пока подключаемся - точка пульсирует. Но если подключение зависло, не
    // анимируем вечно: через полминуты точка замирает, причина есть в статусе.
    const bool connecting = g_link.Connected() && !g_link.HookReady() &&
                            GetTickCount64() < g_connectingUntil;
    if (!connecting && g_exitPhase != ExitPhase::Unloading) return anim;

    anim.active = true;

    // Один кадр - это перерисовка и перемаскирование всего окна. Ради медленно
    // пульсирующей точки делать это 60 раз в секунду жирно, хватит и 20.
    constexpr ULONGLONG kPulseIntervalMs = 50;
    const ULONGLONG now = GetTickCount64();
    if (now >= g_nextPulseFrame) {
        g_pulse += static_cast<float>(kPulseIntervalMs) / 1000.0f;
        g_nextPulseFrame = now + kPulseIntervalMs;
        anim.repaint = true;
    }
    return anim;
}

void RegisterHotkeys() {
    if (!g_settings.globalHotkeys || g_hotkeysRegistered) return;

    bool ok = RegisterHotKey(g_window, kHotkeyToggleWindow, 0, VK_INSERT) != FALSE;
    for (int i = 0; i < kFeatureSlots; ++i) {
        // F-клавиши с Ctrl: работают, пока фокус в игре, и почти наверняка не
        // пересекаются с её собственными биндами.
        ok = RegisterHotKey(g_window, kHotkeyFeatureBase + i, MOD_CONTROL,
                            kFeatures[static_cast<std::size_t>(i)].vk) != FALSE && ok;
    }
    g_hotkeysRegistered = ok;
}

void UnregisterHotkeys() {
    UnregisterHotKey(g_window, kHotkeyToggleWindow);
    for (int i = 0; i < kFeatureSlots; ++i) UnregisterHotKey(g_window, kHotkeyFeatureBase + i);
    g_hotkeysRegistered = false;
}

void SetVisible(bool visible) {
    g_visible = visible;
    if (visible) {
        ShowWindow(g_window, SW_SHOWNOACTIVATE);
        SetWindowPos(g_window, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        Invalidate();
    } else {
        ShowWindow(g_window, SW_HIDE);
    }
}

void RememberPosition() {
    RECT rect{};
    if (!GetWindowRect(g_window, &rect)) return;
    g_settings.windowX = rect.left;
    g_settings.windowY = rect.top;
    crime_trainer::SaveSettings(g_settings);
}

void BeginExit() {
    if (g_exitPhase == ExitPhase::Unloading) return;

    RememberPosition();
    if (!g_link.Connected()) {
        DestroyWindow(g_window);
        return;
    }

    g_banner.clear();
    g_exitPhase = ExitPhase::Unloading;
    g_link.BeginUnload();
    if (!g_link.UnloadInProgress()) {
        // Выгружать нечего - hook в процесс так и не попал.
        g_link.Disconnect();
        DestroyWindow(g_window);
        return;
    }
    Invalidate();
}

void PollExit() {
    if (g_exitPhase != ExitPhase::Unloading) return;

    switch (g_link.PollUnload()) {
    case GameLink::Unload::Pending:
        return;
    case GameLink::Unload::Done:
        g_link.Disconnect();
        DestroyWindow(g_window);
        return;
    case GameLink::Unload::Failed:
        g_exitPhase = ExitPhase::Failed;
        g_banner = L"Hook не выгрузился: главный поток игры занят. Попробуй ещё раз.";
        Invalidate();
        return;
    }
}

void UpdateHover(POINT point) {
    const auto px = static_cast<float>(point.x);
    const auto py = static_cast<float>(point.y);

    int row = -1;
    for (int i = 0; i < kFeatureSlots; ++i) {
        if (g_layout.rows[static_cast<std::size_t>(i)].Contains(px, py)) {
            row = i;
            break;
        }
    }

    const bool close = g_layout.closeButton.Contains(px, py);
    const bool allOff = g_layout.allOff.Contains(px, py);
    const bool quit = g_layout.quit.Contains(px, py);

    if (row != g_hoverRow || close != g_hoverClose || allOff != g_hoverAllOff ||
        quit != g_hoverQuit) {
        g_hoverRow = row;
        g_hoverClose = close;
        g_hoverAllOff = allOff;
        g_hoverQuit = quit;
        Invalidate();
    }
}

bool AnyHotZone() {
    return g_hoverRow >= 0 || g_hoverClose || g_hoverAllOff || g_hoverQuit;
}

void MoveFocus(int delta) {
    g_keyboardFocus = true;
    g_focusRow = (g_focusRow + delta + kFeatureSlots) % kFeatureSlots;
    Invalidate();
}

// Всё, что опрос может изменить на экране. Сравниваю с прошлым срезом, иначе
// простаивающий трейнер перерисовывал бы окно два раза в секунду просто так.
struct PollSnapshot {
    bool connected{};
    bool ready{};
    int hour = -1;
    int minute = -1;
    long patches{};
    std::wstring status;
    std::array<LONG, kFeatureSlots> support{};
    std::array<bool, kFeatureSlots> applied{};

    bool operator==(const PollSnapshot& other) const = default;
};

PollSnapshot g_lastPoll;

PollSnapshot CapturePollSnapshot() {
    PollSnapshot snapshot;
    snapshot.connected = g_link.Connected();
    snapshot.ready = g_link.HookReady();
    snapshot.hour = g_link.Hour();
    snapshot.minute = g_link.Minute();
    snapshot.patches = g_link.ResolvedPatches();
    snapshot.status = g_link.Status();
    for (int i = 0; i < kFeatureSlots; ++i) {
        snapshot.support[static_cast<std::size_t>(i)] = g_link.FeatureSupport(i);
        snapshot.applied[static_cast<std::size_t>(i)] = g_link.FeatureApplied(i);
    }
    return snapshot;
}

void OnPoll() {
    std::wstring announcement;
    const bool changed = g_link.Poll(announcement);
    if (changed && !announcement.empty()) Notify(announcement.c_str());

    // У свежего подключения блок запросов пустой - возвращаем то, что было включено.
    PushDesiredToHook();
    PollExit();

    if (g_trayIconAdded && GetTickCount64() >= g_trayRemoveAt) RemoveTrayIcon();

    PollSnapshot snapshot = CapturePollSnapshot();
    if (snapshot.connected != g_lastPoll.connected || snapshot.ready != g_lastPoll.ready) {
        g_connectingUntil = GetTickCount64() + 30000;
    }
    if (changed || snapshot != g_lastPoll) {
        g_lastPoll = std::move(snapshot);
        Invalidate();
    }

    // Пока игра не найдена, тут только снимок списка процессов - самое дорогое,
    // что вообще делает простаивающий трейнер. После подключения он не нужен,
    // и частый тик остаётся только ради часов.
    const UINT wanted = g_link.Connected() ? 400u : 2000u;
    if (wanted != g_pollInterval && g_window) {
        g_pollInterval = wanted;
        SetTimer(g_window, kPollTimer, wanted, nullptr);
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_pollInterval = 2000;
        SetTimer(hwnd, kPollTimer, g_pollInterval, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == kPollTimer) OnPoll();
        if (wParam == kFrameTimer) {
            // Идущая анимация - тоже повод перерисовать.
            const Animation anim = Advance();
            if ((anim.repaint || g_dirty) && g_visible) Present();
            if (!anim.active && !g_dirty) StopFrames();
        }
        return 0;

    case WM_DPICHANGED: {
        Rebuild(hwnd);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, g_layout.width,
                         g_layout.height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        Present();
        return 0;
    }

    case WM_HOTKEY:
        if (wParam == kHotkeyToggleWindow) {
            SetVisible(!g_visible);
        } else if (wParam >= kHotkeyFeatureBase && wParam < kHotkeyFeatureBase + kFeatureSlots) {
            ToggleRow(static_cast<int>(wParam) - kHotkeyFeatureBase);
        }
        return 0;

    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tracking{};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = hwnd;
        TrackMouseEvent(&tracking);
        UpdateHover(POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;
    }

    case WM_MOUSELEAVE:
        g_hoverRow = -1;
        g_hoverClose = false;
        g_hoverAllOff = false;
        g_hoverQuit = false;
        Invalidate();
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, AnyHotZone() ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const auto px = static_cast<float>(point.x);
        const auto py = static_cast<float>(point.y);

        if (g_layout.closeButton.Contains(px, py) || g_layout.quit.Contains(px, py)) {
            BeginExit();
            return 0;
        }
        if (g_layout.allOff.Contains(px, py)) {
            DisarmAll();
            return 0;
        }
        for (int i = 0; i < kFeatureSlots; ++i) {
            if (g_layout.rows[static_cast<std::size_t>(i)].Contains(px, py)) {
                g_keyboardFocus = false;
                ToggleRow(i);
                return 0;
            }
        }
        if (g_layout.header.Contains(px, py)) {
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        return 0;
    }

    case WM_EXITSIZEMOVE:
        RememberPosition();
        return 0;

    case WM_KEYDOWN:
        switch (wParam) {
        case VK_ESCAPE:
            SetVisible(false);
            return 0;
        case VK_UP:
            MoveFocus(-1);
            return 0;
        case VK_DOWN:
            MoveFocus(1);
            return 0;
        case VK_SPACE:
        case VK_RETURN:
            if (g_keyboardFocus) ToggleRow(g_focusRow);
            return 0;
        default:
            // 1..9 плюс 0 на десятую строку
            if (wParam >= '1' && wParam <= '9') {
                ToggleRow(static_cast<int>(wParam) - '1');
                return 0;
            }
            if (wParam == '0') {
                ToggleRow(kFeatureSlots - 1);
                return 0;
            }
            break;
        }
        break;

    case WM_CLOSE:
        BeginExit();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kPollTimer);
        StopFrames();
        UnregisterHotkeys();
        RemoveTrayIcon();
        g_link.Disconnect();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void PlaceWindow(HWND window) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    int x = 0;
    int y = 0;
    if (g_settings.HasSavedPosition()) {
        x = g_settings.windowX;
        y = g_settings.windowY;
    } else {
        x = work.left + ((work.right - work.left) - g_layout.width) / 2;
        y = work.top + ((work.bottom - work.top) - g_layout.height) / 2;
    }

    // Сохранённая позиция могла быть с монитора, которого уже нет. Возвращаем
    // окно в рабочую область.
    x = std::clamp(x, static_cast<int>(work.left) - g_layout.width / 2,
                   static_cast<int>(work.right) - g_layout.width / 2);
    y = std::clamp(y, static_cast<int>(work.top), static_cast<int>(work.bottom) - g_layout.height);

    SetWindowPos(window, HWND_TOPMOST, x, y, g_layout.width, g_layout.height, SWP_NOACTIVATE);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    EnableDpiAwareness();

    // Два лаунчера на одну игру подрались бы за блок запросов и могли бы
    // заинжектить дважды. Просто показываем уже запущенный.
    g_instanceMutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = nullptr;
        EnumWindows(
            [](HWND candidate, LPARAM param) -> BOOL {
                wchar_t name[64]{};
                GetClassNameW(candidate, name, _countof(name));
                if (wcscmp(name, kWindowClass) != 0) return TRUE;
                *reinterpret_cast<HWND*>(param) = candidate;
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&existing));

        if (existing) {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        CloseHandle(g_instanceMutex);
        return 0;
    }

    g_settings = crime_trainer::LoadSettings();
    g_desired = g_settings.features;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return 1;

    g_window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, kWindowClass,
                               L"Crime Trainer", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr,
                               instance, nullptr);
    if (!g_window) {
        DestroyFonts();
        return 1;
    }

    Rebuild(g_window);
    PlaceWindow(g_window);
    RegisterHotkeys();

    for (int i = 0; i < kFeatureSlots; ++i) {
        g_knob[static_cast<std::size_t>(i)] = Desired(i) ? 1.0f : 0.0f;
    }

    ShowWindow(g_window, SW_SHOWNOACTIVATE);
    Present();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyFonts();
    if (g_instanceMutex) CloseHandle(g_instanceMutex);
    return static_cast<int>(msg.wParam);
}
