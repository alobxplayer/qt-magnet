#pragma once

class QApplication;

// Follows the system light/dark color scheme with native appearance:
//  - Light: the platform's native style and palette (unchanged).
//  - Dark on Windows 10: the native "windowsvista" style cannot render dark,
//    so the standard platform-agnostic combination (Fusion + dark palette)
//    is used; on Windows 11 the native "windows11" style handles dark itself.
//  - On Windows the title bar is switched to the immersive dark variant via
//    DWM so it matches the scheme on every top-level window.
namespace DarkTheme {

void apply(QApplication &app);

// True when the current color scheme is dark (set by the last apply()).
bool isDark();

} // namespace DarkTheme
