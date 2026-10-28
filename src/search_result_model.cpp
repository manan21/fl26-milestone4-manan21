#include "aiws/gui/search_result_model.hpp"

#include <QString>

#include <utility>

namespace aiws::gui {

SearchResultModel::SearchResultModel(QObject* parent)
    : QAbstractTableModel(parent) {
}

int SearchResultModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(results_.size());
}

int SearchResultModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : 5;
}

QVariant SearchResultModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 ||
        static_cast<std::size_t>(index.row()) >= results_.size() ||
        index.column() < 0 || index.column() >= columnCount() ||
        role != Qt::DisplayRole) {
        return {};
    }

    const SearchResult& result = results_[static_cast<std::size_t>(index.row())];
    switch (index.column()) {
    case 0:
        return index.row() + 1;
    case 1:
        return QString::fromStdString(result.document_id);
    case 2:
        return static_cast<qulonglong>(result.chunk_sequence);
    case 3:
        return result.score;
    case 4:
        return QString::fromStdString(result.text);
    default:
        return {};
    }
}

QVariant SearchResultModel::headerData(int section,
                                       Qt::Orientation orientation,
                                       int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
        return {};
    }

    switch (section) {
    case 0:
        return tr("Rank");
    case 1:
        return tr("Document");
    case 2:
        return tr("Chunk");
    case 3:
        return tr("Score");
    case 4:
        return tr("Text");
    default:
        return {};
    }
}

void SearchResultModel::set_results(std::vector<SearchResult> results) {
    beginResetModel();
    results_ = std::move(results);
    endResetModel();
}

void SearchResultModel::clear() {
    set_results({});
}

const SearchResult* SearchResultModel::result_at(int row) const noexcept {
    if (row < 0 || static_cast<std::size_t>(row) >= results_.size()) {
        return nullptr;
    }
    return &results_[static_cast<std::size_t>(row)];
}

}  // namespace aiws::gui
