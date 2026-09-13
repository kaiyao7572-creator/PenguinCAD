#pragma once

#include <QWidget>
#include <QWindow>

#include <AIS_InteractiveContext.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

// The actual native window OCCT renders into. QWindow (unlike a plain
// QWidget) always represents a real native platform window and reliably
// delivers expose events, which is what OCCT's viewer needs to know when
// it's safe to create its GL context and draw.
class OcctNativeWindow : public QWindow
{
    Q_OBJECT

public:
    explicit OcctNativeWindow(QWindow* parent = nullptr);

    Handle(AIS_InteractiveContext) Context() const { return m_context; }
    Handle(V3d_View) View() const { return m_view; }

    void FitAll();

protected:
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void initializeOcctViewer();

    Handle(V3d_Viewer) m_viewer;
    Handle(V3d_View) m_view;
    Handle(AIS_InteractiveContext) m_context;

    bool m_initialized = false;

    enum class DragMode { None, Rotate, Pan };
    DragMode m_dragMode = DragMode::None;
    int m_lastX = 0;
    int m_lastY = 0;
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

private:
    OcctNativeWindow* m_window = nullptr;
};
