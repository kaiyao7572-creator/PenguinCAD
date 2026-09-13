#include "MainWindow.h"

#include <QApplication>
#include <cstdlib>

int main(int argc, char* argv[])
{
    // OCCT's window integration on Linux (Xw_Window) needs an X11 window
    // handle. Force XWayland via the xcb platform plugin so this works
    // whether the session is X11 or Wayland, unless the user has already
    // set QT_QPA_PLATFORM themselves.
    if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }

    // Route dialogs (file open/save, color pickers, etc.) through the
    // desktop portal so they use the system's native picker and follow
    // the OS light/dark setting, instead of Qt's plain built-in style.
    if (qgetenv("QT_QPA_PLATFORMTHEME").isEmpty()) {
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    }

    QApplication app(argc, argv);

    MainWindow window;
    window.show();

    return app.exec();
}
