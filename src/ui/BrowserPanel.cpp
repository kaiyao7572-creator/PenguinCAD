#include "ui/BrowserPanel.h"
#include "ui/UiUtils.h"

#include "core/Feature.h"

#include <QColor>
#include <QMenu>
#include <QSignalBlocker>
#include <QString>
#include <QVariant>

#include <string>

namespace lcad {

namespace {

// Column 0 carries the feature's name plus the suppress checkbox; column 1
// is a read-only type label.
constexpr int kNameColumn = 0;
constexpr int kTypeColumn = 1;

Feature* FeatureOf(const QTreeWidgetItem* theItem)
{
    if (theItem == nullptr) {
        return nullptr;
    }
    return static_cast<Feature*>(theItem->data(kNameColumn, Qt::UserRole).value<void*>());
}

} // namespace

BrowserPanel::BrowserPanel(Document* theDocument, QWidget* theParent)
    : QTreeWidget(theParent)
    , m_document(theDocument)
{
    setColumnCount(2);
    setHeaderLabels({QStringLiteral("Feature"), QStringLiteral("Type")});
    setRootIsDecorated(false);
    setUniformRowHeights(true);
    setContextMenuPolicy(Qt::CustomContextMenu);

    connect(this, &QTreeWidget::itemClicked, this, &BrowserPanel::onItemClicked);
    connect(this, &QTreeWidget::itemChanged, this, &BrowserPanel::onItemChanged);
    connect(this, &QTreeWidget::customContextMenuRequested, this, &BrowserPanel::onContextMenuRequested);

    if (m_document != nullptr) {
        m_document->AddObserver(this);
    }
    RefreshTree();
}

BrowserPanel::~BrowserPanel()
{
    if (m_document != nullptr) {
        m_document->RemoveObserver(this);
    }
}

void BrowserPanel::OnDocumentChanged(Document& /*theDocument*/)
{
    RefreshTree();
}

void BrowserPanel::OnActiveFeatureChanged(Document& /*theDocument*/)
{
    SyncSelection();
}

void BrowserPanel::RefreshTree()
{
    if (m_document == nullptr) {
        return;
    }

    const std::vector<FeaturePtr>& features = m_document->Features();

    // A structural change (feature added/removed/reordered) needs a full
    // rebuild; a pure value/error refresh from editing a parameter
    // elsewhere should update rows in place instead.
    bool structural = static_cast<std::size_t>(topLevelItemCount()) != features.size();
    for (std::size_t i = 0; !structural && i < features.size(); ++i) {
        if (FeatureOf(topLevelItem(static_cast<int>(i))) != features[i].get()) {
            structural = true;
        }
    }

    QSignalBlocker blocker(this);

    if (structural) {
        m_renamingItem = nullptr;
        clear();
        for (const FeaturePtr& feature : features) {
            if (!feature) {
                continue;
            }
            // The QTreeWidget* constructor already appends the item as a
            // top-level row -- calling addTopLevelItem() too would insert
            // it a second time.
            auto* item = new QTreeWidgetItem(this);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setData(kNameColumn, Qt::UserRole, QVariant::fromValue(static_cast<void*>(feature.get())));
        }
    }

    for (std::size_t i = 0; i < features.size(); ++i) {
        const FeaturePtr& feature = features[i];
        if (!feature) {
            continue;
        }
        QTreeWidgetItem* item = topLevelItem(static_cast<int>(i));
        if (item == nullptr) {
            continue;
        }

        // Don't stomp on text the user is actively typing into the inline
        // rename editor.
        if (item != m_renamingItem) {
            item->setText(kNameColumn, QString::fromStdString(feature->Name()));
        }
        item->setText(kTypeColumn, QString::fromStdString(feature->TypeName()));
        item->setCheckState(kNameColumn, feature->IsSuppressed() ? Qt::Unchecked : Qt::Checked);

        const bool failed = !feature->LastError().empty();
        if (failed) {
            const QColor color = ErrorTextColor(palette());
            item->setForeground(kNameColumn, color);
            item->setForeground(kTypeColumn, color);
            const QString tip = QString::fromStdString(feature->LastError());
            item->setToolTip(kNameColumn, tip);
            item->setToolTip(kTypeColumn, tip);
        } else {
            // Clear any previous error styling by removing the role
            // entirely, rather than painting an explicit "normal" color,
            // so selected/hovered rows still theme correctly.
            item->setData(kNameColumn, Qt::ForegroundRole, QVariant());
            item->setData(kTypeColumn, Qt::ForegroundRole, QVariant());
            item->setToolTip(kNameColumn, QString());
            item->setToolTip(kTypeColumn, QString());
        }
    }

    SyncSelection();
}

void BrowserPanel::SyncSelection()
{
    if (m_document == nullptr) {
        return;
    }
    QSignalBlocker blocker(this);
    const FeaturePtr active = m_document->ActiveFeature();
    for (int i = 0; i < topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = topLevelItem(i);
        if (active && FeatureOf(item) == active.get()) {
            setCurrentItem(item);
            return;
        }
    }
    setCurrentItem(nullptr);
}

void BrowserPanel::onItemClicked(QTreeWidgetItem* theItem, int /*theColumn*/)
{
    if (m_document == nullptr) {
        return;
    }
    FeaturePtr feature = FindFeatureByRaw(*m_document, FeatureOf(theItem));
    if (feature) {
        m_document->SetActiveFeature(feature);
    }
}

void BrowserPanel::onItemChanged(QTreeWidgetItem* theItem, int theColumn)
{
    if (m_document == nullptr || theColumn != kNameColumn) {
        return;
    }

    FeaturePtr feature = FindFeatureByRaw(*m_document, FeatureOf(theItem));
    if (!feature) {
        return;
    }

    if (theItem == m_renamingItem) {
        m_renamingItem = nullptr;
        theItem->setFlags(theItem->flags() & ~Qt::ItemIsEditable);

        const std::string newName = theItem->text(kNameColumn).trimmed().toStdString();
        if (!newName.empty() && newName != feature->Name()) {
            feature->SetName(newName);
            // A rename can break a downstream FindFeature() lookup that
            // still holds the old name -- rebuild now so that shows up
            // immediately as a red row instead of waiting for an
            // unrelated edit to trigger the next rebuild.
            m_document->Rebuild();
        } else {
            RefreshTree();
        }
        return;
    }

    const bool suppressed = (theItem->checkState(kNameColumn) == Qt::Unchecked);
    if (suppressed != feature->IsSuppressed()) {
        feature->SetSuppressed(suppressed);
        m_document->Rebuild();
    }
}

void BrowserPanel::onContextMenuRequested(const QPoint& thePos)
{
    QTreeWidgetItem* item = itemAt(thePos);
    if (item == nullptr || m_document == nullptr) {
        return;
    }
    FeaturePtr feature = FindFeatureByRaw(*m_document, FeatureOf(item));
    if (!feature) {
        return;
    }

    QMenu menu(this);
    QAction* renameAction = menu.addAction(QStringLiteral("Rename"));
    QAction* suppressAction = menu.addAction(feature->IsSuppressed() ? QStringLiteral("Unsuppress")
                                                                      : QStringLiteral("Suppress"));
    menu.addSeparator();
    QAction* deleteAction = menu.addAction(QStringLiteral("Delete"));

    QAction* chosen = menu.exec(viewport()->mapToGlobal(thePos));
    if (chosen == renameAction) {
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        m_renamingItem = item;
        editItem(item, kNameColumn);
    } else if (chosen == suppressAction) {
        feature->SetSuppressed(!feature->IsSuppressed());
        m_document->Rebuild();
    } else if (chosen == deleteAction) {
        m_document->RemoveFeature(feature);
    }
}

} // namespace lcad
