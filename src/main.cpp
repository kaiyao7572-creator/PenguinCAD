#include "InputScript.h"
#include "MainWindow.h"
#include "OcctViewport.h"
#include "core/Command.h"
#include "core/Registration.h"

#include <QApplication>
#include <QDir>
#include <QKeySequence>
#include <QGuiApplication>
#include <QIcon>
#include <QPalette>
#include <QPixmap>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTimer>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>

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

// The design named on the command line -- "penguincad part.pcad", which is
// also how a file manager hands one over. Flags, and the values the flags
// that take one are given, are not it.
QString DesignArgument(const QStringList& theArguments)
{
    static const QStringList takesValue = {"--script", "--screenshot", "--screenshot-delay",
                                           "--screenshot-tab", "--run-command"};
    for (int i = 1; i < theArguments.size(); ++i) {
        const QString& argument = theArguments.at(i);
        if (takesValue.contains(argument)) {
            ++i;
        } else if (!argument.startsWith("--")) {
            return argument;
        }
    }
    return QString();
}

// Run theAction once the 3D viewer exists. It is created on the window's
// first paint, and a design opened before then would be displayed into
// nothing and never framed.
void WhenViewerIsUp(MainWindow& theWindow, std::function<void()> theAction, int theTriesLeft = 100)
{
    if (!theWindow.Viewport()->View().IsNull() || theTriesLeft <= 0) {
        theAction();
        return;
    }
    QTimer::singleShot(50, &theWindow, [&theWindow, theAction, theTriesLeft]() {
        WhenViewerIsUp(theWindow, theAction, theTriesLeft - 1);
    });
}

} // namespace

// --check-shortcuts: register every command, with no window, and fail if
// any key is bound twice. Qt fires NEITHER action on an ambiguous shortcut,
// so a clash does not pick a winner -- it silently disables both, which is
// how F and M were dead for a session. Asking the real registry rather than
// reading the source is the point: commands built from a table (the
// standard views, the selection filters) never show up to a text scan.
int CheckShortcuts()
{
    lcad::CommandRegistry& registry = lcad::CommandRegistry::Instance();
    lcad::RegisterAllCommands(registry);

    std::map<QString, QStringList> bound;
    int commands = 0;
    int unreadable = 0;
    for (const std::string& group : registry.Groups()) {
        for (lcad::Command* command : registry.InGroup(group)) {
            ++commands;
            const QString shortcut = QString::fromStdString(command->Shortcut());
            if (shortcut.isEmpty()) {
                continue;
            }
            const QKeySequence sequence(shortcut, QKeySequence::PortableText);
            if (sequence.isEmpty()) {
                std::cout << "  FAIL  " << command->Id() << ": Qt cannot read the shortcut \""
                          << shortcut.toStdString() << "\"" << std::endl;
                ++unreadable;
                continue;
            }
            bound[sequence.toString(QKeySequence::PortableText)]
                << QString::fromStdString(command->Id());
        }
    }

    // The window's own menu actions share the same keyboard. The File
    // menu's four are spelled out in MainWindow::buildMenus.
    const std::pair<const char*, QKeySequence> fileKeys[] = {
        {"File > New Design", QKeySequence(Qt::CTRL | Qt::Key_N)},
        {"File > Open", QKeySequence(Qt::CTRL | Qt::Key_O)},
        {"File > Save", QKeySequence(Qt::CTRL | Qt::Key_S)},
        {"File > Save As", QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S)},
    };
    for (const auto& entry : fileKeys) {
        bound[entry.second.toString(QKeySequence::PortableText)] << entry.first;
    }
    const std::pair<const char*, QKeySequence::StandardKey> menuKeys[] = {
        {"File > Quit", QKeySequence::Quit},
        {"Edit > Undo", QKeySequence::Undo},
        {"Edit > Redo", QKeySequence::Redo},
    };
    for (const auto& entry : menuKeys) {
        for (const QKeySequence& sequence : QKeySequence::keyBindings(entry.second)) {
            bound[sequence.toString(QKeySequence::PortableText)] << entry.first;
        }
    }

    std::cout << "  " << commands << " commands, " << bound.size() << " bound keys" << std::endl;
    int clashes = 0;
    for (const auto& entry : bound) {
        if (entry.second.size() > 1) {
            std::cout << "  FAIL  " << entry.first.toStdString() << " is bound to "
                      << entry.second.join(", ").toStdString() << std::endl;
            ++clashes;
        }
    }
    if (clashes == 0 && unreadable == 0) {
        std::cout << "  PASS  no key is bound twice" << std::endl;
        return 0;
    }
    return 1;
}

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

    // Ties the window to its installed .desktop entry, so the dock shows
    // PenguinCAD's name and icon rather than a generic one, and portals
    // (the file chooser inside Flatpak) know which app is asking.
    QGuiApplication::setDesktopFileName("io.github.kaiyao7572_creator.PenguinCAD");
    QGuiApplication::setWindowIcon(QIcon::fromTheme("io.github.kaiyao7572_creator.PenguinCAD"));

    if (app.arguments().contains("--check-shortcuts")) {
        return CheckShortcuts();
    }

    ApplySystemColorScheme(app);

    const QStringList args = app.arguments();

    // A scripted run saves and opens scratch designs; they belong in a
    // throwaway settings file, not in the user's File > Open Recent.
    if (args.contains("--script")) {
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           QDir::temp().filePath("penguincad-script-settings"));
    }

    MainWindow window;
    window.show();

    const QString design = DesignArgument(args);
    if (!design.isEmpty()) {
        WhenViewerIsUp(window, [&window, design]() { window.OpenDesignFile(design); });
    }

    // Developer aid: --screenshot <path> grabs the window (and the 3D view
    // separately, since the viewport is a native child window Qt's grab
    // can't see into) then exits. Lets UI work be checked without a human
    // driving the app, which matters on headless/CI boxes.

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
