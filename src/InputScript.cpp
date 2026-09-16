#include "InputScript.h"

#include "MainWindow.h"
#include "OcctViewport.h"

#include <V3d_View.hxx>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QPointF>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QWindow>

#include <iostream>

namespace lcad {

namespace {

// Let the event loop run so the app processes what we just posted and
// repaints. Synthetic input that isn't followed by this looks like it did
// nothing, because nothing has drawn yet.
void Settle(int theMilliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(theMilliseconds, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::AllEvents);
    QCoreApplication::processEvents(QEventLoop::AllEvents, theMilliseconds);
}

QWindow* ViewportWindow(MainWindow* theWindow)
{
    OcctViewport* viewport = theWindow->Viewport();
    return viewport != nullptr ? viewport->NativeWindow() : nullptr;
}

void SendMouse(MainWindow*           theWindow,
               QEvent::Type          theType,
               const QPointF&        thePos,
               Qt::MouseButton       theButton,
               Qt::MouseButtons      theButtons,
               Qt::KeyboardModifiers theModifiers)
{
    QWindow* target = ViewportWindow(theWindow);
    if (target == nullptr) {
        return;
    }
    QMouseEvent event(theType, thePos, target->mapToGlobal(thePos.toPoint()),
                      theButton, theButtons, theModifiers);
    QCoreApplication::sendEvent(target, &event);
}

void SendKey(MainWindow* theWindow, const QString& theName)
{
    QWindow* target = ViewportWindow(theWindow);
    if (target == nullptr) {
        return;
    }

    int key = 0;
    QString text;
    const QString name = theName.trimmed();
    if (name.compare("Escape", Qt::CaseInsensitive) == 0) {
        key = Qt::Key_Escape;
    } else if (name.compare("Return", Qt::CaseInsensitive) == 0
               || name.compare("Enter", Qt::CaseInsensitive) == 0) {
        key = Qt::Key_Return;
    } else if (name.compare("Delete", Qt::CaseInsensitive) == 0) {
        key = Qt::Key_Delete;
    } else if (name.compare("Tab", Qt::CaseInsensitive) == 0) {
        key = Qt::Key_Tab;
    } else if (name.size() == 1) {
        key = name.at(0).toUpper().unicode();
        text = name;
    } else {
        return;
    }

    QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier, text);
    QCoreApplication::sendEvent(target, &down);
    QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier, text);
    QCoreApplication::sendEvent(target, &up);
}

void TakeShot(MainWindow* theWindow, const QString& thePath)
{
    theWindow->grab().save(thePath);

    // The GL viewport never appears in a widget grab, so dump it too --
    // that image is the only way to see what a sketch actually looks like.
    if (OcctViewport* viewport = theWindow->Viewport()) {
        Handle(V3d_View) view = viewport->View();
        if (!view.IsNull()) {
            QString viewPath = thePath;
            viewPath.replace(QRegularExpression("\\.(png|jpg|jpeg)$",
                                                QRegularExpression::CaseInsensitiveOption),
                             "-viewport.png");
            if (viewPath == thePath) {
                viewPath = thePath + "-viewport.png";
            }
            view->Redraw();
            view->Dump(viewPath.toLocal8Bit().constData());
        }
    }
    std::cout << "  shot -> " << thePath.toStdString() << std::endl;
}

} // namespace

void RunInputScript(MainWindow* theWindow, const QString& thePath)
{
    QFile file(thePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::cerr << "script: cannot open " << thePath.toStdString() << std::endl;
        QCoreApplication::exit(2);
        return;
    }

    QTextStream stream(&file);
    Settle(600);   // let the viewer come up before anything is sent

    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        const int hash = line.indexOf('#');
        if (hash >= 0) {
            line = line.left(hash).trimmed();
        }
        if (line.isEmpty()) {
            continue;
        }

        const QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        const QString verb = parts.first().toLower();
        auto number = [&parts](int theIndex) {
            return theIndex < parts.size() ? parts.at(theIndex).toDouble() : 0.0;
        };

        if (verb == "run" && parts.size() >= 2) {
            const bool ok = theWindow->RunCommandById(parts.at(1));
            std::cout << "  run " << parts.at(1).toStdString()
                      << (ok ? "" : "  [NO SUCH COMMAND]") << std::endl;
        } else if (verb == "tab" && parts.size() >= 2) {
            theWindow->ActivateTab(parts.at(1));
        } else if (verb == "move" && parts.size() >= 3) {
            SendMouse(theWindow, QEvent::MouseMove, QPointF(number(1), number(2)),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        } else if ((verb == "click" || verb == "rclick") && parts.size() >= 3) {
            const Qt::MouseButton button = (verb == "rclick") ? Qt::RightButton : Qt::LeftButton;
            const QPointF pos(number(1), number(2));
            // A real click is always preceded by the cursor arriving, and
            // tools rely on that move for their preview state.
            SendMouse(theWindow, QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton,
                      Qt::NoModifier);
            SendMouse(theWindow, QEvent::MouseButtonPress, pos, button, button, Qt::NoModifier);
            SendMouse(theWindow, QEvent::MouseButtonRelease, pos, button, Qt::NoButton,
                      Qt::NoModifier);
        } else if (verb == "press" && parts.size() >= 3) {
            const QPointF pos(number(1), number(2));
            SendMouse(theWindow, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
        } else if (verb == "release" && parts.size() >= 3) {
            const QPointF pos(number(1), number(2));
            SendMouse(theWindow, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton,
                      Qt::NoModifier);
        } else if (verb == "drag" && parts.size() >= 5) {
            const QPointF from(number(1), number(2));
            const QPointF to(number(3), number(4));
            SendMouse(theWindow, QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton,
                      Qt::NoModifier);
            SendMouse(theWindow, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
            // Several intermediate moves: a single jump wouldn't exercise
            // rubber-band previews the way a real drag does.
            const int steps = 8;
            for (int i = 1; i <= steps; ++i) {
                const double t = static_cast<double>(i) / steps;
                const QPointF at(from.x() + (to.x() - from.x()) * t,
                                 from.y() + (to.y() - from.y()) * t);
                SendMouse(theWindow, QEvent::MouseMove, at, Qt::NoButton, Qt::LeftButton,
                          Qt::NoModifier);
                Settle(16);
            }
            SendMouse(theWindow, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton,
                      Qt::NoModifier);
        } else if (verb == "key" && parts.size() >= 2) {
            SendKey(theWindow, parts.at(1));
        } else if (verb == "wait" && parts.size() >= 2) {
            Settle(static_cast<int>(number(1)));
            continue;
        } else if (verb == "shot" && parts.size() >= 2) {
            Settle(150);
            TakeShot(theWindow, parts.at(1));
            continue;
        } else if (verb == "echo") {
            std::cout << "  " << line.mid(5).toStdString() << std::endl;
            continue;
        } else if (verb == "quit") {
            break;
        } else {
            std::cerr << "  script: unknown line: " << line.toStdString() << std::endl;
            continue;
        }

        Settle(120);
    }

    QCoreApplication::quit();
}

} // namespace lcad
