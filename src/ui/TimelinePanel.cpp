#include "ui/TimelinePanel.h"
#include "ui/UiUtils.h"

#include "core/Feature.h"

#include <QColor>
#include <QFrame>
#include <QAction>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QPalette>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QString>
#include <QToolButton>
#include <QVariant>

#include <algorithm>
#include <string>

namespace lcad {

namespace {

constexpr int kMarkerWidth = 12;
constexpr const char* kFeatureRawProperty = "lcadFeatureRaw";

Feature* FeatureOfButton(const QToolButton* theButton)
{
    return static_cast<Feature*>(theButton->property(kFeatureRawProperty).value<void*>());
}

} // namespace

TimelinePanel::TimelinePanel(Document* theDocument, QWidget* theParent)
    : QWidget(theParent)
    , m_document(theDocument)
{
    auto* goToEndButton = new QToolButton(this);
    goToEndButton->setText(QStringLiteral("Go to End"));
    goToEndButton->setToolTip(QStringLiteral("Clear rollback and evaluate the full timeline"));
    goToEndButton->setAutoRaise(true);
    connect(goToEndButton, &QToolButton::clicked, this, [this]() {
        if (m_document != nullptr) {
            m_document->SetRollbackIndex(Document::npos);
        }
    });

    // The strip itself lives inside a horizontally scrolling area so a
    // long timeline doesn't force the dock wider than the window.
    m_stripWidget = new QWidget();
    m_stripLayout = new QHBoxLayout(m_stripWidget);
    m_stripLayout->setContentsMargins(4, 2, 4, 2);
    m_stripLayout->setSpacing(0);

    auto* scroll = new QScrollArea(this);
    scroll->setWidget(m_stripWidget);
    scroll->setWidgetResizable(false);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(4, 4, 4, 4);
    rootLayout->addWidget(goToEndButton);
    rootLayout->addWidget(scroll, 1);

    if (m_document != nullptr) {
        m_document->AddObserver(this);
    }
    RefreshStrip();
}

TimelinePanel::~TimelinePanel()
{
    if (m_document != nullptr) {
        m_document->RemoveObserver(this);
    }
}

void TimelinePanel::OnDocumentChanged(Document& /*theDocument*/)
{
    RefreshStrip();
}

void TimelinePanel::OnActiveFeatureChanged(Document& /*theDocument*/)
{
    SyncActiveButton();
}

void TimelinePanel::RefreshStrip()
{
    if (m_document == nullptr) {
        return;
    }

    const std::vector<FeaturePtr>& features = m_document->Features();

    // Only tear the strip down when the feature list changed shape;
    // captured click handlers reference a specific feature, so a reorder
    // needs fresh buttons even though the count is unchanged.
    bool structural = m_featureButtons.size() != features.size();
    for (std::size_t i = 0; !structural && i < features.size(); ++i) {
        if (FeatureOfButton(m_featureButtons[i]) != features[i].get()) {
            structural = true;
        }
    }
    if (structural) {
        RebuildStrip(features);
    }

    const std::size_t rollback = m_document->RollbackIndex();
    const std::size_t effectiveRollback = std::min(rollback, features.size());

    for (std::size_t i = 0; i < features.size(); ++i) {
        UpdateButtonRow(i, features[i], effectiveRollback);
    }
    for (std::size_t i = 0; i < m_markers.size(); ++i) {
        UpdateMarker(i, effectiveRollback);
    }

    // The scroll area does not resize the strip (setWidgetResizable(false)
    // keeps it at its natural width so a long timeline scrolls), so it
    // must be told to take the size its buttons now need -- renames and
    // rollback change their widths too.
    m_stripWidget->adjustSize();

    SyncActiveButton();
}

void TimelinePanel::RebuildStrip(const std::vector<FeaturePtr>& theFeatures)
{
    QLayoutItem* child = nullptr;
    while ((child = m_stripLayout->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }
    m_featureButtons.clear();
    m_markers.clear();

    auto addMarker = [this](std::size_t theIndex) {
        auto* marker = new QToolButton(m_stripWidget);
        marker->setText(QStringLiteral("|"));
        marker->setAutoRaise(true);
        marker->setCheckable(true);
        marker->setFixedWidth(kMarkerWidth);
        marker->setToolTip(QStringLiteral("Roll back to here"));
        connect(marker, &QToolButton::clicked, this, [this, theIndex]() {
            if (m_document != nullptr) {
                m_document->SetRollbackIndex(theIndex);
            }
        });
        m_stripLayout->addWidget(marker);
        marker->show();   // see the button below
        m_markers.push_back(marker);
    };

    // One marker before the first feature, then one after every feature.
    addMarker(0);
    for (std::size_t i = 0; i < theFeatures.size(); ++i) {
        const FeaturePtr& feature = theFeatures[i];

        auto* button = new QToolButton(m_stripWidget);
        button->setAutoRaise(true);
        button->setCheckable(true);
        button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        button->setProperty(kFeatureRawProperty, QVariant::fromValue(static_cast<void*>(feature.get())));

        const Feature* raw = feature.get();
        connect(button, &QToolButton::clicked, this, [this, raw]() {
            if (m_document == nullptr) {
                return;
            }
            FeaturePtr clicked = FindFeatureByRaw(*m_document, raw);
            if (clicked) {
                m_document->SetActiveFeature(clicked);
            }
        });

        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QToolButton::customContextMenuRequested, this,
                [this, button, raw](const QPoint& thePos) {
                    ShowFeatureMenu(raw, button->mapToGlobal(thePos));
                });

        m_stripLayout->addWidget(button);
        // A child added to a strip that is already on screen is only shown
        // by a QUEUED call, and until then the layout counts it as hidden --
        // so the adjustSize below measured an empty strip, nothing resized
        // it once the buttons appeared, and the timeline showed no features
        // at all. Show it now.
        button->show();
        m_featureButtons.push_back(button);

        addMarker(i + 1);
    }

    m_stripLayout->addStretch(1);
}

void TimelinePanel::UpdateButtonRow(std::size_t theIndex, const FeaturePtr& theFeature, std::size_t theEffectiveRollback)
{
    if (theIndex >= m_featureButtons.size() || !theFeature) {
        return;
    }
    QToolButton* button = m_featureButtons[theIndex];
    QSignalBlocker blocker(button);

    const QString label = QString::fromStdString(theFeature->Name()) + "\n"
                         + QString::fromStdString(theFeature->TypeName());
    button->setText(label);

    // Rolled-back and suppressed features are both skipped during
    // rebuild -- grey out either the same way Document treats them alike.
    const bool skipped = theFeature->IsSuppressed() || (theIndex >= theEffectiveRollback);
    button->setEnabled(!skipped);

    const bool failed = !theFeature->LastError().empty();
    button->setToolTip(failed ? QString::fromStdString(theFeature->LastError()) : label);

    if (failed) {
        // A fresh palette with only these two roles set so every other
        // role (background, highlight...) keeps following the live theme;
        // copying the resolved palette here would pin all of it instead.
        QPalette pal;
        const QColor color = ErrorTextColor(button->palette());
        pal.setColor(QPalette::ButtonText, color);
        pal.setColor(QPalette::WindowText, color);
        button->setPalette(pal);
    } else {
        button->setPalette(QPalette());
    }
}

void TimelinePanel::ShowFeatureMenu(const Feature* theRaw, const QPoint& theGlobalPos)
{
    if (m_document == nullptr) {
        return;
    }
    FeaturePtr feature = FindFeatureByRaw(*m_document, theRaw);
    if (!feature) {
        return;
    }

    QMenu menu(this);
    QAction* renameAction = menu.addAction(QStringLiteral("Rename"));
    QAction* suppressAction = menu.addAction(feature->IsSuppressed()
                                                 ? QStringLiteral("Unsuppress")
                                                 : QStringLiteral("Suppress"));
    menu.addSeparator();
    QAction* deleteAction = menu.addAction(QStringLiteral("Delete"));

    QAction* chosen = menu.exec(theGlobalPos);
    if (chosen == renameAction) {
        bool accepted = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Rename Feature"),
                                                   QStringLiteral("Name:"), QLineEdit::Normal,
                                                   QString::fromStdString(feature->Name()),
                                                   &accepted);
        const std::string trimmed = name.trimmed().toStdString();
        if (accepted && !trimmed.empty() && trimmed != feature->Name()) {
            feature->SetName(trimmed);
            // A rename can break a downstream FindFeature() that still
            // holds the old name, so rebuild now rather than letting the
            // breakage surface on some later, unrelated edit.
            m_document->Rebuild();
        }
    } else if (chosen == suppressAction) {
        feature->SetSuppressed(!feature->IsSuppressed());
        m_document->Rebuild();
    } else if (chosen == deleteAction) {
        m_document->RemoveFeature(feature);
    }
}

void TimelinePanel::UpdateMarker(std::size_t theMarkerIndex, std::size_t theEffectiveRollback)
{
    if (theMarkerIndex >= m_markers.size()) {
        return;
    }
    QToolButton* marker = m_markers[theMarkerIndex];
    QSignalBlocker blocker(marker);
    marker->setChecked(theMarkerIndex == theEffectiveRollback);
}

void TimelinePanel::SyncActiveButton()
{
    if (m_document == nullptr) {
        return;
    }
    const FeaturePtr active = m_document->ActiveFeature();
    for (QToolButton* button : m_featureButtons) {
        QSignalBlocker blocker(button);
        button->setChecked(active && FeatureOfButton(button) == active.get());
    }
}

} // namespace lcad
