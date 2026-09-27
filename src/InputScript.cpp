#include "InputScript.h"

#include "MainWindow.h"
#include "OcctViewport.h"
#include "core/Units.h"
#include "ui/MarkingMenu.h"

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <V3d_View.hxx>

#include <QCoreApplication>
#include <QApplication>
#include <QDialog>
#include <QEventLoop>
#include <QWheelEvent>

#include <Aspect_Window.hxx>
#include <Graphic3d_BufferType.hxx>
#include <Image_PixMap.hxx>

#include <cstring>
#include <QFile>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QAbstractButton>
#include <QLineEdit>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QMouseEvent>
#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QWindow>

#include <iostream>

namespace lcad {

bool DumpViewportImage(OcctViewport* theViewport, const QString& thePath)
{
    if (theViewport == nullptr) {
        return false;
    }
    Handle(V3d_View) view = theViewport->View();
    if (view.IsNull()) {
        return false;
    }

    QString viewPath = thePath;
    viewPath.replace(QRegularExpression("\\.(png|jpg|jpeg)$",
                                        QRegularExpression::CaseInsensitiveOption),
                     "-viewport.png");
    if (viewPath == thePath) {
        viewPath = thePath + "-viewport.png";
    }

    view->Redraw();

    // Ask OCCT for the pixels and let it SAY what layout they are in,
    // rather than writing a file and rotating the channels back by hand.
    //
    // The hand-rotation this replaces was correct when it was written and
    // silently stopped being correct: with OCCT writing the file itself,
    // the channel order depended on the build and the driver, so the
    // "fix" eventually became the bug and every screenshot came out one
    // rotation off. Two colours are needed to notice that at all -- the
    // origin axes cannot show it, because {red, green, blue} maps onto
    // itself under a rotation, so they look perfect either way. A gold
    // body turning green is what gives it away.
    Handle(Aspect_Window) window = view->Window();
    if (window.IsNull()) {
        return false;
    }
    Standard_Integer width = 0, height = 0;
    window->Size(width, height);
    if (width <= 0 || height <= 0) {
        return false;
    }

    Image_PixMap pixmap;
    if (!view->ToPixMap(pixmap, width, height, Graphic3d_BT_RGB)) {
        return false;
    }

    QImage::Format qtFormat = QImage::Format_Invalid;
    switch (pixmap.Format()) {
        case Image_Format_RGB:  qtFormat = QImage::Format_RGB888;   break;
        case Image_Format_BGR:  qtFormat = QImage::Format_BGR888;   break;
        case Image_Format_RGBA: qtFormat = QImage::Format_RGBA8888; break;
        case Image_Format_BGRA: qtFormat = QImage::Format_ARGB32;   break;
        default:
            // An unexpected layout is worth failing on: saving it anyway
            // would produce exactly the quietly-wrong colours this whole
            // function exists to avoid.
            std::cerr << "screenshot: unsupported pixel format "
                      << static_cast<int>(pixmap.Format()) << std::endl;
            return false;
    }

    QImage image(static_cast<int>(pixmap.SizeX()), static_cast<int>(pixmap.SizeY()), qtFormat);
    for (int y = 0; y < image.height(); ++y) {
        // Row(y) is always the yth row from the TOP, whichever way the
        // buffer is stored: Image_PixMapData keeps a pointer to the top
        // row and flips the sign of the stride. Compensating for
        // IsTopDown() here as well saves the image upside down.
        std::memcpy(image.scanLine(y), pixmap.Row(static_cast<Standard_Size>(y)),
                    static_cast<std::size_t>(image.bytesPerLine()));
    }

    return image.save(viewPath);
}

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

// Deliver a key the way a keyboard reaches a SHORTCUT, so a script can
// prove a binding fires. `key` cannot: it goes straight to the GL window,
// and Qt only consults its shortcut map for a synthetic press sent to a
// widget. That gap is how F and M stayed dead for a whole session -- each
// was bound to two commands, and Qt fires neither on an ambiguous binding.
//
// Qt also ignores every shortcut while no window of the app is ACTIVE, and
// with nobody at the machine the window manager often never focuses a
// freshly started app. A hotkey sent then does nothing, which looks
// exactly like a dead binding -- so ask for focus, and say loudly when it
// was refused rather than let a broken instrument pass for evidence.
void SendHotkey(MainWindow* theWindow, const QString& theSequence)
{
    const QKeySequence sequence(theSequence.trimmed(), QKeySequence::PortableText);
    if (sequence.isEmpty()) {
        std::cerr << "  script: not a key sequence: " << theSequence.toStdString() << std::endl;
        return;
    }
    if (QApplication::activeWindow() != theWindow) {
        theWindow->raise();
        theWindow->activateWindow();
        for (int i = 0; i < 20 && QApplication::activeWindow() != theWindow; ++i) {
            Settle(50);
        }
        if (QApplication::activeWindow() != theWindow) {
            std::cerr << "  script: WARNING the window never became active, so Qt will ignore "
                      << theSequence.toStdString()
                      << " -- a missing result here says nothing about the binding" << std::endl;
        }
    }
    const QKeyCombination combo = sequence[0];
    const int key = static_cast<int>(combo.key());
    QString text;
    if (combo.keyboardModifiers() == Qt::NoModifier && key >= 0x20 && key < 0x7f) {
        text = QString(QChar(key)).toLower();
    }
    QKeyEvent down(QEvent::KeyPress, key, combo.keyboardModifiers(), text);
    QCoreApplication::sendEvent(theWindow, &down);
    QKeyEvent up(QEvent::KeyRelease, key, combo.keyboardModifiers(), text);
    QCoreApplication::sendEvent(theWindow, &up);
}

// Schedule an action against whatever modal dialog is up in theDelay ms.
//
// The script runs as one straight loop, so a command that opens a modal
// dialog parks the whole script inside the dialog's own event loop and
// nothing further is ever sent -- which is why no dialog-driven command
// has been verifiable here. A timer fires inside that nested loop, so
// arming one BEFORE invoking the command is the way in.
//
// Besides accept and cancel, a timer can look inside the dialog, which is
// the only way a script sees one at all:
//   shot <path>            photograph the dialog (a window grab cannot)
//   type <Label> = <text>  type into the field beside that label, then
//                          finish the edit as Tab would
//   dump                   print every labelled field, tree row and message,
//                          and which buttons are enabled
//   click <Text>           press the dialog's button with that text
//   cell <Row> / <Column> = <text>
//                          edit a tree cell: the row whose cells include
//                          <Row>, the column headed <Column>, committed as
//                          a finished cell editor would
void ArmDialog(int theDelay, const QString& theAction)
{
    const QString verb = theAction.trimmed().section(' ', 0, 0).toLower();
    const QString rest = theAction.trimmed().section(' ', 1).trimmed();
    QTimer::singleShot(theDelay, qApp, [verb, rest]() {
        QWidget* dialog = QApplication::activeModalWidget();
        if (dialog == nullptr) {
            std::cout << "  arm: no dialog was open" << std::endl;
            return;
        }
        if (verb == "click") {
            for (QAbstractButton* button : dialog->findChildren<QAbstractButton*>()) {
                if (button->text().remove('&').trimmed() == rest) {
                    std::cout << "  arm: clicking \"" << rest.toStdString() << "\"" << std::endl;
                    // May open a nested modal dialog; later timers still
                    // fire inside its event loop, which is how they reach it.
                    button->click();
                    return;
                }
            }
            std::cout << "  arm: no button \"" << rest.toStdString() << "\"" << std::endl;
            return;
        }
        if (verb == "cell") {
            const QString rowText = rest.section(" / ", 0, 0).trimmed();
            const QString columnText = rest.section(" / ", 1).section(" = ", 0, 0).trimmed();
            const QString text = rest.section(" = ", 1);
            for (QTreeWidget* tree : dialog->findChildren<QTreeWidget*>()) {
                int column = -1;
                for (int c = 0; c < tree->columnCount(); ++c) {
                    if (tree->headerItem()->text(c) == columnText) {
                        column = c;
                    }
                }
                for (QTreeWidgetItemIterator it(tree); *it != nullptr && column >= 0; ++it) {
                    for (int c = 0; c < tree->columnCount(); ++c) {
                        if ((*it)->text(c) == rowText) {
                            (*it)->setText(column, text);
                            std::cout << "  arm: set " << rowText.toStdString() << " / "
                                      << columnText.toStdString() << " = " << text.toStdString()
                                      << std::endl;
                            return;
                        }
                    }
                }
            }
            std::cout << "  arm: no cell " << rowText.toStdString() << " / "
                      << columnText.toStdString() << std::endl;
            return;
        }
        if (verb == "shot") {
            dialog->grab().save(rest);
            std::cout << "  arm: dialog shot -> " << rest.toStdString() << std::endl;
            return;
        }
        if (verb == "type" || verb == "dump") {
            const QString label = rest.section(" = ", 0, 0).trimmed();
            const QString text = rest.section(" = ", 1);
            for (QLabel* caption : dialog->findChildren<QLabel*>()) {
                auto* field = qobject_cast<QLineEdit*>(caption->buddy());
                if (field == nullptr) {
                    continue;
                }
                const QString name = caption->text().remove('&').trimmed();
                if (verb == "dump") {
                    std::cout << "  arm: [" << name.toStdString() << "] \""
                              << field->text().toStdString() << "\""
                              << (field->toolTip().isEmpty()
                                      ? std::string()
                                      : "  tip: " + field->toolTip().toStdString())
                              << std::endl;
                } else if (name == label) {
                    field->setFocus();
                    field->setText(text);
                    emit field->textEdited(text);
                    emit field->editingFinished();
                    std::cout << "  arm: typed \"" << text.toStdString() << "\" into "
                              << label.toStdString() << std::endl;
                    return;
                }
            }
            if (verb == "dump") {
                for (QLabel* caption : dialog->findChildren<QLabel*>()) {
                    if (caption->buddy() == nullptr && caption->isVisible()
                        && !caption->text().trimmed().isEmpty()) {
                        std::cout << "  arm: text: " << caption->text().simplified().toStdString()
                                  << std::endl;
                    }
                }
                for (QTreeWidget* tree : dialog->findChildren<QTreeWidget*>()) {
                    for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it) {
                        QStringList cells;
                        for (int c = 0; c < tree->columnCount(); ++c) {
                            cells << (*it)->text(c);
                        }
                        std::cout << "  arm: row: " << cells.join(" | ").toStdString() << std::endl;
                    }
                }
                for (QPushButton* button : dialog->findChildren<QPushButton*>()) {
                    std::cout << "  arm: button \"" << button->text().remove('&').toStdString()
                              << "\" " << (button->isEnabled() ? "enabled" : "DISABLED")
                              << std::endl;
                }
            } else {
                std::cout << "  arm: no field labelled " << label.toStdString() << std::endl;
            }
            return;
        }
        if (verb == "cancel" || verb == "reject") {
            if (QDialog* box = qobject_cast<QDialog*>(dialog)) {
                box->reject();
            }
            std::cout << "  arm: cancelled the dialog" << std::endl;
            return;
        }
        if (QDialog* box = qobject_cast<QDialog*>(dialog)) {
            box->accept();
        }
        std::cout << "  arm: accepted the dialog" << std::endl;
    });
}

// Trigger a menu-bar item by what it says, "File > Export...", so the
// actions that are not commands -- Export, Undo, Open -- can be driven
// too. Mnemonic ampersands are ignored.
bool TriggerMenu(MainWindow* theWindow, const QString& thePath)
{
    // "File > Open Recent > part.pcad" walks down through submenus.
    QStringList steps = thePath.split('>');
    for (QString& step : steps) {
        step = step.trimmed();
    }
    QList<QAction*> actions = theWindow->menuBar()->actions();
    for (int depth = 0; depth < steps.size(); ++depth) {
        QAction* found = nullptr;
        for (QAction* action : actions) {
            if (action->text().remove('&').simplified() == steps.at(depth)) {
                found = action;
                break;
            }
        }
        if (found == nullptr) {
            return false;
        }
        if (depth + 1 == steps.size()) {
            found->trigger();
            return true;
        }
        if (found->menu() == nullptr) {
            return false;
        }
        // What a real click on the menu title sends first: a menu that
        // fills itself as it opens (Open Recent) does so here.
        emit found->menu()->aboutToShow();
        actions = found->menu()->actions();
    }
    return false;
}

// Zoom the viewport, so a script can frame what it is about to click.
//
// Without this every target is whatever size the default camera makes it
// -- a 20mm box is about twenty pixels across -- and driving the app
// becomes an exercise in guessing coordinates. QWheelEvent is synthesised
// rather than the zoom called directly so the real wheel handler runs,
// cursor anchoring and all.
void SendWheel(MainWindow* theWindow, const QPointF& thePos, int theNotches)
{
    QWindow* target = ViewportWindow(theWindow);
    if (target == nullptr) {
        return;
    }
    const QPoint angle(0, theNotches * 120);   // 120 units is one detent
    QWheelEvent event(thePos, target->mapToGlobal(thePos.toPoint()), QPoint(), angle,
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(target, &event);
}

// A button word trailing a press, release or drag: "left" (the default),
// "right" or "middle", mixed in any order with the modifier words.
Qt::MouseButton ParseButton(const QStringList& theParts, int theFrom)
{
    for (int i = theFrom; i < theParts.size(); ++i) {
        const QString word = theParts.at(i).toLower();
        if (word == "right") {
            return Qt::RightButton;
        }
        if (word == "middle") {
            return Qt::MiddleButton;
        }
    }
    return Qt::LeftButton;
}

// The gesture trail a right-button flick draws, when one is on screen. It
// is a plain tool window rather than a popup, so `popup` never finds it.
MarkingMenuTrail* VisibleTrail()
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* trail = dynamic_cast<MarkingMenuTrail*>(widget);
        if (trail != nullptr && trail->isVisible()) {
            return trail;
        }
    }
    return nullptr;
}

// Modifier words trailing a click: "ctrl", "shift", "alt", in any order.
Qt::KeyboardModifiers ParseModifiers(const QStringList& theParts, int theFrom)
{
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    for (int i = theFrom; i < theParts.size(); ++i) {
        const QString word = theParts.at(i).toLower();
        if (word == "ctrl") {
            modifiers |= Qt::ControlModifier;
        } else if (word == "shift") {
            modifiers |= Qt::ShiftModifier;
        } else if (word == "alt") {
            modifiers |= Qt::AltModifier;
        }
    }
    return modifiers;
}

double VolumeOf(const TopoDS_Shape& theShape)
{
    if (theShape.IsNull()) {
        return 0.0;
    }
    GProp_GProps props;
    BRepGProp::VolumeProperties(theShape, props);
    return props.Mass();
}

// `design`: what a reopened design has to match, printed so a script can
// compare before and after a save without eyes -- the file, whether it has
// unsaved changes, the timeline, the bodies and the user parameters.
void DumpDesign(MainWindow* theWindow)
{
    Document& document = theWindow->Document();
    // The title as the window manager was handed it, with Qt's [*] already
    // turned into an asterisk or nothing -- windowTitle() still has the [*].
    const QString title = theWindow->windowHandle() != nullptr ? theWindow->windowHandle()->title()
                                                               : theWindow->windowTitle();
    std::cout << "  design: file \"" << theWindow->DesignFile().toStdString() << "\""
              << (theWindow->isWindowModified() ? ", modified" : ", saved") << ", title \""
              << title.toStdString() << "\", units "
              << SymbolOf(DefaultLengthUnit()) << std::endl;
    for (std::size_t i = 0; i < document.FeatureCount(); ++i) {
        const FeaturePtr& feature = document.Features()[i];
        std::cout << "  design: feature " << i << " " << feature->TypeName() << " \""
                  << feature->Name() << "\"" << (feature->IsSuppressed() ? " (suppressed)" : "")
                  << (i >= document.RollbackIndex() ? " (rolled back)" : "");
        for (const Parameter& parameter : feature->EditableParameters()) {
            if (!parameter.expression.empty()) {
                std::cout << " " << parameter.name << "=" << parameter.expression << " ("
                          << parameter.Number() << ")";
            }
        }
        std::cout << (feature->LastError().empty() ? "" : "  ERROR " + feature->LastError())
                  << std::endl;
    }
    for (const BodyPtr& body : document.Bodies()) {
        std::cout << "  design: body \"" << body->Name() << "\" volume " << VolumeOf(body->Shape())
                  << (body->IsVisible() ? "" : " (hidden)") << std::endl;
    }
    for (const UserParameter& row : document.UserParameters().Parameters()) {
        std::cout << "  design: parameter " << row.name << " = " << row.expression << " -> "
                  << row.DisplayText() << std::endl;
    }
    std::cout << "  design: total volume " << VolumeOf(document.Shape()) << std::endl;
}

// The page of the ribbon tab in front: the row of sections inside the tab's
// scroll area. Most of a tab sits scrolled out of sight behind the docks,
// so a window grab shows a third of it at best.
QWidget* CurrentRibbonPage(MainWindow* theWindow)
{
    QTabWidget* ribbon = theWindow->Ribbon();
    auto* scroll = ribbon != nullptr ? qobject_cast<QScrollArea*>(ribbon->currentWidget()) : nullptr;
    return scroll != nullptr ? scroll->widget() : nullptr;
}

// The flyout button on the tab in front whose label (its last line, below
// any text icon) says theLabel.
QToolButton* RibbonFlyout(MainWindow* theWindow, const QString& theLabel)
{
    QWidget* page = CurrentRibbonPage(theWindow);
    if (page == nullptr) {
        return nullptr;
    }
    for (QToolButton* button : page->findChildren<QToolButton*>()) {
        if (button->menu() != nullptr && button->text().section('\n', -1).trimmed() == theLabel) {
            return button;
        }
    }
    return nullptr;
}

void TakeShot(MainWindow* theWindow, const QString& thePath)
{
    theWindow->grab().save(thePath);
    DumpViewportImage(theWindow->Viewport(), thePath);
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

    // Buttons a `press` left down, which `move` reports until `release`,
    // and the modifiers it was pressed with: Shift held through a middle
    // drag is what makes it an orbit rather than a pan.
    Qt::MouseButtons      held = Qt::NoButton;
    Qt::KeyboardModifiers heldModifiers = Qt::NoModifier;

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

        if (verb == "wheel" && parts.size() >= 4) {
            SendWheel(theWindow, QPointF(number(1), number(2)), static_cast<int>(number(3)));
            std::cout << "  wheel " << static_cast<int>(number(3)) << " at " << number(1)
                      << "," << number(2) << std::endl;
            continue;
        }

        if (verb == "arm" && parts.size() >= 3) {
            // Everything after the delay, spaces and all: "type Distance =
            // plate_t * 2" needs them.
            const QString action = line.section(QRegularExpression("\\s+"), 2);
            ArmDialog(static_cast<int>(number(1)), action);
            std::cout << "  arm " << action.toStdString() << " in "
                      << static_cast<int>(number(1)) << "ms" << std::endl;
            continue;
        }

        if (verb == "popup" && parts.size() >= 2) {
            // A popup (the marking menu) is not modal, so the script is not
            // parked inside it and can act on it directly.
            QWidget* popup = QApplication::activePopupWidget();
            const QString what = parts.at(1).toLower();
            if (popup == nullptr) {
                std::cout << "  popup: none is open" << std::endl;
            } else if (what == "shot" && parts.size() >= 3) {
                popup->grab().save(parts.at(2));
                std::cout << "  popup shot -> " << parts.at(2).toStdString() << std::endl;
            } else if (what == "dump") {
                if (auto* ring = qobject_cast<lcad::MarkingMenu*>(popup)) {
                    static const char* const kCompass[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
                    for (int i = 0; i < 8; ++i) {
                        const lcad::MarkingMenu::Item& item = ring->Items()[static_cast<std::size_t>(i)];
                        std::cout << "  popup: " << kCompass[i] << " \"" << item.label.toStdString()
                                  << "\"" << (item.label.isEmpty() ? " (empty)"
                                               : item.enabled ? "" : " (greyed)")
                                  << (i == ring->HighlightedWedge() ? "  <- lit" : "") << std::endl;
                    }
                }
            } else if ((what == "move" || what == "click" || what == "release")
                       && parts.size() >= 4) {
                // Offsets from the ring's centre, where the right-click was.
                QPoint centre = popup->rect().center();
                if (auto* ring = qobject_cast<lcad::MarkingMenu*>(popup)) {
                    centre = ring->Centre();
                }
                const QPointF at(centre.x() + number(2), centre.y() + number(3));
                // A move during a "release" still has the right button down:
                // it stands for the hand travelling to a wedge of a ring a
                // HOLD brought up.
                const Qt::MouseButtons down = what == "release" ? Qt::RightButton : Qt::NoButton;
                QMouseEvent moveEvent(QEvent::MouseMove, at, popup->mapToGlobal(at), Qt::NoButton,
                                      down, Qt::NoModifier);
                QCoreApplication::sendEvent(popup, &moveEvent);
                if (what == "release") {
                    // The release a real display delivers to the RING, which
                    // grabbed the pointer as it opened -- where the script's
                    // own `release` goes to the viewport instead.
                    QMouseEvent release(QEvent::MouseButtonRelease, at, popup->mapToGlobal(at),
                                        Qt::RightButton, Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(popup, &release);
                }
                if (what == "click") {
                    QPointer<QWidget> alive(popup);
                    QMouseEvent press(QEvent::MouseButtonPress, at, popup->mapToGlobal(at),
                                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(popup, &press);
                    if (!alive.isNull()) {
                        QMouseEvent release(QEvent::MouseButtonRelease, at, popup->mapToGlobal(at),
                                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                        QCoreApplication::sendEvent(popup, &release);
                    }
                }
                std::cout << "  popup " << what.toStdString() << " " << number(2) << " " << number(3)
                          << std::endl;
            }
            Settle(120);
            continue;
        }

        if (verb == "ribbon" && parts.size() >= 2) {
            const QString what = parts.at(1).toLower();
            const QString rest = line.section(QRegularExpression("\\s+"), 2).trimmed();
            QWidget* page = CurrentRibbonPage(theWindow);
            if (page == nullptr) {
                std::cout << "  ribbon: no tab is showing" << std::endl;
            } else if (what == "shot" && !rest.isEmpty()) {
                page->grab().save(rest);
                std::cout << "  ribbon shot " << page->width() << "x" << page->height() << " -> "
                          << rest.toStdString() << std::endl;
            } else if (what == "menu") {
                // popup() rather than the button's own showMenu(), which
                // runs the menu modally and would park the script inside it.
                // Opened, it is an ordinary popup: `popup shot` sees it.
                QToolButton* button = RibbonFlyout(theWindow, rest);
                if (button == nullptr) {
                    std::cout << "  ribbon: no flyout \"" << rest.toStdString() << "\"" << std::endl;
                } else {
                    button->menu()->popup(button->mapToGlobal(QPoint(0, button->height())));
                    std::cout << "  ribbon menu " << rest.toStdString() << std::endl;
                }
            } else if (what == "close") {
                if (QWidget* popup = QApplication::activePopupWidget()) {
                    popup->close();
                }
            }
            Settle(200);
            continue;
        }

        if (verb == "trail" && parts.size() >= 2) {
            // The gesture trail is up only while a right button is held and
            // dragged, which is exactly when a script can stop and look.
            MarkingMenuTrail* trail = VisibleTrail();
            const QString what = parts.at(1).toLower();
            if (trail == nullptr) {
                std::cout << "  trail: none is shown" << std::endl;
            } else if (what == "shot" && parts.size() >= 3) {
                trail->grab().save(parts.at(2));
                std::cout << "  trail shot -> " << parts.at(2).toStdString() << std::endl;
            } else {
                static const char* const kCompass[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
                const MarkingMenu::Item* item = trail->LitItem();
                if (trail->LitWedge() < 0) {
                    std::cout << "  trail: points at nothing (inside the threshold)" << std::endl;
                } else {
                    std::cout << "  trail: points " << kCompass[trail->LitWedge()] << " \""
                              << (item != nullptr ? item->label.toStdString() : std::string()) << "\""
                              << (item == nullptr || item->label.isEmpty() ? " (empty)"
                                  : item->enabled ? "" : " (greyed)")
                              << std::endl;
                }
            }
            continue;
        }

        if (verb == "menu" && parts.size() >= 2) {
            const QString path = line.section(QRegularExpression("\\s+"), 1);
            std::cout << "  menu " << path.toStdString() << std::endl;
            if (!TriggerMenu(theWindow, path)) {
                std::cout << "  menu: [NO SUCH ITEM]" << std::endl;
            }
            Settle(120);
            continue;
        }

        if ((verb == "save" || verb == "open") && parts.size() >= 2) {
            // The path is the rest of the line, spaces and all. File > Save
            // As and File > Open go through the desktop's own chooser, which
            // a script cannot type into; these take the same road after it.
            // `open` still asks about unsaved changes -- arm the answer.
            const QString path = line.section(QRegularExpression("\\s+"), 1);
            QString error;
            const bool ok = verb == "save" ? theWindow->SaveDesignFile(path, &error)
                                           : theWindow->OpenDesignFile(path, &error);
            std::cout << "  " << verb.toStdString() << " " << path.toStdString()
                      << (ok ? "" : "  [FAILED] " + error.simplified().toStdString()) << std::endl;
            Settle(200);
            continue;
        }

        if (verb == "design") {
            DumpDesign(theWindow);
            continue;
        }

        if (verb == "run" && parts.size() >= 2) {
            const bool ok = theWindow->RunCommandById(parts.at(1));
            std::cout << "  run " << parts.at(1).toStdString()
                      << (ok ? "" : "  [NO SUCH COMMAND]") << std::endl;
        } else if (verb == "tab" && parts.size() >= 2) {
            theWindow->ActivateTab(parts.at(1));
        } else if (verb == "move" && parts.size() >= 3) {
            // With the button still down after a `press`, so a drag can be
            // stopped halfway and photographed; a move reporting no button
            // held ended every tool's drag on its first step. Modifiers
            // named here are added to the ones the press was made with.
            SendMouse(theWindow, QEvent::MouseMove, QPointF(number(1), number(2)),
                      Qt::NoButton, held, heldModifiers | ParseModifiers(parts, 3));
        } else if ((verb == "click" || verb == "rclick") && parts.size() >= 3) {
            const Qt::MouseButton button = (verb == "rclick") ? Qt::RightButton : Qt::LeftButton;
            const QPointF pos(number(1), number(2));
            // Ctrl and Shift matter: adding to a selection is how every
            // multi-pick interaction in this app works, and without them
            // none of it could be driven from a script at all.
            const Qt::KeyboardModifiers modifiers = ParseModifiers(parts, 3);
            // A real click is always preceded by the cursor arriving, and
            // tools rely on that move for their preview state.
            SendMouse(theWindow, QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton, modifiers);
            SendMouse(theWindow, QEvent::MouseButtonPress, pos, button, button, modifiers);
            SendMouse(theWindow, QEvent::MouseButtonRelease, pos, button, Qt::NoButton,
                      modifiers);
        } else if (verb == "press" && parts.size() >= 3) {
            // "press x y right", "press x y middle shift": the button, then
            // what is held with it, both optional.
            const QPointF pos(number(1), number(2));
            const Qt::MouseButton button = ParseButton(parts, 3);
            heldModifiers = ParseModifiers(parts, 3);
            held |= button;
            SendMouse(theWindow, QEvent::MouseButtonPress, pos, button, held, heldModifiers);
        } else if (verb == "release" && parts.size() >= 3) {
            // Releases the named button, or the one `press` held down.
            const QPointF pos(number(1), number(2));
            Qt::MouseButton button = ParseButton(parts, 3);
            if (!held.testFlag(button) && held != Qt::NoButton) {
                button = held.testFlag(Qt::RightButton)    ? Qt::RightButton
                         : held.testFlag(Qt::MiddleButton) ? Qt::MiddleButton
                                                           : Qt::LeftButton;
            }
            held &= ~Qt::MouseButtons(button);
            SendMouse(theWindow, QEvent::MouseButtonRelease, pos, button, held,
                      heldModifiers | ParseModifiers(parts, 3));
            if (held == Qt::NoButton) {
                heldModifiers = Qt::NoModifier;
            }
        } else if (verb == "drag" && parts.size() >= 5) {
            const QPointF from(number(1), number(2));
            const QPointF to(number(3), number(4));
            // "drag x1 y1 x2 y2 right" drags with the right button -- a
            // marking-menu gesture toward that direction; "middle" pans and
            // "middle shift" orbits, as in Fusion.
            const Qt::MouseButton button = ParseButton(parts, 5);
            const Qt::KeyboardModifiers modifiers = ParseModifiers(parts, 5);
            SendMouse(theWindow, QEvent::MouseMove, from, Qt::NoButton, Qt::NoButton, modifiers);
            SendMouse(theWindow, QEvent::MouseButtonPress, from, button, button, modifiers);
            // Several intermediate moves: a single jump wouldn't exercise
            // rubber-band previews the way a real drag does.
            const int steps = 8;
            for (int i = 1; i <= steps; ++i) {
                const double t = static_cast<double>(i) / steps;
                const QPointF at(from.x() + (to.x() - from.x()) * t,
                                 from.y() + (to.y() - from.y()) * t);
                SendMouse(theWindow, QEvent::MouseMove, at, Qt::NoButton, button, modifiers);
                Settle(16);
            }
            SendMouse(theWindow, QEvent::MouseButtonRelease, to, button, Qt::NoButton, modifiers);
        } else if (verb == "key" && parts.size() >= 2) {
            SendKey(theWindow, parts.at(1));
        } else if (verb == "hotkey" && parts.size() >= 2) {
            SendHotkey(theWindow, parts.at(1));
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
