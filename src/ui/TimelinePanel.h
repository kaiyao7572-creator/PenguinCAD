#pragma once

#include "core/Document.h"

#include <QWidget>

#include <cstddef>
#include <vector>

class QHBoxLayout;
class QToolButton;

namespace lcad {

// Fusion's horizontal feature timeline: one button per feature in order,
// with a clickable rollback marker before the first feature and after
// every one of them, plus a "go to end" control that clears rollback.
//
// Selection is shared with the browser panel purely through Document's
// active-feature notifications -- the two panels never reference each
// other directly.
class TimelinePanel : public QWidget, public DocumentObserver
{
    Q_OBJECT

public:
    explicit TimelinePanel(Document* theDocument, QWidget* theParent = nullptr);
    ~TimelinePanel() override;

    // DocumentObserver
    void OnDocumentChanged(Document& theDocument) override;
    void OnActiveFeatureChanged(Document& theDocument) override;

private:
    // Rebuilds row widgets only when the feature list changed shape (add,
    // remove, reorder); a pure value/error/rollback refresh just updates
    // the buttons that already exist so a live parameter edit elsewhere
    // doesn't make the strip flicker.
    void RefreshStrip();
    void RebuildStrip(const std::vector<FeaturePtr>& theFeatures);
    void UpdateButtonRow(std::size_t theIndex, const FeaturePtr& theFeature, std::size_t theEffectiveRollback);
    void UpdateMarker(std::size_t theMarkerIndex, std::size_t theEffectiveRollback);
    void SyncActiveButton();

    Document* m_document = nullptr;

    QHBoxLayout* m_stripLayout = nullptr;
    QWidget*     m_stripWidget = nullptr;

    // m_markers always has exactly one more entry than m_featureButtons: a
    // rollback slot before the first feature and one after every feature.
    std::vector<QToolButton*> m_featureButtons;
    std::vector<QToolButton*> m_markers;
};

} // namespace lcad
