#include "OcctViewport.h"

#include <Aspect_DisplayConnection.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Xw_Window.hxx>

#include <QMouseEvent>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QWheelEvent>

// ---- OcctNativeWindow ----

OcctNativeWindow::OcctNativeWindow(QWindow* parent)
    : QWindow(parent)
{
    // OCCT manages its own GLX context on top of this window; we just
    // need Qt/xcb to give it a plain native window with an OpenGL-capable
    // visual to draw into. Depth/stencil buffers must be requested here,
    // before the native window is created, or OCCT gets a visual it
    // can't properly z-buffer or render into.
    QSurfaceFormat format;
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(8);
    format.setRenderableType(QSurfaceFormat::OpenGL);
    setFormat(format);

    setSurfaceType(QWindow::OpenGLSurface);
}

void OcctNativeWindow::initializeOcctViewer()
{
    if (m_initialized) {
        return;
    }

    Handle(Aspect_DisplayConnection) displayConnection = new Aspect_DisplayConnection();
    Handle(OpenGl_GraphicDriver) graphicDriver = new OpenGl_GraphicDriver(displayConnection, false);

    m_viewer = new V3d_Viewer(graphicDriver);
    m_viewer->SetDefaultLights();
    m_viewer->SetLightOn();

    m_context = new AIS_InteractiveContext(m_viewer);

    m_view = m_viewer->CreateView();

    Handle(Xw_Window) window = new Xw_Window(displayConnection, (Aspect_Drawable)winId());
    m_view->SetWindow(window);
    if (!window->IsMapped()) {
        window->Map();
    }

    m_view->SetBackgroundColor(Quantity_NOC_GRAY30);
    m_view->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, Quantity_NOC_WHITE, 0.08);
    m_view->MustBeResized();

    m_initialized = true;
}

void OcctNativeWindow::FitAll()
{
    if (m_view.IsNull()) {
        return;
    }
    m_view->FitAll();
    m_view->ZFitAll();
    m_view->Redraw();
}

void OcctNativeWindow::exposeEvent(QExposeEvent*)
{
    if (!isExposed()) {
        return;
    }
    if (!m_initialized) {
        initializeOcctViewer();
    }
    if (!m_view.IsNull()) {
        m_view->Redraw();
    }
}

void OcctNativeWindow::resizeEvent(QResizeEvent*)
{
    if (m_initialized && !m_view.IsNull()) {
        m_view->MustBeResized();
    }
}

void OcctNativeWindow::mousePressEvent(QMouseEvent* event)
{
    m_lastX = event->pos().x();
    m_lastY = event->pos().y();

    if (event->button() == Qt::LeftButton) {
        m_dragMode = DragMode::Rotate;
        if (!m_view.IsNull()) {
            m_view->StartRotation(m_lastX, m_lastY);
        }
    } else if (event->button() == Qt::MiddleButton) {
        m_dragMode = DragMode::Pan;
    }
}

void OcctNativeWindow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_dragMode == DragMode::Rotate) {
        m_dragMode = DragMode::None;
    } else if (event->button() == Qt::MiddleButton && m_dragMode == DragMode::Pan) {
        m_dragMode = DragMode::None;
    }

    if (event->button() == Qt::LeftButton && !m_context.IsNull()) {
        m_context->MoveTo(event->pos().x(), event->pos().y(), m_view, Standard_True);
        m_context->SelectDetected();
    }
}

void OcctNativeWindow::mouseMoveEvent(QMouseEvent* event)
{
    const int x = event->pos().x();
    const int y = event->pos().y();

    if (m_view.IsNull()) {
        return;
    }

    if (m_dragMode == DragMode::Rotate) {
        m_view->Rotation(x, y);
    } else if (m_dragMode == DragMode::Pan) {
        m_view->Pan(x - m_lastX, m_lastY - y);
    } else if (!m_context.IsNull()) {
        m_context->MoveTo(x, y, m_view, Standard_True);
    }

    m_lastX = x;
    m_lastY = y;
}

void OcctNativeWindow::wheelEvent(QWheelEvent* event)
{
    if (m_view.IsNull()) {
        return;
    }

    const int delta = event->angleDelta().y();
    const Standard_Real factor = (delta > 0) ? 1.1 : 0.9;
    m_view->SetZoom(factor);
    m_view->Redraw();

    event->accept();
}

// ---- OcctViewport ----

OcctViewport::OcctViewport(QWidget* parent)
    : QWidget(parent)
{
    m_window = new OcctNativeWindow();
    QWidget* container = QWidget::createWindowContainer(m_window, this);

    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(container);
}
