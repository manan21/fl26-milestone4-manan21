#include "aiws/gui/context_widget.hpp"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QStringList>
#include <QVBoxLayout>

namespace aiws::gui {

ContextWidget::ContextWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    auto* group = new QGroupBox(tr("Context"), this);
    auto* group_layout = new QVBoxLayout(group);
    auto* controls = new QHBoxLayout;
    context_budget_slider_ = new QSlider(Qt::Horizontal, group);
    context_budget_slider_->setRange(50, 1000);
    context_budget_slider_->setSingleStep(50);
    context_budget_slider_->setPageStep(50);
    context_budget_slider_->setValue(300);
    budget_value_label_ = new QLabel(group);
    build_context_button_ = new QPushButton(tr("Build Context"), group);
    controls->addWidget(new QLabel(tr("Token budget:"), group));
    controls->addWidget(context_budget_slider_, 1);
    controls->addWidget(budget_value_label_);
    controls->addWidget(build_context_button_);
    context_text_ = new QPlainTextEdit(group);
    context_text_->setReadOnly(true);
    group_layout->addLayout(controls);
    group_layout->addWidget(context_text_, 1);
    layout->addWidget(group);

    connect(build_context_button_, &QPushButton::clicked,
            this, &ContextWidget::request_context);
    connect(context_budget_slider_, &QSlider::valueChanged,
            this, &ContextWidget::budget_changed);
    budget_changed(context_budget_slider_->value());
    set_build_context_enabled(false);
}

int ContextWidget::token_budget() const {
    return context_budget_slider_->value();
}

void ContextWidget::set_build_context_enabled(bool enabled) {
    build_context_button_->setEnabled(enabled);
}

void ContextWidget::set_context(const std::vector<ContextItem>& items) {
    QStringList blocks;
    blocks.reserve(static_cast<qsizetype>(items.size()));
    for (const auto& item : items) {
        blocks.push_back(
            tr("Document: %1\nChunk: %2\nTokens: %3\nScore: %4\n"
               "Truncated: %5\nText:\n%6")
                .arg(QString::fromStdString(item.document_id))
                .arg(static_cast<qulonglong>(item.chunk_sequence))
                .arg(static_cast<qulonglong>(item.token_count))
                .arg(item.score, 0, 'f', 4)
                .arg(item.truncated ? tr("Yes") : tr("No"))
                .arg(QString::fromStdString(item.text)));
    }
    context_text_->setPlainText(blocks.join(QStringLiteral("\n\n")));
}

void ContextWidget::clear_context() {
    context_text_->clear();
}

void ContextWidget::reset() {
    context_budget_slider_->setValue(300);
    clear_context();
    set_build_context_enabled(false);
}

void ContextWidget::request_context() {
    if (build_context_button_->isEnabled()) {
        emit build_context_requested(token_budget());
    }
}

void ContextWidget::budget_changed(int value) {
    budget_value_label_->setText(QString::number(value));
}

}  // namespace aiws::gui
