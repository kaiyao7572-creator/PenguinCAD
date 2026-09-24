#pragma once

#include "core/Document.h"
#include "core/Feature.h"

#include <QWidget>

#include <cstddef>
#include <vector>

class QFormLayout;
class QLabel;
class QStackedWidget;

namespace lcad {

// The right-hand dock: reads the active feature's Parameters() and builds
// one editor row per parameter (QDoubleSpinBox/QSpinBox/QCheckBox/
// QLineEdit). Editing a row calls Feature::SetParameter() then
// Document::Rebuild() -- this is what makes the app parametric from the
// user's point of view: change a fillet radius after the fact and the
// model updates on the spot.
class PropertiesPanel : public QWidget, public DocumentObserver
{
    Q_OBJECT

public:
    explicit PropertiesPanel(Document* theDocument, QWidget* theParent = nullptr);
    ~PropertiesPanel() override;

    // DocumentObserver
    void OnDocumentChanged(Document& theDocument) override;
    void OnActiveFeatureChanged(Document& theDocument) override;

private:
    // Full teardown + rebuild of the parameter rows for a (possibly new)
    // active feature. Only safe to call outside of a row widget's own
    // signal handler -- see RefreshValues().
    void RebuildEditor();

    // Pushes current name/type/error text into the header labels.
    void RefreshHeader();

    // Same active feature, same parameter shape: pushes current values
    // into the existing row widgets instead of destroying/recreating them.
    // This matters for correctness, not just polish -- a live edit calls
    // this synchronously from inside the very spin box's own valueChanged
    // handler (SetParameter -> Rebuild -> NotifyChanged -> here), and
    // destroying that widget mid-signal would be undefined behavior.
    void RefreshValues();

    bool SameShape(const std::vector<Parameter>& theParameters) const;
    QWidget* MakeEditorWidget(std::size_t theIndex, const Parameter& theParameter);
    void CommitParameter(const Parameter& theEdited);

    // Show theError under the header in the error colour, or hide it.
    void ShowError(const std::string& theError);

    Document*  m_document = nullptr;
    FeaturePtr m_activeFeature;               // identity the current rows were built for
    std::vector<Parameter> m_lastParameters;  // shape + values the rows currently show

    QStackedWidget* m_stack = nullptr;
    QWidget*        m_emptyPage = nullptr;
    QWidget*        m_editorPage = nullptr;
    QLabel*         m_nameLabel = nullptr;
    QLabel*         m_typeLabel = nullptr;
    QLabel*         m_errorLabel = nullptr;
    QWidget*        m_formContainer = nullptr;
    QFormLayout*    m_formLayout = nullptr;

    std::vector<QWidget*> m_rowWidgets;   // parallel to m_lastParameters
};

} // namespace lcad
