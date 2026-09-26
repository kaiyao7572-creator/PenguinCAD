#pragma once

#include <AIS_ListOfInteractive.hxx>
#include <Bnd_Box.hxx>
#include <Graphic3d_Camera.hxx>
#include <V3d_TypeOfOrientation.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

// Where the standard views and the view cube put the camera, what "the
// model" is when a view is fitted to it, and how a camera swing is paced.
//
// Kept free of V3d_View, Qt and any window so tests/run_tests.sh can pin
// the Fusion conventions headlessly: a front view that quietly turned into
// a back view, a cube click that left the model lying on its side, or a
// fit that framed the 360mm origin axes instead of a 20mm part all compile
// and all look "roughly right" at a glance.
namespace lcad {

// A camera orientation with no position: which way it looks and which way
// is up on screen. Screen-right is direction x up.
struct ViewPose
{
    gp_Dir direction;   // from the eye towards what it looks at
    gp_Dir up;
};

// Fusion's standard views in its default Z-up world, where FRONT is the
// -Y side: FRONT looks along +Y with Z up and X to the right, TOP looks
// down -Z with Y up (so the front of the model sits at the bottom of the
// screen, as it does in Fusion), BOTTOM looks up +Z with the front at the
// top, and the home view looks in from the front-right-top corner.
//
// Takes any of the 26 orientations -- faces, edges and corners -- and
// keeps everything but TOP and BOTTOM upright: Z up on screen.
//
// This deliberately does not defer to V3d_View::SetProj's own up rule:
// the whole point is to have one written-down answer a test can hold the
// app to.
ViewPose StandardViewPose(V3d_TypeOfOrientation theOrientation);

// Where a click on the view cube's face, edge or corner theTarget swings a
// camera that is at theCurrent now.
//
// Anything but straight down or straight up stays upright, whatever roll
// the camera had: an edge or corner click never leaves the model lying on
// its side. AIS_ViewCube's own rule -- the roll nearest the current one --
// does exactly that from a top view, where "nearest" to a horizontal up is
// a horizontal up.
//
// Straight down or up, "upright" means nothing, so the camera takes the
// roll it would arrive with by tipping over from where it is -- the
// shortest swing -- squared to a side of the model. From FRONT that puts
// FRONT at the bottom of the top view, from RIGHT it puts RIGHT there.
// From the home corner the swing lands exactly between FRONT and RIGHT,
// and the tie goes to Fusion's own top view, FRONT at the bottom.
ViewPose CubeClickPose(V3d_TypeOfOrientation theTarget, const ViewPose& theCurrent);

// Turns theCamera to thePose about theCenter, keeping its distance --
// and so, in orthographic, its zoom -- untouched. Fitting is separate.
void AimCamera(const Handle(Graphic3d_Camera)& theCamera,
               const ViewPose&                 thePose,
               const gp_Pnt&                   theCenter);

// The box "Fit" frames: every displayed AIS_Shape -- bodies, sketch
// curves, previews -- and nothing else. The origin axes are AIS_Lines
// running 180mm each way from the origin; a fit that counted them framed
// a 20mm part as a speck in the middle of the screen. Anything with
// transform persistence (the view cube, labels, the home icon) is screen
// furniture rather than geometry and is skipped too.
//
// Void when nothing counts, and callers must then leave the zoom alone.
Bnd_Box ModelBounds(const AIS_ListOfInteractive& theObjects);

// Cubic ease-in-out on [0, 1]. A linear camera swing starts and stops with
// a jolt; Fusion's accelerates away and settles in.
double EaseInOut(double theT);

} // namespace lcad
