#pragma once

#include <QWidget>
#include <QWindow>

#include "core/ViewportInteraction.h"

#include <AIS_InteractiveContext.hxx>
#include <AIS_SelectionScheme.hxx>
#include <AIS_ViewController.hxx>
#include <Graphic3d_Vec2.hxx>
#include <NCollection_Sequence.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include <functional>
#include <vector>

// The actual native window OCCT renders into. QWindow (unlike a plain
// QWidget) always represents a real native platform window and reliably
// delivers expose events, which is what OCCT's viewer needs to know when
// it's safe to create its GL context and draw.
//
// Mouse handling is delegated to OCCT's own AIS_ViewController, which
// gives us Fusion-360-style navigation for free: left-click/drag to
// select (with a rubber-band box for multi-select), right-drag to orbit,
// middle-drag to pan, scroll to zoom.
//
// Left-drag box selection is direction-sensitive like Fusion: dragging
// left-to-right draws a "window" box (only fully-enclosed objects get
// selected), dragging right-to-left draws a "crossing" box (anything the
// box merely touches gets selected too). Ctrl+left-click/drag adds to
// the current selection instead of replacing it.
class OcctNativeWindow : public QWindow, protected AIS_ViewController
{
    Q_OBJECT

public:
    explicit OcctNativeWindow(QWindow* parent = nullptr);

    Handle(AIS_InteractiveContext) Context() const { return m_context; }
    Handle(V3d_View) View() const { return m_view; }

    void FitAll();

    // Tools push an interaction handler to take over viewport input while
    // they're active, and pop it when they finish. Top of stack wins.
    void PushInteraction(lcad::ViewportInteraction* theInteraction);
    void PopInteraction();
    lcad::ViewportInteraction* CurrentInteraction() const;

    // The topmost handler that is a TOOL, or null when the only things
    // pushed are permanent background handlers. This is what to ask when
    // the question is "is the user in the middle of something?" --
    // CurrentInteraction() answers "is anything pushed?", which is always
    // yes once the view cube has materialised.
    lcad::ViewportInteraction* ExclusiveInteraction() const;

    // Called after OCCT's own selection changes, so the shell can read
    // what was picked without polling for it every frame.
    void SetSelectionCallback(std::function<void()> theCallback)
    {
        m_onSelectionChanged = std::move(theCallback);
    }

    // A right CLICK -- pressed and released without moving -- in global
    // screen coordinates, which is where Fusion opens its marking menu. A
    // right DRAG still orbits and never reaches this.
    void SetContextClickCallback(std::function<void(const QPoint&)> theCallback)
    {
        m_onContextClick = std::move(theCallback);
    }

protected:
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

    // AIS_ViewController calls this to finalize a box/lasso selection
    // drag. Overridden so we can pick window-vs-crossing semantics based
    // on drag direction before the rectangle select actually runs.
    void SelectInViewer(const NCollection_Sequence<Graphic3d_Vec2i>& thePnts,
                         const AIS_SelectionScheme theScheme) override;

    // OCCT's own hook, fired once the selection has actually settled --
    // which is after FlushViewEvents, not inside the mouse handler, so
    // reading the selection from mousePressEvent would read the previous
    // one.
    void OnSelectionChanged(const Handle(AIS_InteractiveContext)& theCtx,
                            const Handle(V3d_View)& theView) override;

private:
    void initializeOcctViewer();
    void updateView();
    void updateRubberBandStyle();

    // Offer an event to the interaction stack, innermost first, stopping
    // at the first handler that consumes it. True when one did.
    bool dispatchToInteractions(
        const std::function<bool(lcad::ViewportInteraction*)>& theDeliver);

    // Qt reports mouse positions in logical pixels, but OCCT's Xw_Window
    // renders straight into the native X11 surface, which is sized in
    // physical (device) pixels. On any scaled display those two spaces
    // disagree, which is what makes a drag look detached from the actual
    // cursor -- so every position handed to OCCT goes through this first.
    Graphic3d_Vec2i toDevicePixels(const QPointF& point) const;

    Handle(V3d_Viewer) m_viewer;
    Handle(V3d_View) m_view;
    Handle(AIS_InteractiveContext) m_context;

    bool m_initialized = false;

    // X positions of a left-button drag, tracked to tell a left-to-right
    // "window" box apart from a right-to-left "crossing" box.
    int m_selectionStartX = 0;
    int m_lastMoveX = 0;

    // Active tool input handlers, innermost last.
    std::vector<lcad::ViewportInteraction*> m_interactions;

    std::function<void()> m_onSelectionChanged;

    // Where the right button went down, to tell a click from an orbit.
    std::function<void(const QPoint&)> m_onContextClick;
    QPointF m_rightPressPos;
    bool    m_rightPressed = false;
};

// Thin QWidget wrapper so this drops into a normal Qt layout (menus,
// docking, etc.) while the actual rendering happens in OcctNativeWindow.
class OcctViewport : public QWidget
{
    Q_OBJECT

public:
    explicit OcctViewport(QWidget* parent = nullptr);

    Handle(AIS_InteractiveContext) Context() const { return m_window->Context(); }
    Handle(V3d_View) View() const { return m_window->View(); }

    void FitAll() { m_window->FitAll(); }

    void PushInteraction(lcad::ViewportInteraction* theInteraction)
    {
        m_window->PushInteraction(theInteraction);
    }
    void PopInteraction() { m_window->PopInteraction(); }
    void SetSelectionCallback(std::function<void()> theCallback)
    {
        m_window->SetSelectionCallback(std::move(theCallback));
    }
    void SetContextClickCallback(std::function<void(const QPoint&)> theCallback)
    {
        m_window->SetContextClickCallback(std::move(theCallback));
    }
    lcad::ViewportInteraction* CurrentInteraction() const
    {
        return m_window->CurrentInteraction();
    }
    lcad::ViewportInteraction* ExclusiveInteraction() const
    {
        return m_window->ExclusiveInteraction();
    }

    // The native render window, for code that needs it directly.
    OcctNativeWindow* NativeWindow() const { return m_window; }

private:
    OcctNativeWindow* m_window = nullptr;
};
