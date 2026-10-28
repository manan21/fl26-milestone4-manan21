#include "aiws/gui/workspace_widget.hpp"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace aiws::gui {

WorkspaceWidget::WorkspaceWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);

    auto* workspace_group = new QGroupBox(tr("Workspace"), this);
    auto* workspace_layout = new QVBoxLayout(workspace_group);
    document_list_ = new QListWidget(workspace_group);
    add_document_button_ = new QPushButton(tr("Add Document"), workspace_group);
    build_corpus_button_ = new QPushButton(tr("Build Corpus"), workspace_group);
    corpus_status_label_ = new QLabel(tr("Corpus: Not built"), workspace_group);
    workspace_layout->addWidget(document_list_, 1);
    workspace_layout->addWidget(add_document_button_);
    workspace_layout->addWidget(build_corpus_button_);
    workspace_layout->addWidget(corpus_status_label_);

    auto* document_group = new QGroupBox(tr("Selected Document"), this);
    auto* document_layout = new QVBoxLayout(document_group);
    document_title_label_ = new QLabel(document_group);
    document_title_label_->setWordWrap(true);
    document_text_ = new QPlainTextEdit(document_group);
    document_text_->setReadOnly(true);
    document_layout->addWidget(document_title_label_);
    document_layout->addWidget(document_text_, 1);

    layout->addWidget(workspace_group, 1);
    layout->addWidget(document_group, 2);

    connect(add_document_button_, &QPushButton::clicked,
            this, &WorkspaceWidget::add_document_requested);
    connect(build_corpus_button_, &QPushButton::clicked,
            this, &WorkspaceWidget::build_corpus_requested);
    connect(document_list_, &QListWidget::currentRowChanged,
            this, &WorkspaceWidget::document_selected);
    clear_selected_document();
    set_build_corpus_enabled(false);
}

void WorkspaceWidget::add_document(const QString& title) {
    document_list_->addItem(title);
}

void WorkspaceWidget::select_document(int row) {
    if (row >= 0 && row < document_list_->count()) {
        document_list_->setCurrentRow(row);
    }
}

void WorkspaceWidget::show_selected_document(const QString& title,
                                              const QString& text) {
    document_title_label_->setText(title);
    document_text_->setPlainText(text);
}

void WorkspaceWidget::clear_selected_document() {
    document_title_label_->clear();
    document_text_->clear();
}

void WorkspaceWidget::set_corpus_status(const QString& text) {
    corpus_status_label_->setText(text);
}

void WorkspaceWidget::set_build_corpus_enabled(bool enabled) {
    build_corpus_button_->setEnabled(enabled);
}

void WorkspaceWidget::reset() {
    document_list_->clear();
    clear_selected_document();
    set_corpus_status(tr("Corpus: Not built"));
    set_build_corpus_enabled(false);
}

}  // namespace aiws::gui
