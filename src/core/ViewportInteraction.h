#pragma once

#include <Graphic3d_Vec2.hxx>

#include <Qt>

namespace lcad {

// Lets a tool temporarily take over viewport input -- placing sketch
// points, dragging a gizmo handle, picking a measurement target.
//
// Push one onto OcctViewport while the tool is active and pop it when the
// tool finishes. Handlers are asked top-of-stack first; return true to
// consume the event, false to let normal orbit/pan/select handling run.
//
// Positions are in DEVICE pixels, already matching OCCT's coordinate
// space, so they can be handed straight to V3d_View::Convert and friends.
class ViewportInteraction
{
public:
    virtual ~ViewportInteraction() = default;

    virtual bool OnMousePress(const Graphic3d_Vec2i& thePos,
                              Qt::MouseButton        theButton,
                              Qt::KeyboardModifiers  theModifiers)
    {
        (void)thePos; (void)theButton; (void)theModifiers;
        return false;
    }

    virtual bool OnMouseMove(const Graphic3d_Vec2i& thePos,
                             Qt::MouseButtons       theButtons,
                             Qt::KeyboardModifiers  theModifiers)
    {
        (void)thePos; (void)theButtons; (void)theModifiers;
        return false;
    }

    virtual bool OnMouseRelease(const Graphic3d_Vec2i& thePos,
                                Qt::MouseButton        theButton,
                                Qt::KeyboardModifiers  theModifiers)
    {
        (void)thePos; (void)theButton; (void)theModifiers;
        return false;
    }

    virtual bool OnMouseDoubleClick(const Graphic3d_Vec2i& thePos,
                                    Qt::MouseButton        theButton,
                                    Qt::KeyboardModifiers  theModifiers)
    {
        (void)thePos; (void)theButton; (void)theModifiers;
        return false;
    }

    // theKey is a Qt::Key value. Escape should normally end the tool.
    virtual bool OnKeyPress(int theKey, Qt::KeyboardModifiers theModifiers)
    {
        (void)theKey; (void)theModifiers;
        return false;
    }

    // Called right after this handler is popped off the stack, so a tool
    // can clean up preview geometry.
    virtual void OnDeactivated() {}
};

} // namespace lcad
