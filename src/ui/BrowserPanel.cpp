#include "ui/BrowserPanel.h"
#include "ui/UiUtils.h"

#include "core/Body.h"
#include "core/Entity.h"
#include "core/Feature.h"
#include "core/Origin.h"

#include <QColor>
#include <QHeaderView>
#include <QMenu>
#include <QSignalBlocker>
#include <QString>
#include <QVariant>

#include <string>

namespace lcad {

namespace {

// Column 0 carries the name plus the checkbox; column 1 is a read-only
// type label, showing Fusion's own word for the thing.
constexpr int kNameColumn = 0;
constexpr int kTypeColumn = 1;

// What a row stands for. Stored on the row because the kinds answer
// clicks, checkboxes and right-clicks completely differently.
enum class RowKind
{
    Folder = 0,
    Feature,
    Body,
    Origin
};

constexpr int kKindRole = Qt::UserRole + 1;
constexpr int kBodyRole = Qt::UserRole + 2;

RowKind KindOf(const QTreeWidgetItem* theItem)
{
    if (theItem == nullptr) {
        return RowKind::Folder;
    }
    return static_cast<RowKind>(theItem->data(kNameColumn, kKindRole).toInt());
}

Feature* FeatureOf(const QTreeWidgetItem* theItem)
{
    if (theItem == nullptr || KindOf(theItem) != RowKind::Feature) {
        return nullptr;
    }
    return static_cast<Feature*>(theItem->data(kNameColumn, Qt::UserRole).value<void*>());
}

// Which folder a feature belongs in, or None when it belongs in the
// timeline instead -- an extrude is not a thing the design CONTAINS, it
// is a step in how the design was made.
BrowserFolder FolderOfFeature(const Feature& theFeature)
{
    const std::string& type = theFeature.TypeName();
    if (type == "Sketch") {
        return BrowserFolder::Sketches;
    }
    if (type == "ConstructionPlane" || type == "ConstructionAxis"
     || type == "ConstructionPoint") {
        return BrowserFolder::Construction;
    }
    return BrowserFolder::None;
}

EntityType EntityTypeOfFeature(const Feature& theFeature)
{
    const std::string& type = theFeature.TypeName();
    if (type == "Sketch") {
        return EntityType::Sketch;
    }
    if (type == "ConstructionPlane") {
        return EntityType::ConstructionPlane;
    }
    if (type == "ConstructionAxis") {
        return EntityType::ConstructionAxis;
    }
    if (type == "ConstructionPoint") {
        return EntityType::ConstructionPoint;
    }
    return EntityType::Feature;
}

QTreeWidgetItem* MakeFolder(QTreeWidget* theTree, BrowserFolder theFolder)
{
    auto* item = new QTreeWidgetItem(theTree);
    item->setText(kNameColumn, QString::fromStdString(BrowserFolderName(theFolder)));
    item->setData(kNameColumn, kKindRole, static_cast<int>(RowKind::Folder));
    // Folders carry no checkbox: Fusion's folder light-bulbs hide
    // everything inside them, which needs per-entity visibility to be
    // wired up first, and a checkbox that does nothing is worse than none.
    item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
    return item;
}

} // namespace

BrowserPanel::BrowserPanel(Document* theDocument, QWidget* theParent)
    : QTreeWidget(theParent)
    , m_document(theDocument)
{
    setColumnCount(2);
    // No column headers: Fusion's browser has none, the dock is already
    // titled, and the row that would carry them is worth more as tree.
    setHeaderHidden(true);
    // The name column takes whatever is left after the type label, so a
    // body called "Bracket Mounting Plate" is readable in a narrow dock
    // instead of being elided to three dots.
    header()->setSectionResizeMode(kNameColumn, QHeaderView::Stretch);
    header()->setSectionResizeMode(kTypeColumn, QHeaderView::ResizeToContents);
    setRootIsDecorated(true);
    // Fusion indents its browser tightly. With a dock this narrow the
    // default indent costs more in elided names than it buys in clarity,
    // and there are only ever two levels to tell apart.
    setIndentation(12);
    // Wide enough that a default name and its type both fit without
    // eliding. The dock can still be dragged narrower; this is only what
    // it opens at.
    setMinimumWidth(210);
    setUniformRowHeights(true);
    setContextMenuPolicy(Qt::CustomContextMenu);

    connect(this, &QTreeWidget::itemClicked, this, &BrowserPanel::onItemClicked);
    connect(this, &QTreeWidget::itemChanged, this, &BrowserPanel::onItemChanged);
    connect(this, &QTreeWidget::customContextMenuRequested, this,
            &BrowserPanel::onContextMenuRequested);

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

QSet<QString> BrowserPanel::ExpandedFolders() const
{
    QSet<QString> open;
    for (int i = 0; i < topLevelItemCount(); ++i) {
        const QTreeWidgetItem* item = topLevelItem(i);
        if (item != nullptr && item->isExpanded()) {
            open.insert(item->text(kNameColumn));
        }
    }
    return open;
}

void BrowserPanel::RestoreExpanded(const QSet<QString>& theOpen)
{
    for (int i = 0; i < topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = topLevelItem(i);
        if (item != nullptr) {
            item->setExpanded(theOpen.contains(item->text(kNameColumn)));
        }
    }
}

void BrowserPanel::RefreshTree()
{
    if (m_document == nullptr) {
        return;
    }
    if (m_renamingItem != nullptr) {
        return;  // don't pull the tree out from under an open rename editor
    }

    QSignalBlocker blocker(this);

    // Folders are cheap and the tree is small, so it is rebuilt whole
    // rather than diffed -- only the expansion state has to survive, and
    // that is remembered by name.
    const QSet<QString> open = ExpandedFolders().isEmpty()
                                   ? QSet<QString>{QStringLiteral("Bodies"),
                                                   QStringLiteral("Sketches"),
                                                   QStringLiteral("Construction")}
                                   : ExpandedFolders();
    clear();

    // ---- Origin: always present, always first ----
    QTreeWidgetItem* originFolder = MakeFolder(this, BrowserFolder::Origin);
    for (const OriginEntity& entity : OriginEntities()) {
        auto* item = new QTreeWidgetItem(originFolder);
        item->setText(kNameColumn, QString::fromStdString(entity.name));
        item->setText(kTypeColumn, QString::fromStdString(EntityDisplayName(entity.type)));
        item->setData(kNameColumn, kKindRole, static_cast<int>(RowKind::Origin));
        // Origin geometry cannot be renamed, deleted or suppressed --
        // it is intrinsic to the component, as it is in Fusion.
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
    }

    // ---- Bodies ----
    const std::vector<BodyPtr>& bodies = m_document->Bodies();
    if (!bodies.empty()) {
        QTreeWidgetItem* folder = MakeFolder(this, BrowserFolder::Bodies);
        for (const BodyPtr& body : bodies) {
            if (!body) {
                continue;
            }
            auto* item = new QTreeWidgetItem(folder);
            item->setText(kNameColumn, QString::fromStdString(body->Name()));
            item->setText(kTypeColumn,
                          QString::fromStdString(EntityDisplayName(EntityType::BRepBody)));
            item->setData(kNameColumn, kKindRole, static_cast<int>(RowKind::Body));
            item->setData(kNameColumn, kBodyRole, QString::fromStdString(body->Name()));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(kNameColumn, body->IsVisible() ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(kTypeColumn,
                             QStringLiteral("%1 faces, %2 edges, %3 vertices")
                                 .arg(body->FaceCount())
                                 .arg(body->EdgeCount())
                                 .arg(body->VertexCount()));
        }
    }

    // ---- Sketches and Construction ----
    for (const BrowserFolder folder : {BrowserFolder::Sketches, BrowserFolder::Construction}) {
        QTreeWidgetItem* parent = nullptr;
        for (const FeaturePtr& feature : m_document->Features()) {
            if (!feature || FolderOfFeature(*feature) != folder) {
                continue;
            }
            if (parent == nullptr) {
                parent = MakeFolder(this, folder);
            }

            auto* item = new QTreeWidgetItem(parent);
            item->setText(kNameColumn, QString::fromStdString(feature->Name()));
            item->setText(kTypeColumn, QString::fromStdString(
                                           EntityDisplayName(EntityTypeOfFeature(*feature))));
            item->setData(kNameColumn, kKindRole, static_cast<int>(RowKind::Feature));
            item->setData(kNameColumn, Qt::UserRole,
                          QVariant::fromValue(static_cast<void*>(feature.get())));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(kNameColumn,
                                feature->IsSuppressed() ? Qt::Unchecked : Qt::Checked);

            if (!feature->LastError().empty()) {
                // A broken sketch or plane has to be findable at a glance;
                // this is the same red the timeline uses.
                const QColor color = ErrorTextColor(palette());
                item->setForeground(kNameColumn, color);
                item->setForeground(kTypeColumn, color);
                const QString tip = QString::fromStdString(feature->LastError());
                item->setToolTip(kNameColumn, tip);
                item->setToolTip(kTypeColumn, tip);
            }
        }
    }

    RestoreExpanded(open);
    SyncSelection();
}

void BrowserPanel::SyncSelection()
{
    if (m_document == nullptr) {
        return;
    }
    QSignalBlocker blocker(this);
    const FeaturePtr active = m_document->ActiveFeature();
    if (!active) {
        setCurrentItem(nullptr);
        return;
    }
    for (QTreeWidgetItemIterator it(this); *it != nullptr; ++it) {
        if (FeatureOf(*it) == active.get()) {
            setCurrentItem(*it);
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
    if (m_document == nullptr || theColumn != kNameColumn || theItem == nullptr) {
        return;
    }

    if (KindOf(theItem) == RowKind::Body) {
        Body* body = m_document->FindBody(
            theItem->data(kNameColumn, kBodyRole).toString().toStdString());
        if (body == nullptr) {
            return;
        }
        const bool visible = theItem->checkState(kNameColumn) == Qt::Checked;
        if (visible != body->IsVisible()) {
            body->SetVisible(visible);
            // Not Rebuild(): hiding a body changes nothing about the
            // model, only what is drawn. Rebuilding would be wasted work
            // and would throw away the rest of the timeline's results.
            m_document->NotifyChanged();
        }
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

    if (KindOf(item) == RowKind::Body) {
        Body* body = m_document->FindBody(
            item->data(kNameColumn, kBodyRole).toString().toStdString());
        if (body == nullptr) {
            return;
        }
        QMenu menu(this);
        QAction* toggle = menu.addAction(body->IsVisible() ? QStringLiteral("Hide")
                                                           : QStringLiteral("Show"));
        if (menu.exec(viewport()->mapToGlobal(thePos)) == toggle) {
            body->SetVisible(!body->IsVisible());
            m_document->NotifyChanged();
        }
        return;
    }

    FeaturePtr feature = FindFeatureByRaw(*m_document, FeatureOf(item));
    if (!feature) {
        return;  // folders and origin geometry have no menu, as in Fusion
    }

    QMenu menu(this);
    QAction* renameAction = menu.addAction(QStringLiteral("Rename"));
    QAction* suppressAction = menu.addAction(feature->IsSuppressed()
                                                 ? QStringLiteral("Unsuppress")
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
