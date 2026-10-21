#include "darktheme.h"

#include <QApplication>
#include <QEvent>
#include <QList>
#include <QPalette>
#include <QStyle>
#include <QStyleHints>
#include <QWidget>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace {

QString g_initialStyle;
QPalette g_initialPalette;
bool g_dark = false;
bool g_initialized = false;

#ifdef Q_OS_WIN
constexpr DWORD kDwmwaImmersiveDarkMode = 20; // Windows 10 1903+ / Windows 11
constexpr DWORD kDwmwaImmersiveDarkModeLegacy = 19; // Windows 10 1809

HICON g_hIconBig = nullptr;
HICON g_hIconSmall = nullptr;

// Qt never sends WM_SETICON (it only registers a window class icon), and the
// Windows 11 taskbar shows no icon without WM_SETICON. Load ours from the
// exe resource once and set it on every top-level window.
void loadWindowIcons()
{
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    g_hIconBig = reinterpret_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    g_hIconSmall = reinterpret_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
}

void polishTitleBar(QWidget *widget)
{
    if (!widget || !widget->windowHandle())
        return;
    const HWND hwnd = reinterpret_cast<HWND>(widget->winId());
    const BOOL dark = g_dark ? TRUE : FALSE;
    if (DwmSetWindowAttribute(hwnd, kDwmwaImmersiveDarkMode, &dark, sizeof(dark)) != S_OK)
        DwmSetWindowAttribute(hwnd, kDwmwaImmersiveDarkModeLegacy, &dark, sizeof(dark));
    if (g_hIconBig)
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_hIconBig));
    if (g_hIconSmall)
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_hIconSmall));
}
#else
inline void polishTitleBar(QWidget *) {}
inline void loadWindowIcons() {}
#endif

class TitleBarFilter : public QObject {
public:
    explicit TitleBarFilter(QObject *parent) : QObject(parent) {}

    bool eventFilter(QObject *obj, QEvent *e) override
    {
        const QEvent::Type t = e->type();
        if ((t == QEvent::WinIdChange || t == QEvent::Show)
            && g_initialized) {
            if (auto *w = qobject_cast<QWidget *>(obj); w && w->isWindow())
                polishTitleBar(w);
        }
        return QObject::eventFilter(obj, e);
    }
};

// The canonical dark palette for Fusion (Qt Wiki "dark theme" recipe).
QPalette darkPalette()
{
    QPalette p;
    p.setColor(QPalette::Window, QColor(53, 53, 53));
    p.setColor(QPalette::WindowText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(127, 127, 127));
    p.setColor(QPalette::Base, QColor(42, 42, 42));
    p.setColor(QPalette::AlternateBase, QColor(60, 60, 60));
    p.setColor(QPalette::ToolTipBase, Qt::white);
    p.setColor(QPalette::ToolTipText, QColor(53, 53, 53));
    p.setColor(QPalette::Text, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(127, 127, 127));
    p.setColor(QPalette::Dark, QColor(35, 35, 35));
    p.setColor(QPalette::Shadow, QColor(20, 20, 20));
    p.setColor(QPalette::Button, QColor(53, 53, 53));
    p.setColor(QPalette::ButtonText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(127, 127, 127));
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Link, QColor(42, 130, 218));
    p.setColor(QPalette::Highlight, QColor(42, 130, 218));
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(80, 80, 80));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(127, 127, 127));
    return p;
}

} // namespace

namespace DarkTheme {

bool isDark()
{
    return g_dark;
}

void apply(QApplication &app)
{
    if (!g_initialized) {
        g_initialStyle = app.style()->objectName();
        g_initialPalette = app.palette();
        // Parented to the app: destroyed exactly once, with the application.
        auto *filter = new TitleBarFilter(&app);
        app.installEventFilter(filter);
        loadWindowIcons();
        g_initialized = true;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const bool dark = app.styleHints()
                      && app.styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    // Qt < 6.5 has no color scheme API; assume light and keep the native style.
    const bool dark = false;
#endif
    g_dark = dark;

    if (dark && g_initialStyle == QLatin1String("windowsvista")) {
        // Windows 10: the native style cannot render a dark theme, so switch
        // to the standard platform-agnostic dark combination.
        app.setStyle(QStringLiteral("Fusion"));
        app.setPalette(darkPalette());
    } else {
        // Light, or the platform style handles dark natively (Windows 11,
        // Linux): keep the native style and the initial palette.
        app.setStyle(g_initialStyle);
        app.setPalette(g_initialPalette);
    }

    const QList<QWidget *> tops = app.topLevelWidgets();
    for (QWidget *w : tops)
        polishTitleBar(w);
}

} // namespace DarkTheme
