#include "InputScript.h"
#include "MainWindow.h"
#include "OcctViewport.h"

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTimer>

#include <cstdlib>

namespace {

// Qt's default style doesn't repaint itself dark just because the desktop
// asks for a dark scheme, so a GNOME-dark user was getting a glaring white
// app. Fusion does follow the scheme, so switch to it and hand it an
// explicit dark palette -- with a light one on the other branch so this
// stays correct for a light desktop too.
void ApplySystemColorScheme(QApplication& theApp)
{
    theApp.setStyle(QStyleFactory::create("Fusion"));

    Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Unknown) {
        // The portal didn't tell us; ask GNOME directly before giving up.
        QProcess probe;
        probe.start("gsettings", {"get", "org.gnome.desktop.interface", "color-scheme"});
        if (probe.waitForFinished(1000)) {
            const QString value = QString::fromUtf8(probe.readAllStandardOutput());
            scheme = value.contains("dark", Qt::CaseInsensitive) ? Qt::ColorScheme::Dark
                                                                  : Qt::ColorScheme::Light;
        }
    }

    if (scheme != Qt::ColorScheme::Dark) {
        return;   // Fusion's stock palette is already the light one
    }

    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);

    const QColor window(0x35, 0x35, 0x35);
    const QColor base(0x2a, 0x2a, 0x2a);
    const QColor text(0xe6, 0xe6, 0xe6);
    const QColor highlight(0x2a, 0x82, 0xda);
    const QColor disabled(0x7f, 0x7f, 0x7f);

    QPalette palette;
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, window);
    palette.setColor(QPalette::ToolTipBase, window);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, window);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, Qt::red);
    palette.setColor(QPalette::Link, highlight);
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, Qt::black);
    palette.setColor(QPalette::PlaceholderText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    theApp.setPalette(palette);
}

} // namespace

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

    ApplySystemColorScheme(app);

    MainWindow window;
    window.show();

    // Developer aid: --screenshot <path> grabs the window (and the 3D view
    // separately, since the viewport is a native child window Qt's grab
    // can't see into) then exits. Lets UI work be checked without a human
    // driving the app, which matters on headless/CI boxes.
    const QStringList args = app.arguments();

    // --script replays synthetic input so interactive behaviour can be
    // exercised and screenshotted without a human driving the mouse.
    const int scriptIndex = args.indexOf("--script");
    if (scriptIndex >= 0 && scriptIndex + 1 < args.size()) {
        const QString scriptPath = args.at(scriptIndex + 1);
        QTimer::singleShot(0, &window,
                           [&window, scriptPath]() { lcad::RunInputScript(&window, scriptPath); });
        return app.exec();
    }

    const int shotIndex = args.indexOf("--screenshot");
    if (shotIndex >= 0 && shotIndex + 1 < args.size()) {
        const QString path = args.at(shotIndex + 1);
        int delayMs = 1200;
        const int delayIndex = args.indexOf("--screenshot-delay");
        if (delayIndex >= 0 && delayIndex + 1 < args.size()) {
            delayMs = args.at(delayIndex + 1).toInt();
        }

        QString tabToShow;
        const int tabIndex = args.indexOf("--screenshot-tab");
        if (tabIndex >= 0 && tabIndex + 1 < args.size()) {
            tabToShow = args.at(tabIndex + 1);
        }

        QString commandToRun;
        const int cmdIndex = args.indexOf("--run-command");
        if (cmdIndex >= 0 && cmdIndex + 1 < args.size()) {
            commandToRun = args.at(cmdIndex + 1);
        }

        QTimer::singleShot(delayMs, &window, [&window, path, tabToShow, commandToRun]() {
            if (!tabToShow.isEmpty()) {
                window.ActivateTab(tabToShow);
            }
            if (!commandToRun.isEmpty()) {
                window.RunCommandById(commandToRun);
            }
            window.grab().save(path);

            // Same helper as the script runner, so both paths write colour
            // correct images.
            lcad::DumpViewportImage(window.Viewport(), path);
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
