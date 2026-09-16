#include "ui/PropertiesPanel.h"
#include "ui/UiUtils.h"

#include <QCheckBox>
#include <QDoubleSpinBox>

#include "widgets/UnitLineEdit.h"
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace lcad {

namespace {
// Fallback range when a feature doesn't supply minimum/maximum hints
// (Parameter's own contract: they're ignored when min == max). Wide
// enough for any CAD dimension without being infinite.
constexpr double kDefaultDoubleRange = 1.0e6;
constexpr int    kDefaultIntRange    = 1000000;
} // namespace

PropertiesPanel::PropertiesPanel(Document* theDocument, QWidget* theParent)
    : QWidget(theParent)
    , m_document(theDocument)
{
    m_stack = new QStackedWidget(this);

    m_emptyPage = new QWidget(m_stack);
    {
        auto* layout = new QVBoxLayout(m_emptyPage);
        auto* label = new QLabel(QStringLiteral("No selection"), m_emptyPage);
        label->setAlignment(Qt::AlignCenter);
        QFont font = label->font();
        font.setItalic(true);
        label->setFont(font);
        layout->addStretch(1);
        layout->addWidget(label);
        layout->addStretch(1);
    }

    m_editorPage = new QWidget(m_stack);
    {
        auto* outer = new QVBoxLayout(m_editorPage);

        m_nameLabel = new QLabel(m_editorPage);
        QFont nameFont = m_nameLabel->font();
        nameFont.setBold(true);
        nameFont.setPointSize(nameFont.pointSize() + 1);
        m_nameLabel->setFont(nameFont);
        m_nameLabel->setWordWrap(true);

        m_typeLabel = new QLabel(m_editorPage);
        QFont typeFont = m_typeLabel->font();
        typeFont.setItalic(true);
        m_typeLabel->setFont(typeFont);

        m_errorLabel = new QLabel(m_editorPage);
        m_errorLabel->setWordWrap(true);
        m_errorLabel->hide();

        outer->addWidget(m_nameLabel);
        outer->addWidget(m_typeLabel);
        outer->addWidget(m_errorLabel);

        auto* rule = new QFrame(m_editorPage);
        rule->setFrameShape(QFrame::HLine);
        rule->setFrameShadow(QFrame::Sunken);
        outer->addWidget(rule);

        // The form lives in its own container inside a scroll area so a
        // feature with many parameters doesn't force the dock taller than
        // the window.
        m_formContainer = new QWidget();
        m_formLayout = new QFormLayout(m_formContainer);
        m_formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        m_formLayout->setRowWrapPolicy(QFormLayout::DontWrapRows);

        auto* scroll = new QScrollArea(m_editorPage);
        scroll->setWidget(m_formContainer);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        outer->addWidget(scroll, 1);
    }

    m_stack->addWidget(m_emptyPage);
    m_stack->addWidget(m_editorPage);
    m_stack->setCurrentWidget(m_emptyPage);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->addWidget(m_stack);

    if (m_document != nullptr) {
        m_document->AddObserver(this);
    }
    RebuildEditor();
}

PropertiesPanel::~PropertiesPanel()
{
    if (m_document != nullptr) {
        m_document->RemoveObserver(this);
    }
}

void PropertiesPanel::OnDocumentChanged(Document& /*theDocument*/)
{
    // Undo/redo, a browser delete, and a rollback past the active feature
    // can all change or clear Document::ActiveFeature() without ever
    // calling SetActiveFeature() -- always resync identity here instead of
    // trusting OnActiveFeatureChanged alone to have fired.
    if (m_document != nullptr && m_document->ActiveFeature() != m_activeFeature) {
        RebuildEditor();
        return;
    }
    RefreshHeader();
    RefreshValues();
}

void PropertiesPanel::OnActiveFeatureChanged(Document& /*theDocument*/)
{
    RebuildEditor();
}

void PropertiesPanel::RebuildEditor()
{
    QLayoutItem* child = nullptr;
    while ((child = m_formLayout->takeAt(0)) != nullptr) {
        delete child->widget();
        delete child;
    }
    m_rowWidgets.clear();
    m_lastParameters.clear();

    m_activeFeature = (m_document != nullptr) ? m_document->ActiveFeature() : nullptr;

    if (!m_activeFeature) {
        m_stack->setCurrentWidget(m_emptyPage);
        return;
    }

    m_stack->setCurrentWidget(m_editorPage);
    RefreshHeader();

    m_lastParameters = m_activeFeature->Parameters();
    for (std::size_t i = 0; i < m_lastParameters.size(); ++i) {
        QWidget* editor = MakeEditorWidget(i, m_lastParameters[i]);
        m_rowWidgets.push_back(editor);
        m_formLayout->addRow(QString::fromStdString(m_lastParameters[i].name), editor);
    }
}

void PropertiesPanel::RefreshHeader()
{
    if (!m_activeFeature) {
        return;
    }
    m_nameLabel->setText(QString::fromStdString(m_activeFeature->Name()));
    m_typeLabel->setText(QString::fromStdString(m_activeFeature->TypeName()));

    const std::string& error = m_activeFeature->LastError();
    if (error.empty()) {
        m_errorLabel->hide();
        m_errorLabel->clear();
        m_errorLabel->setPalette(QPalette());
    } else {
        m_errorLabel->setText(QString::fromStdString(error));
        QPalette pal;
        pal.setColor(QPalette::WindowText, ErrorTextColor(m_errorLabel->palette()));
        m_errorLabel->setPalette(pal);
        m_errorLabel->show();
    }
}

void PropertiesPanel::RefreshValues()
{
    if (!m_activeFeature) {
        return;
    }
    const std::vector<Parameter> current = m_activeFeature->Parameters();

    if (!SameShape(current)) {
        // No current feature changes its parameter set on the fly, but if
        // one ever does, this can be reached re-entrantly from inside the
        // very row widget's own signal handler (edit -> SetParameter ->
        // Rebuild -> NotifyChanged -> here). Rebuilding the form
        // synchronously would destroy that widget while it's still on the
        // call stack, so defer to the next event loop turn instead, same
        // as any other "delete this while its own signal is still being
        // handled" situation.
        QTimer::singleShot(0, this, [this]() { RebuildEditor(); });
        return;
    }

    for (std::size_t i = 0; i < current.size(); ++i) {
        const Parameter& parameter = current[i];
        QWidget* editor = m_rowWidgets[i];
        QSignalBlocker blocker(editor);
        switch (parameter.type) {
        case Parameter::Type::Double:
            if (auto* box = qobject_cast<UnitLineEdit*>(editor)) {
                box->SetValue(parameter.doubleValue);
            }
            break;
        case Parameter::Type::Int:
            if (auto* spin = qobject_cast<QSpinBox*>(editor)) {
                spin->setValue(parameter.intValue);
            }
            break;
        case Parameter::Type::Bool:
            if (auto* check = qobject_cast<QCheckBox*>(editor)) {
                check->setChecked(parameter.boolValue);
            }
            break;
        case Parameter::Type::String:
            if (auto* line = qobject_cast<QLineEdit*>(editor)) {
                line->setText(QString::fromStdString(parameter.stringValue));
            }
            break;
        }
    }
    m_lastParameters = current;
}

bool PropertiesPanel::SameShape(const std::vector<Parameter>& theParameters) const
{
    if (theParameters.size() != m_lastParameters.size()) {
        return false;
    }
    for (std::size_t i = 0; i < theParameters.size(); ++i) {
        if (theParameters[i].name != m_lastParameters[i].name
            || theParameters[i].type != m_lastParameters[i].type) {
            return false;
        }
    }
    return true;
}

QWidget* PropertiesPanel::MakeEditorWidget(std::size_t theIndex, const Parameter& theParameter)
{
    switch (theParameter.type) {
    case Parameter::Type::Double: {
        // The parameter's declared unit decides what kind of quantity this
        // is; the field then accepts any unit of that kind, so a radius can
        // be retyped as "1/4in" and will keep showing inches afterwards.
        const UnitKind kind =
            theParameter.unit == "deg"  ? UnitKind::Angle
            : theParameter.unit.empty() ? UnitKind::Unitless
                                        : UnitKind::Length;
        auto* spin = new UnitLineEdit(kind, m_formContainer);
        if (theParameter.minimum != theParameter.maximum) {
            spin->SetRange(theParameter.minimum, theParameter.maximum);
        }
        // Commits on Enter/focus-out rather than per keystroke, so typing
        // "-5" doesn't rebuild the model after every digit.
        spin->SetValue(theParameter.doubleValue);
        connect(spin, &UnitLineEdit::ValueChanged, this,
                [this, theIndex](double theValue) {
                    if (theIndex >= m_lastParameters.size()) {
                        return;
                    }
                    Parameter edited = m_lastParameters[theIndex];
                    edited.doubleValue = theValue;
                    CommitParameter(edited);
                });
        return spin;
    }
    case Parameter::Type::Int: {
        auto* spin = new QSpinBox(m_formContainer);
        spin->setKeyboardTracking(false);
        if (theParameter.minimum != theParameter.maximum) {
            spin->setRange(static_cast<int>(theParameter.minimum), static_cast<int>(theParameter.maximum));
        } else {
            spin->setRange(-kDefaultIntRange, kDefaultIntRange);
        }
        if (!theParameter.unit.empty()) {
            spin->setSuffix(" " + QString::fromStdString(theParameter.unit));
        }
        spin->setValue(theParameter.intValue);
        connect(spin, qOverload<int>(&QSpinBox::valueChanged), this,
                [this, theIndex](int theValue) {
                    if (theIndex >= m_lastParameters.size()) {
                        return;
                    }
                    Parameter edited = m_lastParameters[theIndex];
                    edited.intValue = theValue;
                    CommitParameter(edited);
                });
        return spin;
    }
    case Parameter::Type::Bool: {
        auto* check = new QCheckBox(m_formContainer);
        check->setChecked(theParameter.boolValue);
        connect(check, &QCheckBox::toggled, this,
                [this, theIndex](bool theValue) {
                    if (theIndex >= m_lastParameters.size()) {
                        return;
                    }
                    Parameter edited = m_lastParameters[theIndex];
                    edited.boolValue = theValue;
                    CommitParameter(edited);
                });
        return check;
    }
    case Parameter::Type::String:
    default: {
        auto* line = new QLineEdit(m_formContainer);
        line->setText(QString::fromStdString(theParameter.stringValue));
        // editingFinished (Enter/focus-out) rather than textChanged, so a
        // rename-in-progress doesn't rebuild after every character.
        connect(line, &QLineEdit::editingFinished, this,
                [this, theIndex, line]() {
                    if (theIndex >= m_lastParameters.size()) {
                        return;
                    }
                    Parameter edited = m_lastParameters[theIndex];
                    edited.stringValue = line->text().toStdString();
                    CommitParameter(edited);
                });
        return line;
    }
    }
}

void PropertiesPanel::CommitParameter(const Parameter& theEdited)
{
    if (!m_activeFeature || m_document == nullptr) {
        return;
    }
    if (m_activeFeature->SetParameter(theEdited)) {
        m_document->Rebuild();
    }
}

} // namespace lcad
