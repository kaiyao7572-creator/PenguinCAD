#pragma once

#include "core/Document.h"

#include <QPoint>
#include <QSet>
#include <QString>
#include <QTreeWidget>

namespace lcad {

class Feature;

// Fusion's browser: what the design CONTAINS, folder by folder.
//
// Deliberately not a list of features. In Fusion the browser holds the
// origin geometry, the bodies, the sketches and the construction
// geometry, while the features that made them live in the timeline --
// they are two different questions ("what is there" versus "how did it
// get there") and Fusion answers them in two different places. Feature
// rename, suppress and delete moved to the timeline's own context menu
// when this stopped listing features, so nothing was lost in the move.
//
// Rows are one of four kinds: a folder, a body, a sketch or construction
// feature, or a read-only origin entity. The checkbox means visibility on
// a body and suppression on a feature, matching what each one supports.
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
    void RefreshTree();
    void SyncSelection();

    // Folders the user has opened, remembered by name across the rebuild
    // that every document change triggers -- a tree that collapsed itself
    // every time a dimension changed would be unusable.
    QSet<QString> ExpandedFolders() const;
    void RestoreExpanded(const QSet<QString>& theOpen);

    Document* m_document = nullptr;

    // Set only while a rename's inline editor is open, so onItemChanged
    // can tell a typed name apart from a checkbox click -- both land on
    // column 0 and look identical to the signal.
    QTreeWidgetItem* m_renamingItem = nullptr;
};

} // namespace lcad
