#include "core/Command.h"
#include "core/Registration.h"
#include "ui/BrowserPanel.h"
#include "ui/PropertiesPanel.h"
#include "ui/TimelinePanel.h"

#include <QDockWidget>
#include <QMainWindow>

namespace lcad {

// Builds the three Fusion-style docks and hands them to the main window.
// Each panel is a self-sufficient DocumentObserver: they never reference
// each other directly, only Document's notifications keep browser,
// timeline, and properties in sync.
void CreateDockPanels(QMainWindow* theWindow, const CommandContext& theContext)
{
    if (theWindow == nullptr || theContext.document == nullptr) {
        return;
    }
    Document* document = theContext.document;

    auto* browserDock = new QDockWidget("Browser", theWindow);
    browserDock->setObjectName("BrowserDock");
    browserDock->setWidget(new BrowserPanel(document));
    theWindow->addDockWidget(Qt::LeftDockWidgetArea, browserDock);

    auto* propertiesDock = new QDockWidget("Properties", theWindow);
    propertiesDock->setObjectName("PropertiesDock");
    propertiesDock->setWidget(new PropertiesPanel(document));
    theWindow->addDockWidget(Qt::RightDockWidgetArea, propertiesDock);

    auto* timelineDock = new QDockWidget("Timeline", theWindow);
    timelineDock->setObjectName("TimelineDock");
    timelineDock->setWidget(new TimelinePanel(document));
    timelineDock->setMaximumHeight(140);
    theWindow->addDockWidget(Qt::BottomDockWidgetArea, timelineDock);
}

} // namespace lcad
