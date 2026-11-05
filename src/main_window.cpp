#include "aiws/gui/main_window.hpp"

#include "aiws/gui/context_widget.hpp"
#include "aiws/gui/search_widget.hpp"
#include "aiws/gui/term_inspector_widget.hpp"
#include "aiws/gui/workspace_widget.hpp"
#include "aiws/chunker.hpp"
#include "aiws/context_builder.hpp"
#include "aiws/retrieval_engine.hpp"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QCloseEvent>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <thread>
#include <stdexcept>
#include <string>
#include <utility>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace aiws::gui {

namespace {

class DelayedChunker final : public ChunkingStrategy {
public:
    DelayedChunker(std::shared_ptr<std::atomic<bool>> cancelled,
                   int delay_ms)
        : chunker_(std::make_unique<Chunker>()),
          cancelled_(std::move(cancelled)),
          delay_ms_(delay_ms) {}

    std::vector<Chunk> chunk(const Document& document,
                             std::size_t document_order) const override {
        auto chunks = chunker_->chunk(document, document_order);
        if (delay_ms_ <= 0 || cancelled_->load()) {
            return chunks;
        }

        const auto end =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(delay_ms_);
        while (!cancelled_->load() &&
               std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return chunks;
    }

private:
    std::unique_ptr<Chunker> chunker_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
    int delay_ms_;
};

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(tr("AI Workspace"));
    resize(900, 900);

    auto* central_widget = new QWidget(this);
    auto* main_layout = new QVBoxLayout(central_widget);
    auto* title = new QLabel(tr("AI Workspace"), central_widget);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(
        "QLabel { background-color: #263b5a; color: white; "
        "font-size: 20px; font-weight: bold; padding: 12px; }");
    main_layout->addWidget(title);

    workspace_widget_ = new WorkspaceWidget(central_widget);
    search_widget_ = new SearchWidget(central_widget);
    term_inspector_widget_ = new TermInspectorWidget(central_widget);
    context_widget_ = new ContextWidget(central_widget);

    auto* workspace_row = new QHBoxLayout;
    workspace_row->addWidget(workspace_widget_, 1);
    main_layout->addLayout(workspace_row, 3);
    main_layout->addWidget(search_widget_, 4);

    auto* processing_row = new QHBoxLayout;
    processing_row->addWidget(term_inspector_widget_, 1);
    processing_row->addWidget(context_widget_, 2);
    main_layout->addLayout(processing_row, 3);

    auto* footer = new QHBoxLayout;
    status_label_ = new QLabel(tr("Ready"), central_widget);
    status_label_->setWordWrap(true);
    reset_button_ = new QPushButton(tr("Reset"), central_widget);
    reset_button_->setStyleSheet(
        "QPushButton { background-color: #d8e7f5; color: #172b40; "
        "padding: 6px 14px; }");
    exit_button_ = new QPushButton(tr("Exit"), central_widget);
    exit_button_->setStyleSheet(
        "QPushButton { background-color: #8b3030; color: white; "
        "padding: 6px 14px; }");
    footer->addWidget(status_label_, 1);
    footer->addWidget(reset_button_);
    footer->addWidget(exit_button_);
    main_layout->addLayout(footer);
    setCentralWidget(central_widget);

    connect(workspace_widget_, &WorkspaceWidget::add_document_requested,
            this, &MainWindow::add_document);
    connect(workspace_widget_, &WorkspaceWidget::build_corpus_requested,
            this, &MainWindow::build_corpus);
    connect(workspace_widget_, &WorkspaceWidget::document_selected,
            this, &MainWindow::document_selected);
    connect(search_widget_, &SearchWidget::search_requested,
            this, &MainWindow::search_requested);
    connect(search_widget_, &SearchWidget::query_changed,
            this, &MainWindow::query_changed);
    connect(search_widget_, &SearchWidget::source_document_requested,
            this, &MainWindow::source_document_requested);
    connect(search_widget_, &SearchWidget::selected_chunk_changed,
            term_inspector_widget_, &TermInspectorWidget::set_selected_chunk);
    connect(term_inspector_widget_,
            &TermInspectorWidget::term_inspection_requested,
            this, &MainWindow::term_inspection_requested);
    connect(context_widget_, &ContextWidget::build_context_requested,
            this, &MainWindow::build_context_requested);
    connect(reset_button_, &QPushButton::clicked,
            this, &MainWindow::reset_application);
    connect(exit_button_, &QPushButton::clicked,
            this, &MainWindow::exit_application);

    build_timer_ = new QTimer(this);
    build_timer_->setInterval(25);
    connect(build_timer_, &QTimer::timeout,
            this, &MainWindow::poll_build_result);

    update_action_states();
}

MainWindow::~MainWindow() {
    stop_build();
    join_build();
    stop_ipc_server();
}

void MainWindow::set_exit_callback(ExitCallback callback) noexcept {
    exit_callback_ = callback;
}

void MainWindow::set_build_delay_ms(int ms_per_document) noexcept {
    build_delay_ms_ = ms_per_document >= 0 ? ms_per_document : 0;
}

bool MainWindow::start_ipc_server(const QString& socket_path) {
    ipc_socket_path_ = socket_path;
    ipc_stop_ = std::make_shared<std::atomic<bool>>(false);
    ipc_thread_ = std::thread([this, socket_path = socket_path.toStdString(),
                               stop = ipc_stop_] {
        const int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd < 0) return;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (socket_path.size() >= sizeof(address.sun_path)) {
            ::close(listen_fd);
            return;
        }
        std::memcpy(address.sun_path, socket_path.data(), socket_path.size());
        unlink(socket_path.c_str());
        if (bind(listen_fd, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) < 0 ||
            listen(listen_fd, 16) < 0) {
            ::close(listen_fd);
            return;
        }
        ipc_listen_socket_ = listen_fd;

        pollfd poll_fd{listen_fd, POLLIN, 0};
        while (!stop->load()) {
            if (poll(&poll_fd, 1, 50) <= 0) continue;
            sockaddr_storage unused{};
            socklen_t unused_length = sizeof(unused);
            const int client = accept4(listen_fd, reinterpret_cast<sockaddr*>(&unused),
                                       &unused_length, SOCK_CLOEXEC);
            if (client < 0) continue;

            std::uint32_t length_network = 0;
            const ssize_t header = recv(client, &length_network, 4, MSG_WAITALL);
            if (header == 4) {
                const std::uint32_t length =
                    __builtin_bswap32(length_network);
                std::string payload(length, '\0');
                const ssize_t payload_bytes =
                    length <= 16u * 1024u * 1024u
                        ? recv(client, payload.data(), length, MSG_WAITALL)
                        : -1;
                std::string response;
                if (payload_bytes >= 0 &&
                    static_cast<std::size_t>(payload_bytes) == length &&
                    length >= 4 && payload.compare(0, 4, "ADD ", 4) == 0) {
                    const QString path =
                        QString::fromUtf8(payload.data() + 4,
                                          static_cast<int>(length - 4));
                    QFile file(path);
                    if (!file.open(QIODevice::ReadOnly)) {
                        response = "ERR unreadable file";
                    } else {
                        file.close();
                        response = "OK";
                        QMetaObject::invokeMethod(
                            this, "add_remote_document",
                            Qt::QueuedConnection,
                            Q_ARG(QString, path));
                    }
                } else {
                    response = "ERR invalid request";
                }
                send(client, response.data(), response.size(), MSG_NOSIGNAL);
            }
            ::close(client);
        }
        ::close(listen_fd);
        ipc_listen_socket_ = -1;
        unlink(socket_path.c_str());
    });
    return true;
}

void MainWindow::stop_ipc_server() {
    if (ipc_stop_) ipc_stop_->store(true);
    if (ipc_listen_socket_ >= 0) shutdown(ipc_listen_socket_, SHUT_RDWR);
    if (ipc_thread_.joinable()) ipc_thread_.join();
    ipc_stop_.reset();
    ipc_listen_socket_ = -1;
    if (!ipc_socket_path_.isEmpty()) {
        unlink(ipc_socket_path_.toStdString().c_str());
    }
}

void MainWindow::add_remote_document(const QString& path) {
    (void)add_document_file(path);
}

bool MainWindow::add_document_file(const QString& path) {
    const QFileInfo file_info(path);
    QFile file(file_info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray contents = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return false;
    }

    try {
        workspace_.add_document(aiws::Document(
            file_info.absoluteFilePath().toStdString(),
            file_info.fileName().toStdString(),
            QString::fromUtf8(contents).toStdString()));
    } catch (...) {
        return false;
    }

    workspace_widget_->add_document(file_info.fileName());
    ++workspace_revision_;
    invalidate_corpus();
    status_label_->setText(tr("Document added. Build the corpus to process it."));
    update_action_states();
    return true;
}

void MainWindow::add_document() {
    const QString file_path = QFileDialog::getOpenFileName(
        this, tr("Add Text Document"), QString(),
        tr("Text files (*.txt);;All files (*)"));
    if (file_path.isEmpty()) {
        return;
    }

    if (!add_document_file(file_path)) {
        QMessageBox::warning(this, tr("Unable to Read Document"),
                             tr("The selected file could not be read."));
        return;
    }
}

void MainWindow::build_corpus() {
    if (workspace_.documents().empty() || build_state_) {
        return;
    }

    clear_processing_outputs();
    corpus_current_ = false;
    workspace_widget_->set_corpus_status(tr("Corpus: Building..."));
    status_label_->setText(tr("Building corpus..."));

    Workspace snapshot;
    for (const auto& document : workspace_.documents()) {
        snapshot.add_document(document);
    }

    build_state_ = std::make_unique<BuildState>();
    build_state_->revision = workspace_revision_;
    build_state_->cancelled = std::make_shared<std::atomic<bool>>(false);
    std::promise<BuildResult> promise;
    build_state_->result = promise.get_future();

    const int delay_ms = build_delay_ms_;
    auto cancelled = build_state_->cancelled;
    build_state_->worker = std::thread(
        [snapshot = std::move(snapshot), delay_ms, cancelled,
         promise = std::move(promise)]() mutable {
            BuildResult result;
            if (cancelled->load()) {
                promise.set_value(std::move(result));
                return;
            }
            try {
                auto core = std::make_unique<ProcessingCore>(
                    std::make_unique<DelayedChunker>(cancelled, delay_ms),
                    std::make_unique<RetrievalEngine>(),
                    std::make_unique<ContextBuilder>());
                core->rebuild(snapshot);
                result.core = std::move(core);
            } catch (const std::exception& error) {
                result.error = error.what();
            }
            promise.set_value(std::move(result));
        });
    build_timer_->start();
    update_action_states();
}

void MainWindow::document_selected(int row) {
    if (row < 0 || static_cast<std::size_t>(row) >= workspace_.documents().size()) {
        workspace_widget_->clear_selected_document();
        return;
    }

    const auto& document = workspace_.documents()[static_cast<std::size_t>(row)];
    workspace_widget_->show_selected_document(
        QString::fromStdString(document.title()),
        QString::fromStdString(document.text()));
}

void MainWindow::search_requested(const QString& query, int k) {
    if (!corpus_current_ || query.trimmed().isEmpty()) {
        return;
    }

    try {
        auto results = processing_core_.search(query.toStdString(), k);
        search_widget_->set_results(std::move(results));
        context_widget_->clear_context();
        status_label_->setText(tr("Search completed."));
    } catch (const std::exception& error) {
        QMessageBox::critical(this, tr("Search Failed"),
                              QString::fromLocal8Bit(error.what()));
        status_label_->setText(tr("Search failed."));
    }
    update_action_states();
}

void MainWindow::query_changed(const QString& query) {
    Q_UNUSED(query);
    update_action_states();
}

void MainWindow::build_context_requested(int token_budget) {
    if (!corpus_current_ || !search_widget_->has_query()) {
        return;
    }

    try {
        context_widget_->set_context(processing_core_.build_context(
            search_widget_->query().toStdString(),
            search_widget_->result_count(),
            static_cast<std::size_t>(token_budget)));
        status_label_->setText(tr("Context built."));
    } catch (const std::exception& error) {
        QMessageBox::critical(this, tr("Context Build Failed"),
                              QString::fromLocal8Bit(error.what()));
        status_label_->setText(tr("Context build failed."));
    }
}

void MainWindow::source_document_requested(const QString& document_id) {
    for (std::size_t row = 0; row < workspace_.documents().size(); ++row) {
        if (QString::fromStdString(workspace_.documents()[row].id()) ==
            document_id) {
            workspace_widget_->select_document(static_cast<int>(row));
            return;
        }
    }
    status_label_->setText(tr("The source document is no longer available."));
}

void MainWindow::term_inspection_requested(const QString& term,
                                           const QString& chunk_id) {
    const QString effective_term = term.trimmed();
    if (!corpus_current_ || effective_term.isEmpty()) {
        return;
    }

    try {
        const std::size_t document_frequency =
            processing_core_.document_frequency(effective_term.toStdString());
        const bool has_chunk_frequency = !chunk_id.isEmpty();
        const std::size_t chunk_frequency =
            has_chunk_frequency
                ? processing_core_.term_frequency(effective_term.toStdString(),
                                                  chunk_id.toStdString())
                : 0;
        term_inspector_widget_->set_statistics(
            document_frequency, has_chunk_frequency, chunk_frequency);
        status_label_->setText(tr("Term inspection completed."));
    } catch (const std::exception& error) {
        QMessageBox::critical(this, tr("Term Inspection Failed"),
                              QString::fromLocal8Bit(error.what()));
        status_label_->setText(tr("Term inspection failed."));
    }
}

void MainWindow::reset_application() {
    workspace_.clear();
    ++workspace_revision_;
    stop_build();
    corpus_current_ = false;
    Resettable* resettable_components[] = {
        workspace_widget_, search_widget_, term_inspector_widget_, context_widget_};
    for (Resettable* component : resettable_components) {
        component->reset();
    }
    status_label_->setText(tr("Application reset."));
    update_action_states();
}

void MainWindow::exit_application() {
    if (exit_callback_ != nullptr) {
        exit_callback_(this);
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
        stop_build();
        join_build();
        QMainWindow::closeEvent(event);
}

void MainWindow::stop_build() {
        if (build_state_) {
            build_state_->cancelled->store(true);
        }
}

void MainWindow::join_build() {
        if (build_state_ && build_state_->worker.joinable()) {
            build_state_->worker.join();
        }
        build_state_.reset();
        if (build_timer_ != nullptr) {
            build_timer_->stop();
        }
}

void MainWindow::poll_build_result() {
        if (!build_state_ || build_state_->worker.joinable() == false ||
            build_state_->result.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready) {
            return;
        }

        build_state_->worker.join();
        BuildResult result = build_state_->result.get();
        const std::uint64_t revision = build_state_->revision;
        build_state_.reset();
        build_timer_->stop();

    #ifdef AIWS_FAULT_ACCEPT_STALE
        const bool revision_matches = true;
    #else
        const bool revision_matches = revision == workspace_revision_;
    #endif

    #ifdef AIWS_FAULT_LOSE_FAILURE
        const bool report_failure = result.core != nullptr ||
                                    result.error.empty();
    #else
        const bool report_failure = true;
    #endif

        if (!revision_matches) {
            workspace_widget_->set_corpus_status(tr("Corpus: Needs rebuild"));
            status_label_->setText(tr("Corpus changed. Build again to update."));
        } else if (result.core == nullptr) {
            if (report_failure) {
                workspace_widget_->set_corpus_status(tr("Corpus: Needs rebuild"));
                status_label_->setText(tr("Corpus build failed."));
            }
        } else {
            processing_core_ = std::move(*result.core);
            corpus_current_ = true;
            workspace_widget_->set_corpus_status(
                tr("Corpus: %1 chunks")
                    .arg(static_cast<qulonglong>(processing_core_.chunk_count())));
            status_label_->setText(tr("Corpus built."));
        }

        update_action_states();
}

void MainWindow::update_action_states() {
    const bool has_documents = !workspace_.documents().empty();
    const bool has_query = search_widget_->has_query();
    workspace_widget_->set_build_corpus_enabled(has_documents && !build_state_);
    search_widget_->set_search_enabled(corpus_current_ && has_query);
    term_inspector_widget_->set_inspection_enabled(corpus_current_);
    context_widget_->set_build_context_enabled(corpus_current_ && has_query);
}

void MainWindow::invalidate_corpus() {
    corpus_current_ = false;
    workspace_widget_->set_corpus_status(tr("Corpus: Needs rebuild"));
    clear_processing_outputs();
    update_action_states();
}

void MainWindow::clear_processing_outputs() {
    search_widget_->clear_results();
    term_inspector_widget_->set_selected_chunk(QString());
    term_inspector_widget_->clear_statistics();
    context_widget_->clear_context();
}

}  // namespace aiws::gui
