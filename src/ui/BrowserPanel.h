#pragma once

#include "core/Document.h"

#include <QPoint>
#include <QTreeWidget>

namespace lcad {

class Feature;

// Fusion's model tree: every feature in timeline order, with a per-row
// suppress checkbox and a right-click menu for rename/delete/suppress.
// Features that failed to compute show in red with the error as a
// tooltip -- this is how the user finds a broken model at a glance.
//
// Selection is not wired directly to the timeline/properties panels; all
// three talk only through Document's observer notifications, so any of
// them can come and go independently.
class BrowserPanel : public QTreeWidget, public DocumentObserver
{
    Q_OBJECT

public:
    explicit BrowserPanel(Document* theDocument, QWidget* theParent = nullptr);
    ~BrowserPanel() override;

    // DocumentObserver
    void OnDocumentChanged(Document& theDocument) override;
    void OnActiveFeatureChanged(Document& theDocument) override;

private slots:
    void onItemClicked(QTreeWidgetItem* theItem, int theColumn);
    void onItemChanged(QTreeWidgetItem* theItem, int theColumn);
    void onContextMenuRequested(const QPoint& thePos);

private:
    // Full teardown + repopulate, used only when the feature list actually
    // changed shape (add/remove/reorder); a pure value or error refresh
    // updates existing rows in place so scroll position and an in-flight
    // rename survive a live parameter edit elsewhere.
    void RefreshTree();
    void SyncSelection();

    Document* m_document = nullptr;

    // Set only while a rename's inline editor is open, so onItemChanged
    // can tell a typed name apart from a checkbox click -- both land on
    // column 0 and look identical to the signal.
    QTreeWidgetItem* m_renamingItem = nullptr;
};

} // namespace lcad
