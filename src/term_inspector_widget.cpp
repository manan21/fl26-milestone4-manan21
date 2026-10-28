#include "aiws/gui/term_inspector_widget.hpp"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace aiws::gui {

TermInspectorWidget::TermInspectorWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* group = new QGroupBox(tr("Term Inspector"), this);
    auto* group_layout = new QVBoxLayout(group);
    auto* term_row = new QHBoxLayout;
    term_edit_ = new QLineEdit(group);
    term_edit_->setPlaceholderText(tr("Enter a term"));
    inspect_button_ = new QPushButton(tr("Inspect"), group);
    term_row->addWidget(term_edit_, 1);
    term_row->addWidget(inspect_button_);

    auto* statistics = new QFormLayout;
    document_frequency_value_ = new QLabel(group);
    chunk_frequency_value_ = new QLabel(group);
    statistics->addRow(tr("Document frequency (DF):"),
                       document_frequency_value_);
    statistics->addRow(tr("Selected chunk TF:"),
                       chunk_frequency_value_);
    group_layout->addLayout(term_row);
    group_layout->addLayout(statistics);
    layout->addWidget(group);

    connect(inspect_button_, &QPushButton::clicked,
            this, &TermInspectorWidget::request_inspection);
    clear_statistics();
    set_inspection_enabled(false);
}

void TermInspectorWidget::set_inspection_enabled(bool enabled) {
    inspect_button_->setEnabled(enabled);
}

void TermInspectorWidget::set_selected_chunk(const QString& chunk_id) {
    if (selected_chunk_id_ != chunk_id) {
        selected_chunk_id_ = chunk_id;
        clear_statistics();
    }
}

void TermInspectorWidget::set_statistics(std::size_t document_frequency,
                                         bool has_chunk_frequency,
                                         std::size_t chunk_frequency) {
    document_frequency_value_->setText(
        QString::number(static_cast<qulonglong>(document_frequency)));
    chunk_frequency_value_->setText(
        has_chunk_frequency
            ? QString::number(static_cast<qulonglong>(chunk_frequency))
            : QString());
}

void TermInspectorWidget::clear_statistics() {
    document_frequency_value_->clear();
    chunk_frequency_value_->clear();
}

void TermInspectorWidget::reset() {
    term_edit_->clear();
    selected_chunk_id_.clear();
    clear_statistics();
    set_inspection_enabled(false);
}

void TermInspectorWidget::request_inspection() {
    const QString term = term_edit_->text().trimmed();
    if (!term.isEmpty() && inspect_button_->isEnabled()) {
        emit term_inspection_requested(term, selected_chunk_id_);
    }
}

}  // namespace aiws::gui
