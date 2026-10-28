#include "aiws/gui/search_widget.hpp"

#include "aiws/gui/search_result_model.hpp"

#include <QAbstractItemView>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableView>
#include <QVBoxLayout>

#include <utility>

namespace aiws::gui {

SearchWidget::SearchWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* query_group = new QGroupBox(tr("Query"), this);
    auto* query_layout = new QHBoxLayout(query_group);
    query_edit_ = new QLineEdit(query_group);
    query_edit_->setPlaceholderText(tr("Enter search terms"));
    result_count_spin_ = new QSpinBox(query_group);
    result_count_spin_->setRange(1, 100);
    result_count_spin_->setValue(3);
    search_button_ = new QPushButton(tr("Search"), query_group);
    query_layout->addWidget(query_edit_, 1);
    query_layout->addWidget(new QLabel(tr("Results (k):"), query_group));
    query_layout->addWidget(result_count_spin_);
    query_layout->addWidget(search_button_);

    auto* results_group = new QGroupBox(tr("Search Results"), this);
    auto* results_layout = new QVBoxLayout(results_group);
    results_view_ = new QTableView(results_group);
    result_model_ = new SearchResultModel(results_view_);
    results_view_->setModel(result_model_);
    results_view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    results_view_->setSelectionMode(QAbstractItemView::SingleSelection);
    results_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results_view_->setWordWrap(true);
    results_view_->horizontalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    results_view_->horizontalHeader()->setSectionResizeMode(
        3, QHeaderView::Stretch);
    results_layout->addWidget(results_view_);

    auto* selected_group = new QGroupBox(tr("Selected Result"), this);
    auto* selected_layout = new QVBoxLayout(selected_group);
    selected_result_text_ = new QPlainTextEdit(selected_group);
    selected_result_text_->setReadOnly(true);
    view_source_button_ = new QPushButton(tr("View Source"), selected_group);
    selected_layout->addWidget(selected_result_text_);
    selected_layout->addWidget(view_source_button_, 0, Qt::AlignRight);

    layout->addWidget(query_group);
    layout->addWidget(results_group, 3);
    layout->addWidget(selected_group, 2);

    connect(query_edit_, &QLineEdit::textChanged,
            this, &SearchWidget::query_changed);
    connect(search_button_, &QPushButton::clicked,
            this, &SearchWidget::request_search);
    connect(results_view_->selectionModel(),
            &QItemSelectionModel::selectionChanged,
            this, &SearchWidget::show_selected_result);
    connect(view_source_button_, &QPushButton::clicked,
            this, &SearchWidget::request_source_document);

    clear_results();
    set_search_enabled(false);
}

QString SearchWidget::query() const {
    return query_edit_->text();
}

int SearchWidget::result_count() const {
    return result_count_spin_->value();
}

bool SearchWidget::has_query() const {
    return !query_edit_->text().trimmed().isEmpty();
}

void SearchWidget::set_search_enabled(bool enabled) {
    search_button_->setEnabled(enabled);
}

void SearchWidget::set_results(std::vector<SearchResult> results) {
    results_view_->clearSelection();
    result_model_->set_results(std::move(results));
    selected_result_text_->clear();
    view_source_button_->setEnabled(false);
    emit selected_chunk_changed(QString());
}

void SearchWidget::clear_results() {
    results_view_->clearSelection();
    result_model_->clear();
    selected_result_text_->clear();
    view_source_button_->setEnabled(false);
    emit selected_chunk_changed(QString());
}

void SearchWidget::reset() {
    query_edit_->clear();
    result_count_spin_->setValue(3);
    clear_results();
    set_search_enabled(false);
}

void SearchWidget::request_search() {
    if (has_query() && search_button_->isEnabled()) {
        emit search_requested(query_edit_->text(), result_count_spin_->value());
    }
}

void SearchWidget::show_selected_result() {
    const QModelIndex current = results_view_->currentIndex();
    const SearchResult* result = current.isValid()
                                     ? result_model_->result_at(current.row())
                                     : nullptr;
    if (result == nullptr) {
        selected_result_text_->clear();
        view_source_button_->setEnabled(false);
        emit selected_chunk_changed(QString());
        return;
    }

    selected_result_text_->setPlainText(
        tr("Rank: %1\nDocument: %2\nChunk: %3\nScore: %4\n\nText:\n%5")
            .arg(current.row() + 1)
            .arg(QString::fromStdString(result->document_id))
            .arg(static_cast<qulonglong>(result->chunk_sequence))
            .arg(result->score, 0, 'f', 4)
            .arg(QString::fromStdString(result->text)));
    view_source_button_->setEnabled(true);
    emit selected_chunk_changed(QString::fromStdString(result->chunk_id));
}

void SearchWidget::request_source_document() {
    const QModelIndex current = results_view_->currentIndex();
    const SearchResult* result = current.isValid()
                                     ? result_model_->result_at(current.row())
                                     : nullptr;
    if (result != nullptr) {
        emit source_document_requested(
            QString::fromStdString(result->document_id));
    }
}

}  // namespace aiws::gui
