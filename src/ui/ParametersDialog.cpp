#include "ui/ParametersDialog.h"
#include "ui/UiUtils.h"

#include "core/ParameterTable.h"
#include "core/Units.h"

#include <QAbstractItemDelegate>
#include <QAction>
#include <QBrush>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringList>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace lcad {

namespace {

enum Column
{
    kParameterColumn = 0,
    kNameColumn,
    kUnitColumn,
    kExpressionColumn,
    kValueColumn,
    kCommentsColumn,
    kColumnCount
};

enum class RowKind
{
    Group,
    UserParameter,
    Feature,
    ModelParameter
};

// Row-level data lives on the Parameter column. kEditableRole is per cell:
// Qt's ItemIsEditable flag is per ROW, and Fusion lets you edit a user
// parameter's name but not its value, so the delegate asks each cell.
constexpr int kKindRole = Qt::UserRole;
constexpr int kKeyRole = Qt::UserRole + 1;
constexpr int kNameRole = Qt::UserRole + 2;           // user or model parameter name
constexpr int kFeatureRole = Qt::UserRole + 3;        // feature name
constexpr int kFeatureIndexRole = Qt::UserRole + 4;   // its timeline index at refresh time
constexpr int kEditableRole = Qt::UserRole + 5;

const QString kUserGroupKey = QStringLiteral("group:user");
const QString kModelGroupKey = QStringLiteral("group:model");

// Keys name a row by what it shows, so the selection and the collapsed
// rows survive a rebuild that recreated every item. The separator is a
// control character no feature or parameter name will contain.
QString UserKey(const std::string& theName)
{
    return QStringLiteral("user:") + QString::fromStdString(theName);
}

QString FeatureKey(const std::string& theFeature)
{
    return QStringLiteral("feature:") + QString::fromStdString(theFeature);
}

QString ModelKey(const std::string& theFeature, const std::string& theParameter)
{
    return QStringLiteral("model:") + QString::fromStdString(theFeature) + QChar(0x1F)
           + QString::fromStdString(theParameter);
}

QString KeyOf(const QTreeWidgetItem* theItem)
{
    return theItem != nullptr ? theItem->data(kParameterColumn, kKeyRole).toString() : QString();
}

RowKind KindOf(const QTreeWidgetItem* theItem)
{
    return static_cast<RowKind>(theItem->data(kParameterColumn, kKindRole).toInt());
}

// The row to land on once theItem is deleted, so pressing Delete again
// keeps working down the list instead of dropping the selection.
QString NeighbourKey(const QTreeWidgetItem* theItem)
{
    const QTreeWidgetItem* parent = theItem->parent();
    if (parent == nullptr) {
        return QString();
    }
    const int index = parent->indexOfChild(const_cast<QTreeWidgetItem*>(theItem));
    if (index + 1 < parent->childCount()) {
        return KeyOf(parent->child(index + 1));
    }
    if (index > 0) {
        return KeyOf(parent->child(index - 1));
    }
    return KeyOf(parent);
}

// Sketch dimensions carry Fusion's own model-parameter names (d1, d2...),
// which is what the Name column is for; "Distance" on an extrude is a
// description, not a name anything can refer to.
bool IsDimensionLabel(const std::string& theName)
{
    if (theName.size() < 2 || theName[0] != 'd') {
        return false;
    }
    for (std::size_t i = 1; i < theName.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(theName[i])) == 0) {
            return false;
        }
    }
    return true;
}

QString ModelUnitSymbol(UnitKind theKind)
{
    switch (theKind) {
        case UnitKind::Length:
            return QStringLiteral("mm");
        case UnitKind::Angle:
            return QStringLiteral("deg");
        case UnitKind::Unitless:
            break;
    }
    return QString();
}

QString UserUnitSymbol(const UserParameter& theParameter)
{
    switch (theParameter.kind) {
        case UnitKind::Length:
            return QString::fromStdString(SymbolOf(theParameter.lengthUnit));
        case UnitKind::Angle:
            return QString::fromStdString(SymbolOf(theParameter.angleUnit));
        case UnitKind::Unitless:
            break;
    }
    return QString();
}

// A plain number is shown formatted ("20 mm"), exactly what ParseValue
// reads back, so opening a cell and pressing Enter changes nothing.
std::string ShownExpression(const Parameter& theParameter)
{
    return theParameter.expression.empty()
               ? FormatValue(theParameter.doubleValue, theParameter.Kind())
               : theParameter.expression;
}

// Core errors are phrase fragments ("a parameter needs a name") meant to be
// spliced into a sentence; standing alone in a status line they read better
// capitalised.
QString Sentence(const std::string& theText, const char* theFallback)
{
    QString text = QString::fromStdString(theText.empty() ? std::string(theFallback) : theText);
    if (!text.isEmpty()) {
        text[0] = text[0].toUpper();
    }
    return text;
}

// Fusion's Unit list for a new parameter, in its order.
struct UnitChoice
{
    const char* label;
    UnitKind    kind;
    LengthUnit  lengthUnit;
    AngleUnit   angleUnit;
};

const UnitChoice kUnitChoices[] = {
    {"mm", UnitKind::Length, LengthUnit::Millimeter, AngleUnit::Degree},
    {"cm", UnitKind::Length, LengthUnit::Centimeter, AngleUnit::Degree},
    {"m", UnitKind::Length, LengthUnit::Meter, AngleUnit::Degree},
    {"in", UnitKind::Length, LengthUnit::Inch, AngleUnit::Degree},
    {"ft", UnitKind::Length, LengthUnit::Foot, AngleUnit::Degree},
    {"deg", UnitKind::Angle, LengthUnit::Millimeter, AngleUnit::Degree},
    {"rad", UnitKind::Angle, LengthUnit::Millimeter, AngleUnit::Radian},
    {"No Units", UnitKind::Unitless, LengthUnit::Millimeter, AngleUnit::Degree},
};
constexpr int kUnitChoiceCount = static_cast<int>(sizeof(kUnitChoices) / sizeof(kUnitChoices[0]));

// A new parameter starts in the unit the rest of the design is shown in.
int DefaultUnitChoice()
{
    const LengthUnit unit = DefaultLengthUnit();
    for (int i = 0; i < kUnitChoiceCount; ++i) {
        if (kUnitChoices[i].kind == UnitKind::Length && kUnitChoices[i].lengthUnit == unit) {
            return i;
        }
    }
    return 0;
}

// Not "d1": that is the shape of Fusion's sketch-dimension names, and a
// user parameter called d3 next to a sketch dimension called d3 is a trap.
std::string FirstFreeName(const ParameterTable& theTable)
{
    std::string error;
    for (int i = 1; i < 100000; ++i) {
        const std::string candidate = "param" + std::to_string(i);
        if (theTable.IsNameAvailable(candidate, -1, error)) {
            return candidate;
        }
    }
    return "param";
}

void TintError(QLabel* theLabel)
{
    QPalette palette = theLabel->palette();
    palette.setColor(QPalette::WindowText, ErrorTextColor(theLabel->palette()));
    theLabel->setPalette(palette);
}

QTreeWidgetItem* MakeGroupRow(QTreeWidget* theTree, const QString& theLabel, const QString& theKey)
{
    auto* row = new QTreeWidgetItem(theTree);
    row->setText(kParameterColumn, theLabel);
    row->setData(kParameterColumn, kKindRole, static_cast<int>(RowKind::Group));
    row->setData(kParameterColumn, kKeyRole, theKey);
    QFont font = row->font(kParameterColumn);
    font.setBold(true);
    row->setFont(kParameterColumn, font);
    row->setFirstColumnSpanned(true);
    return row;
}

} // namespace

// Per-cell editability, and whether an editor is open right now.
//
// QAbstractItemView::state() would answer the second question but is
// protected; the delegate creates and destroys every editor, so it knows.
// Editors are tracked by QPointer rather than counted, so one that dies by
// some path other than destroyEditor cannot leave the dialog believing an
// edit is still open and never refreshing again.
//
// Not Q_OBJECT: it adds no signals or slots, and a Q_OBJECT class in a .cpp
// is never seen by moc.
class ParameterCellDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    // Called after an editor has been released, by whichever path closed
    // it. The view's closeEditor slot is not only reached through the
    // delegate's closeEditor signal -- a current-row change closes editors
    // directly -- so this is the one hook that sees every close.
    void SetEditorClosedCallback(std::function<void()> theCallback)
    {
        m_onEditorClosed = std::move(theCallback);
    }

    QWidget* createEditor(QWidget*                    theParent,
                          const QStyleOptionViewItem& theOption,
                          const QModelIndex&          theIndex) const override
    {
        // Refusing here, rather than relying on edit triggers, also covers
        // Tab: the view walks to the next cell of an editable row and asks
        // for an editor whatever column it landed on.
        if (!theIndex.data(kEditableRole).toBool()) {
            return nullptr;
        }
        QWidget* editor = QStyledItemDelegate::createEditor(theParent, theOption, theIndex);
        if (editor != nullptr) {
            m_editors.emplace_back(editor);
        }
        return editor;
    }

    void destroyEditor(QWidget* theEditor, const QModelIndex& theIndex) const override
    {
        // Forgotten now, not when the deleteLater below lands: the refresh
        // waiting on this editor may run before the deferred delete does.
        m_editors.erase(std::remove_if(m_editors.begin(), m_editors.end(),
                                       [theEditor](const QPointer<QWidget>& theOpen) {
                                           return theOpen.isNull() || theOpen.data() == theEditor;
                                       }),
                        m_editors.end());
        QStyledItemDelegate::destroyEditor(theEditor, theIndex);
        if (m_onEditorClosed) {
            m_onEditorClosed();
        }
    }

    bool IsEditing() const
    {
        for (const QPointer<QWidget>& editor : m_editors) {
            if (!editor.isNull()) {
                return true;
            }
        }
        return false;
    }

private:
    mutable std::vector<QPointer<QWidget>> m_editors;
    std::function<void()>                  m_onEditorClosed;
};

ParametersDialog::ParametersDialog(Document* theDocument, QWidget* theParent)
    : QDialog(theParent)
    , m_document(theDocument)
{
    setWindowTitle(QStringLiteral("Parameters"));
    setMinimumSize(760, 420);
    resize(960, 480);

    auto* addButton = new QToolButton(this);
    addButton->setText(QStringLiteral("+"));
    addButton->setToolTip(QStringLiteral("Add a user parameter"));
    addButton->setEnabled(m_document != nullptr);
    connect(addButton, &QToolButton::clicked, this, [this]() { AddUserParameter(); });

    auto* toolRow = new QHBoxLayout();
    toolRow->addWidget(addButton);
    toolRow->addStretch(1);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(kColumnCount);
    m_tree->setHeaderLabels({QStringLiteral("Parameter"), QStringLiteral("Name"),
                             QStringLiteral("Unit"), QStringLiteral("Expression"),
                             QStringLiteral("Value"), QStringLiteral("Comments")});
    m_tree->setUniformRowHeights(true);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    // No AnyKeyPressed: a stray keystroke on a selected row should not
    // start overwriting an expression.
    m_tree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::SelectedClicked
                            | QAbstractItemView::EditKeyPressed);
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->resizeSection(kParameterColumn, 230);
    m_tree->header()->resizeSection(kNameColumn, 110);
    m_tree->header()->resizeSection(kUnitColumn, 55);
    m_tree->header()->resizeSection(kExpressionColumn, 180);
    m_tree->header()->resizeSection(kValueColumn, 110);

    m_delegate = new ParameterCellDelegate(m_tree);
    m_tree->setItemDelegate(m_delegate);
    // A refresh that came due while an editor was open runs once it closes.
    // Queued rather than run here: this fires from inside the editor's own
    // key or focus handling, the same trap as a commit.
    m_delegate->SetEditorClosedCallback([this]() {
        if (m_refreshAfterEdit) {
            ScheduleRefresh();
        }
    });

    // WidgetShortcut, so Delete only means "delete this parameter" while
    // the tree itself has focus -- inside a cell editor it deletes a
    // character, as it should.
    m_deleteAction = new QAction(QStringLiteral("Delete"), m_tree);
    m_deleteAction->setShortcut(QKeySequence(Qt::Key_Delete));
    m_deleteAction->setShortcutContext(Qt::WidgetShortcut);
    m_tree->addAction(m_deleteAction);
    connect(m_deleteAction, &QAction::triggered, this, [this]() { DeleteCurrentParameter(); });

    connect(m_tree, &QTreeWidget::itemChanged, this, &ParametersDialog::OnItemChanged);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this,
            &ParametersDialog::OnContextMenu);

    // Fusion's inline warnings sit at the foot of the dialog, beside the
    // button, rather than in a message box the user has to dismiss before
    // fixing the cell that caused it.
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    TintError(m_statusLabel);

    // Never the default button. A cell editor lets Return through
    // unconsumed -- the delegate commits on a queued call -- so only the
    // tree's own key handling stands between that Return and a default OK
    // closing the dialog in the middle of the commit. Not worth leaning on.
    auto* okButton = new QPushButton(QStringLiteral("OK"), this);
    okButton->setAutoDefault(false);
    okButton->setDefault(false);
    connect(okButton, &QPushButton::clicked, this, &QDialog::accept);

    auto* bottomRow = new QHBoxLayout();
    bottomRow->addWidget(m_statusLabel, 1);
    bottomRow->addWidget(okButton, 0, Qt::AlignBottom);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(toolRow);
    layout->addWidget(m_tree, 1);
    layout->addLayout(bottomRow);

    if (m_document != nullptr) {
        m_document->AddObserver(this);
    }
    Refresh();
}

ParametersDialog::~ParametersDialog()
{
    // The tree, and any editor still open in it, outlives this body: Qt
    // deletes children only after the members the callback reads are gone.
    m_delegate->SetEditorClosedCallback(nullptr);
    // Same trap from the other side: ~QDialog hides a dialog still on
    // screen, the open editor loses focus and commits, and itemChanged
    // would land in OnItemChanged on an object whose own destructor has
    // already finished.
    disconnect(m_tree, nullptr, this, nullptr);
    if (m_document != nullptr) {
        m_document->RemoveObserver(this);
    }
}

void ParametersDialog::OnDocumentChanged(Document& /*theDocument*/)
{
    // Most of the time this arrives from inside OnItemChanged: commit ->
    // mutator -> Rebuild -> NotifyChanged -> here, with the edited item
    // still in the middle of its own setData() and its editor still open.
    // Rebuilding the rows now would delete both under their own feet, so
    // only queue it. An Undo from elsewhere lands here too and gets the
    // same treatment, harmlessly.
    ScheduleRefresh();
}

void ParametersDialog::ScheduleRefresh()
{
    if (m_refreshQueued) {
        return;
    }
    m_refreshQueued = true;
    // `this` as context: the timer dies with the dialog.
    QTimer::singleShot(0, this, [this]() {
        m_refreshQueued = false;
        Refresh();
    });
}

void ParametersDialog::Refresh()
{
    // Tab out of one cell can land straight in the next cell's editor
    // before this runs. Clearing the tree would close that editor and
    // throw away what the user is typing; wait for it to close instead.
    if (m_delegate->IsEditing()) {
        m_refreshAfterEdit = true;
        return;
    }
    // Belt and braces for the trap OnDocumentChanged describes: if anything
    // under a mutator ever spins an event loop, this timer could fire with
    // a commit still on the stack. OnItemChanged queues a fresh refresh on
    // its way out, so returning loses nothing.
    if (m_isCommitting) {
        return;
    }
    m_refreshAfterEdit = false;

    const QString selected =
        m_pendingSelection.isEmpty() ? KeyOf(m_tree->currentItem()) : m_pendingSelection;
    m_pendingSelection.clear();
    const QSet<QString> collapsed = CollapsedKeys();
    const int scroll = m_tree->verticalScrollBar()->value();

    // Every setText below emits itemChanged, which would arrive at
    // OnItemChanged looking exactly like the user typing it.
    QSignalBlocker blocker(m_tree);
    m_tree->clear();

    const QBrush errorBrush(ErrorTextColor(m_tree->palette()));
    QTreeWidgetItem* toSelect = nullptr;
    std::vector<QTreeWidgetItem*> expandable;

    // ---- User Parameters ----
    QTreeWidgetItem* userGroup =
        MakeGroupRow(m_tree, QStringLiteral("User Parameters"), kUserGroupKey);
    expandable.push_back(userGroup);

    if (m_document != nullptr) {
        for (const UserParameter& parameter : m_document->UserParameters().Parameters()) {
            auto* row = new QTreeWidgetItem(userGroup);
            row->setData(kParameterColumn, kKindRole, static_cast<int>(RowKind::UserParameter));
            row->setData(kParameterColumn, kKeyRole, UserKey(parameter.name));
            row->setData(kParameterColumn, kNameRole, QString::fromStdString(parameter.name));

            row->setText(kParameterColumn, QStringLiteral("User Parameter"));
            row->setText(kNameColumn, QString::fromStdString(parameter.name));
            row->setText(kUnitColumn, UserUnitSymbol(parameter));
            row->setText(kExpressionColumn, QString::fromStdString(parameter.expression));
            row->setText(kCommentsColumn, QString::fromStdString(parameter.comment));
            if (parameter.isValid) {
                row->setText(kValueColumn, QString::fromStdString(parameter.DisplayText()));
            } else {
                // DisplayText() is empty for a row that did not resolve; a
                // blank cell would read as "not computed yet".
                row->setText(kValueColumn, QStringLiteral("Error"));
                row->setToolTip(kValueColumn, QString::fromStdString(parameter.error));
                row->setForeground(kValueColumn, errorBrush);
            }

            row->setFlags(row->flags() | Qt::ItemIsEditable);
            row->setData(kNameColumn, kEditableRole, true);
            row->setData(kExpressionColumn, kEditableRole, true);
            row->setData(kCommentsColumn, kEditableRole, true);

            if (KeyOf(row) == selected) {
                toSelect = row;
            }
        }
    }

    // ---- Model Parameters ----
    QTreeWidgetItem* modelGroup =
        MakeGroupRow(m_tree, QStringLiteral("Model Parameters"), kModelGroupKey);
    expandable.push_back(modelGroup);

    if (m_document != nullptr) {
        const std::vector<FeaturePtr>& features = m_document->Features();
        for (std::size_t i = 0; i < features.size(); ++i) {
            const FeaturePtr& feature = features[i];
            if (!feature) {
                continue;
            }
            std::vector<Parameter> numbers;
            for (const Parameter& parameter : feature->EditableParameters()) {
                if (parameter.type == Parameter::Type::Double) {
                    numbers.push_back(parameter);
                }
            }
            if (numbers.empty()) {
                continue;
            }

            const QString featureName = QString::fromStdString(feature->Name());
            auto* featureRow = new QTreeWidgetItem(modelGroup);
            featureRow->setText(kParameterColumn, featureName);
            featureRow->setData(kParameterColumn, kKindRole, static_cast<int>(RowKind::Feature));
            featureRow->setData(kParameterColumn, kKeyRole, FeatureKey(feature->Name()));
            featureRow->setFirstColumnSpanned(true);
            if (!feature->LastError().empty()) {
                // Coloured to match the tooltip's own palette, which is not
                // the tree's and can be dark on a light desktop.
                const QString color = ErrorTextColor(QToolTip::palette()).name();
                featureRow->setToolTip(
                    kParameterColumn,
                    QStringLiteral("<span style=\"color:%1\">%2</span>")
                        .arg(color, QString::fromStdString(feature->LastError()).toHtmlEscaped()));
                featureRow->setForeground(kParameterColumn, errorBrush);
            }
            expandable.push_back(featureRow);
            if (KeyOf(featureRow) == selected) {
                toSelect = featureRow;
            }

            for (const Parameter& parameter : numbers) {
                const UnitKind kind = parameter.Kind();
                auto* row = new QTreeWidgetItem(featureRow);
                row->setData(kParameterColumn, kKindRole, static_cast<int>(RowKind::ModelParameter));
                row->setData(kParameterColumn, kKeyRole, ModelKey(feature->Name(), parameter.name));
                row->setData(kParameterColumn, kNameRole, QString::fromStdString(parameter.name));
                row->setData(kParameterColumn, kFeatureRole, featureName);
                row->setData(kParameterColumn, kFeatureIndexRole, static_cast<int>(i));

                row->setText(kParameterColumn, QString::fromStdString(parameter.name));
                if (IsDimensionLabel(parameter.name)) {
                    row->setText(kNameColumn, QString::fromStdString(parameter.name));
                }
                row->setText(kUnitColumn, ModelUnitSymbol(kind));
                row->setText(kExpressionColumn, QString::fromStdString(ShownExpression(parameter)));
                row->setText(kValueColumn,
                             QString::fromStdString(FormatValue(parameter.doubleValue, kind)));

                row->setFlags(row->flags() | Qt::ItemIsEditable);
                row->setData(kExpressionColumn, kEditableRole, true);

                if (KeyOf(row) == selected) {
                    toSelect = row;
                }
            }
        }
    }

    for (QTreeWidgetItem* row : expandable) {
        row->setExpanded(!collapsed.contains(KeyOf(row)));
        if (KeyOf(row) == selected) {
            toSelect = row;
        }
    }
    if (toSelect != nullptr) {
        m_tree->setCurrentItem(toSelect);
    }

    // The view lays its rows out lazily, so until it does the scroll bar
    // still has the empty tree's range and would clamp the old position
    // to zero.
    m_tree->doItemsLayout();
    m_tree->verticalScrollBar()->setValue(scroll);
}

QSet<QString> ParametersDialog::CollapsedKeys() const
{
    // Collapsed rather than expanded, so a row that is new since the last
    // refresh -- a feature just added -- opens expanded like the rest.
    // Only rows with children count: Qt does not reliably keep an empty
    // row expanded, and remembering the empty User Parameters group as
    // "collapsed" would hide the first parameter the moment it was added.
    QSet<QString> collapsed;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        const QTreeWidgetItem* group = m_tree->topLevelItem(i);
        if (group->childCount() > 0 && !group->isExpanded()) {
            collapsed.insert(KeyOf(group));
        }
        for (int j = 0; j < group->childCount(); ++j) {
            const QTreeWidgetItem* child = group->child(j);
            if (child->childCount() > 0 && !child->isExpanded()) {
                collapsed.insert(KeyOf(child));
            }
        }
    }
    return collapsed;
}

void ParametersDialog::OnItemChanged(QTreeWidgetItem* theItem, int theColumn)
{
    if (m_document == nullptr || theItem == nullptr) {
        return;
    }

    // Everything the commit needs is copied off the item here. The mutator
    // below rebuilds the model and notifies observers before returning,
    // and a refused edit is put right by the same deferred refresh that
    // shows an accepted one. theItem itself stays alive throughout --
    // only Refresh() deletes rows, and it will not run while
    // m_isCommitting is set -- but its contents are stale from here on.
    const RowKind kind = KindOf(theItem);
    const QString text = theItem->text(theColumn);
    const QScopedValueRollback<bool> committing(m_isCommitting, true);

    if (kind == RowKind::UserParameter) {
        const std::string name = theItem->data(kParameterColumn, kNameRole).toString().toStdString();
        const std::string current = CommitUserParameter(name, theColumn, text);
        if (current != name) {
            // Tab from a renamed cell opens the next row's editor at once,
            // and the refresh that would re-key this row waits for that
            // editor to close. Shift+Tab straight back and a second rename
            // would go looking for the old name, find nothing and be
            // dropped without a word -- so re-key the row now.
            const QSignalBlocker blocker(m_tree);
            theItem->setData(kParameterColumn, kNameRole, QString::fromStdString(current));
            theItem->setData(kParameterColumn, kKeyRole, UserKey(current));
        }
    } else if (kind == RowKind::ModelParameter && theColumn == kExpressionColumn) {
        CommitModelParameter(theItem->data(kParameterColumn, kFeatureRole).toString().toStdString(),
                             theItem->data(kParameterColumn, kFeatureIndexRole).toInt(),
                             theItem->data(kParameterColumn, kNameRole).toString().toStdString(),
                             text);
    }

    // Always, even for a no-op: the cell may be showing something the
    // model does not hold, such as a name with stray spaces around it.
    ScheduleRefresh();
}

std::string ParametersDialog::CommitUserParameter(const std::string& theName, int theColumn,
                                                  const QString& theText)
{
    const UserParameter* current = m_document->UserParameters().Find(theName);
    if (current == nullptr) {
        // Silently doing nothing would look exactly like the edit landing
        // until the refresh put the old text back.
        ShowError(QString::fromStdString(theName) + QStringLiteral(" no longer exists"));
        return theName;
    }
    const std::string text = theText.trimmed().toStdString();
    std::string error;

    switch (theColumn) {
        case kNameColumn:
            if (text == theName) {
                return theName;
            }
            if (!m_document->RenameUserParameter(theName, text, error)) {
                ShowError(QStringLiteral("Cannot rename %1: %2")
                              .arg(QString::fromStdString(theName),
                                   QString::fromStdString(error)));
                return theName;
            }
            m_pendingSelection = UserKey(text);
            ClearStatus();
            return text;
        case kExpressionColumn:
            if (text == current->expression) {
                return theName;
            }
            if (!m_document->SetUserParameterExpression(theName, text, error)) {
                ShowError(QString::fromStdString(theName) + QStringLiteral(": ")
                          + QString::fromStdString(error));
                return theName;
            }
            break;
        case kCommentsColumn:
            if (text == current->comment) {
                return theName;
            }
            if (!m_document->SetUserParameterComment(theName, text, error)) {
                ShowError(QString::fromStdString(theName) + QStringLiteral(": ")
                          + QString::fromStdString(error));
                return theName;
            }
            break;
        default:
            return theName;
    }
    // `current` may dangle from here on: the mutator rewrote the table.
    ClearStatus();
    return theName;
}

void ParametersDialog::CommitModelParameter(const std::string& theFeatureName, int theFeatureIndex,
                                            const std::string& theParameterName,
                                            const QString&     theText)
{
    const QString where =
        QString::fromStdString(theFeatureName) + QLatin1Char(' ')
        + QString::fromStdString(theParameterName);

    FeaturePtr feature = FindFeature(theFeatureName, theFeatureIndex);
    if (!feature) {
        ShowError(QString::fromStdString(theFeatureName)
                  + QStringLiteral(" is no longer in the timeline"));
        return;
    }

    // Start from what the feature holds now, not from what the row was
    // built from: only the one field the user typed should change.
    Parameter edited;
    bool found = false;
    for (const Parameter& parameter : feature->EditableParameters()) {
        if (parameter.type == Parameter::Type::Double && parameter.name == theParameterName) {
            edited = parameter;
            found = true;
            break;
        }
    }
    if (!found) {
        ShowError(where + QStringLiteral(" no longer exists"));
        return;
    }

    const std::string text = theText.trimmed().toStdString();
    if (text.empty()) {
        ShowError(where + QStringLiteral(" needs a value or an expression"));
        return;
    }
    if (text == ShownExpression(edited)) {
        return;
    }

    // A plain value -- "25", "25 mm", "1/2 in" -- makes the parameter a
    // plain number again, even if an expression drove it until now.
    // Anything else is an expression, and the document decides whether it
    // evaluates.
    const ParsedValue parsed = ParseValue(text, edited.Kind());
    if (parsed.ok) {
        if (edited.expression.empty() && parsed.value == edited.doubleValue) {
            return;   // "20" over "20 mm": the same number, not worth an undo step
        }
        edited.doubleValue = parsed.value;
        edited.expression.clear();
    } else {
        edited.expression = text;
    }

    std::string error;
    if (!m_document->SetFeatureParameter(feature, edited, error)) {
        ShowError(where + QStringLiteral(": ") + QString::fromStdString(error));
        return;
    }
    ClearStatus();
}

FeaturePtr ParametersDialog::FindFeature(const std::string& theName, int theIndex) const
{
    if (m_document == nullptr) {
        return nullptr;
    }
    const std::vector<FeaturePtr>& features = m_document->Features();
    if (theIndex >= 0 && static_cast<std::size_t>(theIndex) < features.size()) {
        const FeaturePtr& hinted = features[static_cast<std::size_t>(theIndex)];
        if (hinted && hinted->Name() == theName) {
            return hinted;
        }
    }
    for (const FeaturePtr& feature : features) {
        if (feature && feature->Name() == theName) {
            return feature;
        }
    }
    return nullptr;
}

void ParametersDialog::OnContextMenu(const QPoint& thePos)
{
    QTreeWidgetItem* item = m_tree->itemAt(thePos);
    if (item == nullptr || KindOf(item) != RowKind::UserParameter) {
        return;   // model parameters belong to their features; Fusion offers nothing here
    }
    m_tree->setCurrentItem(item);

    // The menu runs its own event loop, in which a queued refresh can
    // rebuild the tree and delete `item`. The action reads the current
    // row afresh when it fires -- the refresh restores it by key -- so
    // nothing here outlives the exec().
    QMenu menu(this);
    menu.addAction(m_deleteAction);
    menu.exec(m_tree->viewport()->mapToGlobal(thePos));
}

void ParametersDialog::DeleteCurrentParameter()
{
    QTreeWidgetItem* item = m_tree->currentItem();
    if (m_document == nullptr || item == nullptr || KindOf(item) != RowKind::UserParameter) {
        return;
    }
    const QString name = item->data(kParameterColumn, kNameRole).toString();
    const QString neighbour = NeighbourKey(item);
    const std::string stdName = name.toStdString();

    // The document would allow it -- features that read the parameter
    // just fail on the next rebuild -- but Fusion refuses a parameter in
    // use, and says who is using it, so the user can see what to change
    // first.
    const std::vector<std::string> users = m_document->UsersOfUserParameter(stdName);
    if (!users.empty()) {
        QStringList names;
        for (const std::string& user : users) {
            names << QString::fromStdString(user);
        }
        ShowError(QStringLiteral("%1 is used by %2 and cannot be deleted")
                      .arg(name, names.join(QStringLiteral(", "))));
        return;
    }

    std::string error;
    if (!m_document->RemoveUserParameter(stdName, error)) {
        ShowError(Sentence(error, "that parameter could not be deleted"));
        return;
    }
    m_pendingSelection = neighbour;
    ClearStatus();
}

void ParametersDialog::AddUserParameter()
{
    if (m_document == nullptr) {
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add User Parameter"));

    auto* nameEdit =
        new QLineEdit(QString::fromStdString(FirstFreeName(m_document->UserParameters())), &dialog);
    auto* unitCombo = new QComboBox(&dialog);
    for (const UnitChoice& choice : kUnitChoices) {
        unitCombo->addItem(QString::fromLatin1(choice.label));
    }
    unitCombo->setCurrentIndex(DefaultUnitChoice());
    auto* expressionEdit = new QLineEdit(QStringLiteral("1"), &dialog);
    auto* commentEdit = new QLineEdit(&dialog);

    auto* form = new QFormLayout();
    form->addRow(QStringLiteral("Name"), nameEdit);
    form->addRow(QStringLiteral("Unit"), unitCombo);
    form->addRow(QStringLiteral("Expression"), expressionEdit);
    form->addRow(QStringLiteral("Comment"), commentEdit);

    auto* errorLabel = new QLabel(&dialog);
    errorLabel->setWordWrap(true);
    TintError(errorLabel);
    errorLabel->hide();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);

    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(errorLabel);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    // Not wired to accept(): a refused name or a cycle keeps the dialog
    // open with the reason in it, so the user fixes one field instead of
    // retyping all four.
    connect(buttons, &QDialogButtonBox::accepted, &dialog,
            [this, &dialog, nameEdit, unitCombo, expressionEdit, commentEdit, errorLabel]() {
                UserParameter parameter;
                parameter.name = nameEdit->text().trimmed().toStdString();
                parameter.expression = expressionEdit->text().trimmed().toStdString();
                parameter.comment = commentEdit->text().trimmed().toStdString();
                int choice = unitCombo->currentIndex();
                if (choice < 0 || choice >= kUnitChoiceCount) {
                    choice = 0;
                }
                parameter.kind = kUnitChoices[choice].kind;
                parameter.lengthUnit = kUnitChoices[choice].lengthUnit;
                parameter.angleUnit = kUnitChoices[choice].angleUnit;

                std::string error;
                if (!m_document->AddUserParameter(parameter, error)) {
                    errorLabel->setText(Sentence(error, "that parameter could not be added"));
                    errorLabel->show();
                    return;
                }
                // Set before accept(): the refresh this add queued may run
                // inside this dialog's own event loop on the way out.
                m_pendingSelection = UserKey(parameter.name);
                ClearStatus();
                dialog.accept();
            });

    nameEdit->selectAll();
    nameEdit->setFocus();
    dialog.exec();
}

void ParametersDialog::ShowError(const QString& theText)
{
    m_statusLabel->setText(theText);
}

void ParametersDialog::ClearStatus()
{
    m_statusLabel->clear();
}

} // namespace lcad
