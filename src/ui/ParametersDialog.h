#pragma once

#include "core/Document.h"
#include "core/Feature.h"

#include <QDialog>
#include <QSet>
#include <QString>

#include <string>

class QAction;
class QLabel;
class QPoint;
class QTreeWidget;
class QTreeWidgetItem;

namespace lcad {

class ParameterCellDelegate;

// Fusion's SOLID > MODIFY > Change Parameters: one table holding the
// document's user parameters and every numeric dimension of every feature,
// each editable in place.
//
// There is no Apply and no Cancel, exactly as in Fusion: every cell edit
// goes straight to a Document mutator, is its own undo step, and rebuilds
// the model on the spot. OK only closes the window.
//
// The table is rebuilt from the document rather than edited alongside it,
// so it cannot drift from the model: a rename that rewrote three feature
// expressions, or an Undo, shows up because the dialog re-reads what is
// there, not because each edit remembered to patch the rows it touched.
class ParametersDialog : public QDialog, public DocumentObserver
{
    Q_OBJECT

public:
    explicit ParametersDialog(Document* theDocument, QWidget* theParent = nullptr);
    ~ParametersDialog() override;

    // DocumentObserver
    void OnDocumentChanged(Document& theDocument) override;

private:
    // Refresh() on the next event-loop turn, never now. Every mutator this
    // dialog calls notifies OnDocumentChanged synchronously, usually from
    // inside a cell's own commit -- see OnItemChanged.
    void ScheduleRefresh();

    // Tear the rows down and rebuild them from the document, carrying the
    // selected row, collapsed rows and scroll position across by key. Only
    // safe with no cell commit on the stack; ScheduleRefresh() is how
    // everything but the constructor gets here.
    void Refresh();

    // A cell editor finished. Reads what it needs off theItem FIRST, then
    // calls the mutator, and leaves rebuilding the rows to ScheduleRefresh.
    void OnItemChanged(QTreeWidgetItem* theItem, int theColumn);
    void OnContextMenu(const QPoint& thePos);

    // Returns the name the row answers to afterwards: the new one after an
    // accepted rename, theName otherwise.
    std::string CommitUserParameter(const std::string& theName, int theColumn,
                                    const QString& theText);
    void CommitModelParameter(const std::string& theFeatureName, int theFeatureIndex,
                              const std::string& theParameterName, const QString& theText);

    // A model parameter's Name cell: rename d3 to "wall", as Fusion allows.
    void CommitModelName(const std::string& theFeatureName, int theFeatureIndex,
                         const std::string& theParameterName, const QString& theText);

    // The "+" button: a small modal "Add User Parameter" dialog that stays
    // open, showing why, when the document refuses what was typed.
    void AddUserParameter();

    // Fusion refuses to delete a parameter anything still reads.
    void DeleteCurrentParameter();

    // By name rather than by FeaturePtr: an undo swaps in clones, so a
    // pointer taken at the last refresh can point at a feature that is no
    // longer in the timeline. theIndex is only a hint that tells two
    // features with the same name apart.
    FeaturePtr FindFeature(const std::string& theName, int theIndex) const;

    QSet<QString> CollapsedKeys() const;

    void ShowError(const QString& theText);
    void ClearStatus();

    Document*              m_document = nullptr;
    QTreeWidget*           m_tree = nullptr;
    ParameterCellDelegate* m_delegate = nullptr;
    QAction*               m_deleteAction = nullptr;
    QLabel*                m_statusLabel = nullptr;

    // A refresh is queued on the event loop.
    bool m_refreshQueued = false;

    // OnItemChanged is on the stack; Refresh() must not run.
    bool m_isCommitting = false;

    // A refresh came due while a cell editor was open and waits for it to
    // close -- rebuilding would destroy the editor mid-typing.
    bool m_refreshAfterEdit = false;

    // Row to select after the next refresh when it is not the current one:
    // a parameter just added or renamed, or the neighbour of one deleted.
    QString m_pendingSelection;
};

} // namespace lcad
