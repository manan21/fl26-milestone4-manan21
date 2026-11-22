#include "aiws/gui/main_window.hpp"

#include "aiws/gui/context_widget.hpp"
#include "aiws/gui/search_widget.hpp"
#include "aiws/gui/term_inspector_widget.hpp"
#include "aiws/gui/workspace_widget.hpp"
#include "aiws/chunker.hpp"
#include "aiws/context_builder.hpp"
#include "aiws/retrieval_engine.hpp"

#include <QByteArray>
#include <QCloseEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <thread>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace aiws::gui {

namespace {

class BuildCancelled final : public std::exception {};

class DelayedChunker final : public ChunkingStrategy {
public:
    DelayedChunker(std::function<void(int)> wait_for_step, int delay_ms)
        : chunker_(std::make_unique<Chunker>()),
          wait_for_step_(std::move(wait_for_step)),
          delay_ms_(delay_ms) {}

    std::vector<Chunk> chunk(const Document& document,
                             std::size_t document_order) const override {
        wait_for_step_(0);
        auto chunks = chunker_->chunk(document, document_order);
        wait_for_step_(delay_ms_);
        return chunks;
    }

private:
    std::unique_ptr<Chunker> chunker_;
    std::function<void(int)> wait_for_step_;
    int delay_ms_;
};

bool receive_exact(int socket_fd, char* data, std::size_t length,
                   const std::atomic<bool>& stopping) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (length > 0) {
        if (stopping.load()) {
            return false;
        }
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            return false;
        }
        pollfd ready{socket_fd, POLLIN, 0};
        const int timeout = static_cast<int>(
            std::min<std::int64_t>(remaining.count(), 50));
        const int polled = poll(&ready, 1, timeout);
        if (polled == 0) {
            continue;
        }
        if (polled < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        const ssize_t count = recv(socket_fd, data, length, 0);
        if (count > 0) {
            data += count;
            length -= static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool send_all(int socket_fd, const char* data, std::size_t length) {
    while (length > 0) {
        const ssize_t count = send(socket_fd, data, length, MSG_NOSIGNAL);
        if (count > 0) {
            data += count;
            length -= static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

struct BuildCompletion {
    std::uint64_t request_id;
    std::uint64_t revision;
    std::shared_ptr<const ProcessingCore> core;
    std::string error;
};

class CorpusState {
public:
    virtual ~CorpusState() = default;
    virtual void build_requested(MainWindow& window) = 0;
    virtual void workspace_will_change(MainWindow& window) = 0;
    virtual void workspace_changed(MainWindow& window) = 0;
    virtual void reset_requested(MainWindow& window) = 0;
    virtual void build_finished(
        MainWindow& window,
        const std::shared_ptr<const BuildCompletion>& completion) = 0;
    virtual void exit_requested(MainWindow& window) = 0;
    virtual bool is_building() const noexcept = 0;
    virtual bool is_current() const noexcept = 0;
};

class EmptyCorpusState final : public CorpusState {
public:
    void build_requested(MainWindow&) override {}
    void workspace_will_change(MainWindow& window) override;
    void workspace_changed(MainWindow& window) override;
    void reset_requested(MainWindow& window) override;
    void build_finished(
        MainWindow&,
        const std::shared_ptr<const BuildCompletion>&) override {}
    void exit_requested(MainWindow& window) override;
    bool is_building() const noexcept override { return false; }
    bool is_current() const noexcept override { return false; }
};

class NeedsBuildCorpusState final : public CorpusState {
public:
    void build_requested(MainWindow& window) override { window.begin_build(); }
    void workspace_will_change(MainWindow& window) override;
    void workspace_changed(MainWindow& window) override;
    void reset_requested(MainWindow& window) override;
    void build_finished(
        MainWindow&,
        const std::shared_ptr<const BuildCompletion>&) override {}
    void exit_requested(MainWindow& window) override;
    bool is_building() const noexcept override { return false; }
    bool is_current() const noexcept override { return false; }
};

class BuildingCorpusState final : public CorpusState {
public:
    BuildingCorpusState(std::uint64_t request_id, std::uint64_t revision)
        : request_id_(request_id), revision_(revision) {}

    void build_requested(MainWindow& window) override { window.begin_build(); }
    void workspace_will_change(MainWindow& window) override;
    void workspace_changed(MainWindow& window) override;
    void reset_requested(MainWindow& window) override;
    void build_finished(
        MainWindow& window,
        const std::shared_ptr<const BuildCompletion>& completion) override;
    void exit_requested(MainWindow& window) override;
    bool is_building() const noexcept override { return true; }
    bool is_current() const noexcept override { return false; }

private:
    std::uint64_t request_id_;
    std::uint64_t revision_;
};

class CurrentCorpusState final : public CorpusState {
public:
    void build_requested(MainWindow& window) override { window.begin_build(); }
    void workspace_will_change(MainWindow& window) override;
    void workspace_changed(MainWindow& window) override;
    void reset_requested(MainWindow& window) override;
    void build_finished(
        MainWindow&,
        const std::shared_ptr<const BuildCompletion>&) override {}
    void exit_requested(MainWindow& window) override;
    bool is_building() const noexcept override { return false; }
    bool is_current() const noexcept override { return true; }
};

class StoppedCorpusState final : public CorpusState {
public:
    void build_requested(MainWindow&) override {}
    void workspace_will_change(MainWindow&) override {}
    void workspace_changed(MainWindow&) override {}
    void reset_requested(MainWindow&) override {}
    void build_finished(
        MainWindow&,
        const std::shared_ptr<const BuildCompletion>&) override {}
    void exit_requested(MainWindow&) override {}
    bool is_building() const noexcept override { return false; }
    bool is_current() const noexcept override { return false; }
};

void EmptyCorpusState::workspace_will_change(MainWindow& window) {
    window.cancel_build();
}

void EmptyCorpusState::workspace_changed(MainWindow& window) {
    window.invalidate_workspace();
    if (window.workspace_.documents().empty()) {
        window.transition_to(std::make_shared<EmptyCorpusState>());
    } else {
        window.transition_to(std::make_shared<NeedsBuildCorpusState>());
    }
}

void EmptyCorpusState::reset_requested(MainWindow& window) {
    window.reset_state();
}

void EmptyCorpusState::exit_requested(MainWindow& window) {
    window.stop_worker();
    window.transition_to(std::make_shared<StoppedCorpusState>());
}

void NeedsBuildCorpusState::workspace_will_change(MainWindow& window) {
    window.cancel_build();
}

void NeedsBuildCorpusState::workspace_changed(MainWindow& window) {
    window.invalidate_workspace();
    if (window.workspace_.documents().empty()) {
        window.transition_to(std::make_shared<EmptyCorpusState>());
    } else {
        window.transition_to(std::make_shared<NeedsBuildCorpusState>());
    }
}

void NeedsBuildCorpusState::reset_requested(MainWindow& window) {
    window.reset_state();
}

void NeedsBuildCorpusState::exit_requested(MainWindow& window) {
    window.stop_worker();
    window.transition_to(std::make_shared<StoppedCorpusState>());
}

void BuildingCorpusState::workspace_will_change(MainWindow& window) {
    window.cancel_build();
}

void BuildingCorpusState::workspace_changed(MainWindow& window) {
    window.invalidate_workspace();
    if (window.workspace_.documents().empty()) {
        window.transition_to(std::make_shared<EmptyCorpusState>());
    } else {
        window.transition_to(std::make_shared<NeedsBuildCorpusState>());
    }
}

void BuildingCorpusState::reset_requested(MainWindow& window) {
    window.reset_state();
}

void BuildingCorpusState::build_finished(
    MainWindow& window,
    const std::shared_ptr<const BuildCompletion>& completion) {
    if (completion->request_id != request_id_ ||
        completion->revision != revision_) {
        return;
    }
    if (completion->core) {
        window.publish_build(completion->core);
        window.transition_to(std::make_shared<CurrentCorpusState>());
    } else {
        window.report_build_failure(completion->error);
        window.transition_to(std::make_shared<NeedsBuildCorpusState>());
    }
}

void BuildingCorpusState::exit_requested(MainWindow& window) {
    window.stop_worker();
    window.transition_to(std::make_shared<StoppedCorpusState>());
}

void CurrentCorpusState::workspace_will_change(MainWindow& window) {
    window.cancel_build();
}

void CurrentCorpusState::workspace_changed(MainWindow& window) {
    window.invalidate_workspace();
    if (window.workspace_.documents().empty()) {
        window.transition_to(std::make_shared<EmptyCorpusState>());
    } else {
        window.transition_to(std::make_shared<NeedsBuildCorpusState>());
    }
}

void CurrentCorpusState::reset_requested(MainWindow& window) {
    window.reset_state();
}

void CurrentCorpusState::exit_requested(MainWindow& window) {
    window.stop_worker();
    window.transition_to(std::make_shared<StoppedCorpusState>());
}

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

    corpus_state_ = std::make_shared<EmptyCorpusState>();
    update_action_states();
    build_worker_ = std::thread([this] { worker_loop(); });
}

MainWindow::~MainWindow() {
    stop_ipc_server();
    auto state = corpus_state_;
    state->exit_requested(*this);
}

void MainWindow::set_exit_callback(ExitCallback callback) noexcept {
    exit_callback_ = callback;
}

void MainWindow::set_build_delay_ms(int ms_per_document) noexcept {
    build_delay_ms_ = ms_per_document >= 0 ? ms_per_document : 0;
}

bool MainWindow::start_ipc_server(const QString& socket_path) {
    if (ipc_thread_.joinable()) {
        return false;
    }
    sockaddr_un address{};
    const QByteArray encoded_path = socket_path.toUtf8();
    if (encoded_path.isEmpty() ||
        static_cast<std::size_t>(encoded_path.size()) >=
            sizeof(address.sun_path)) {
        return false;
    }

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, encoded_path.constData(),
                static_cast<std::size_t>(encoded_path.size()));
    const std::string path(encoded_path.constData(),
                           static_cast<std::size_t>(encoded_path.size()));

    const int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return false;
    }
    unlink(path.c_str());
    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) < 0 ||
        listen(listen_fd, 16) < 0) {
        const int error = errno;
        ::close(listen_fd);
        unlink(path.c_str());
        errno = error;
        return false;
    }

    ipc_stop_.store(false);
    ipc_socket_path_ = socket_path;
    try {
        ipc_thread_ = std::thread([this, listen_fd, path] {
            pollfd poll_fd{listen_fd, POLLIN, 0};
            while (!ipc_stop_.load()) {
                const int ready = poll(&poll_fd, 1, 50);
                if (ready == 0) {
                    continue;
                }
                if (ready < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }

                sockaddr_storage peer{};
                socklen_t peer_length = sizeof(peer);
                const int client = accept4(
                    listen_fd, reinterpret_cast<sockaddr*>(&peer),
                    &peer_length, SOCK_CLOEXEC);
                if (client < 0) {
                    if (errno == EINTR || errno == EAGAIN ||
                        errno == EWOULDBLOCK) {
                        continue;
                    }
                    break;
                }

                ipc_client_socket_.store(client);
                std::string response = "ERR invalid request";
                std::uint32_t length_network = 0;
                if (!ipc_stop_.load() &&
                    receive_exact(client,
                                  reinterpret_cast<char*>(&length_network),
                                  sizeof(length_network), ipc_stop_)) {
                    const std::uint32_t length = ntohl(length_network);
                    if (length >= 4 && length <= 16u * 1024u * 1024u) {
                        std::string payload(length, '\0');
                        if (receive_exact(client, payload.data(), length,
                                          ipc_stop_) &&
                            payload.compare(0, 4, "ADD ", 4) == 0) {
                            const QString remote_path = QString::fromUtf8(
                                payload.data() + 4,
                                static_cast<int>(length - 4));
                            if (!remote_path.isEmpty() &&
                                QFileInfo(remote_path).isAbsolute()) {
                                QFile file(remote_path);
                                if (!file.open(QIODevice::ReadOnly)) {
                                    response = "ERR unreadable file";
                                } else {
                                    file.close();
                                    const bool queued = QMetaObject::invokeMethod(
                                        this, "add_remote_document",
                                        Qt::QueuedConnection,
                                        Q_ARG(QString, remote_path));
                                    response = queued ? "OK"
                                                      : "ERR application unavailable";
                                }
                            }
                        }
                    }
                }
                if (!ipc_stop_.load()) {
                    send_all(client, response.data(), response.size());
                }
                ipc_client_socket_.store(-1);
                ::close(client);
            }
            ::close(listen_fd);
            unlink(path.c_str());
        });
    } catch (const std::system_error&) {
        ipc_socket_path_.clear();
        ipc_stop_.store(true);
        ::close(listen_fd);
        unlink(path.c_str());
        return false;
    }
    return true;
}

void MainWindow::stop_ipc_server() {
    ipc_stop_.store(true);
    const int client = ipc_client_socket_.load();
    if (client >= 0) {
        shutdown(client, SHUT_RDWR);
    }
    if (ipc_thread_.joinable()) {
        ipc_thread_.join();
    }
    if (!ipc_socket_path_.isEmpty()) {
        unlink(ipc_socket_path_.toStdString().c_str());
        ipc_socket_path_.clear();
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

    std::optional<aiws::Document> document;
    try {
        document.emplace(file_info.absoluteFilePath().toStdString(),
                         file_info.fileName().toStdString(),
                         QString::fromUtf8(contents).toStdString());
    } catch (const std::exception&) {
        return false;
    }

    auto state = corpus_state_;
    state->workspace_will_change(*this);
    try {
        workspace_.add_document(std::move(*document));
    } catch (const std::exception&) {
        state->workspace_changed(*this);
        return false;
    }

    workspace_widget_->add_document(file_info.fileName());
    ++workspace_revision_;
    state->workspace_changed(*this);
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
    auto state = corpus_state_;
    state->build_requested(*this);
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
    if (!corpus_state_->is_current() || query.trimmed().isEmpty()) {
        return;
    }

    try {
        auto results = processing_core_->search(query.toStdString(), k);
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
    if (!corpus_state_->is_current() || !search_widget_->has_query()) {
        return;
    }

    try {
        context_widget_->set_context(processing_core_->build_context(
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
    if (!corpus_state_->is_current() || effective_term.isEmpty()) {
        return;
    }

    try {
        const std::size_t document_frequency =
            processing_core_->document_frequency(effective_term.toStdString());
        const bool has_chunk_frequency = !chunk_id.isEmpty();
        const std::size_t chunk_frequency =
            has_chunk_frequency
                ? processing_core_->term_frequency(effective_term.toStdString(),
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
    auto state = corpus_state_;
    state->reset_requested(*this);
    workspace_.clear();
    ++workspace_revision_;
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
    stop_ipc_server();
    auto state = corpus_state_;
    state->exit_requested(*this);
    QMainWindow::closeEvent(event);
}

void MainWindow::worker_loop() {
    while (true) {
        BuildRequest request;
        {
            std::unique_lock<std::mutex> lock(build_mutex_);
            build_condition_.wait(lock, [this] {
                return worker_stopping_ || pending_build_.has_value();
            });
            if (worker_stopping_) {
                return;
            }
            request = std::move(*pending_build_);
            pending_build_.reset();
            if (request.id != build_generation_) {
                continue;
            }
            worker_busy_ = true;
        }

        std::shared_ptr<const ProcessingCore> core;
        std::string error;
        try {
            auto next_core = std::make_shared<ProcessingCore>(
                std::make_unique<DelayedChunker>(
                    [this, request_id = request.id](int delay_ms) {
                        wait_for_build_step(request_id, delay_ms);
                    },
                    request.delay_ms),
                std::make_unique<RetrievalEngine>(),
                std::make_unique<ContextBuilder>());
            next_core->rebuild(request.snapshot);
            core = std::move(next_core);
        } catch (const BuildCancelled&) {
            std::lock_guard<std::mutex> lock(build_mutex_);
            worker_busy_ = false;
            continue;
        } catch (const std::exception& exception) {
            error = exception.what();
        }

        bool still_current = false;
        {
            std::lock_guard<std::mutex> lock(build_mutex_);
            worker_busy_ = false;
            still_current = !worker_stopping_ &&
                            request.id == build_generation_;
        }
        if (!still_current) {
            continue;
        }

        auto completion = std::make_shared<const BuildCompletion>(
            BuildCompletion{request.id, request.revision, std::move(core),
                            std::move(error)});
        QMetaObject::invokeMethod(
            this,
            [this, completion] { handle_build_completion(completion); },
            Qt::QueuedConnection);
    }
}

void MainWindow::stop_worker() {
#ifdef AIWS_FAULT_SILENT_SHUTDOWN
    bool worker_was_busy = false;
#endif
    {
        std::lock_guard<std::mutex> lock(build_mutex_);
#ifdef AIWS_FAULT_SILENT_SHUTDOWN
        worker_was_busy = worker_busy_;
#endif
        worker_stopping_ = true;
        ++build_generation_;
        pending_build_.reset();
    }
#ifdef AIWS_FAULT_SILENT_SHUTDOWN
    if (worker_was_busy) {
        build_condition_.notify_all();
    }
#else
    build_condition_.notify_all();
#endif
    if (build_worker_.joinable()) {
        build_worker_.join();
    }
}

void MainWindow::wait_for_build_step(std::uint64_t request_id, int delay_ms) {
    std::unique_lock<std::mutex> lock(build_mutex_);
    const auto cancelled = [this, request_id] {
        if (worker_stopping_) {
            return true;
        }
#ifdef AIWS_FAULT_NO_CANCEL
        return false;
#else
        return request_id != build_generation_;
#endif
    };
    if (cancelled()) {
        throw BuildCancelled();
    }
    if (delay_ms > 0) {
        build_condition_.wait_for(lock, std::chrono::milliseconds(delay_ms),
                                  cancelled);
    }
    if (cancelled()) {
        throw BuildCancelled();
    }
}

std::uint64_t MainWindow::enqueue_build() {
    Workspace snapshot;
    for (const auto& document : workspace_.documents()) {
        snapshot.add_document(document);
    }
    std::lock_guard<std::mutex> lock(build_mutex_);
    if (worker_stopping_) {
        return 0;
    }
    const std::uint64_t request_id = ++build_generation_;
    pending_build_.emplace(
        BuildRequest{request_id, workspace_revision_, std::move(snapshot),
                     build_delay_ms_});
    build_condition_.notify_one();
    return request_id;
}

void MainWindow::cancel_build() {
    {
        std::lock_guard<std::mutex> lock(build_mutex_);
        ++build_generation_;
        pending_build_.reset();
    }
    build_condition_.notify_all();
}

void MainWindow::handle_build_completion(
    std::shared_ptr<const BuildCompletion> completion) {
    auto state = corpus_state_;
    state->build_finished(*this, completion);
}

void MainWindow::begin_build() {
    if (workspace_.documents().empty()) {
        return;
    }
    clear_processing_outputs();
    processing_core_.reset();
    workspace_widget_->set_corpus_status(tr("Corpus: Building..."));
    status_label_->setText(tr("Building corpus..."));
    const std::uint64_t request_id = enqueue_build();
    if (request_id != 0) {
        transition_to(std::make_shared<BuildingCorpusState>(
            request_id, workspace_revision_));
    }
}

void MainWindow::invalidate_workspace() {
    processing_core_.reset();
    clear_processing_outputs();
    workspace_widget_->set_corpus_status(
        workspace_.documents().empty() ? tr("Corpus: Not built")
                                       : tr("Corpus: Needs rebuild"));
}

void MainWindow::reset_state() {
    cancel_build();
    processing_core_.reset();
    clear_processing_outputs();
    workspace_widget_->set_corpus_status(tr("Corpus: Not built"));
    status_label_->setText(tr("Application reset."));
    transition_to(std::make_shared<EmptyCorpusState>());
}

void MainWindow::publish_build(std::shared_ptr<const ProcessingCore> core) {
    processing_core_ = std::move(core);
    workspace_widget_->set_corpus_status(
        tr("Corpus: %1 chunks")
            .arg(static_cast<qulonglong>(processing_core_->chunk_count())));
    status_label_->setText(tr("Corpus built."));
}

void MainWindow::report_build_failure(const std::string& error) {
    workspace_widget_->set_corpus_status(tr("Corpus: Needs rebuild"));
    QString message = tr("Corpus build failed.");
    if (!error.empty()) {
        message += QStringLiteral(" ") + QString::fromStdString(error);
    }
    status_label_->setText(message);
}

void MainWindow::transition_to(std::shared_ptr<CorpusState> state) {
    corpus_state_ = std::move(state);
    update_action_states();
}

void MainWindow::update_action_states() {
    const bool has_documents = !workspace_.documents().empty();
    const bool has_query = search_widget_->has_query();
    const bool corpus_current = corpus_state_->is_current();
    workspace_widget_->set_build_corpus_enabled(has_documents);
    search_widget_->set_search_enabled(corpus_current && has_query);
    term_inspector_widget_->set_inspection_enabled(corpus_current);
    context_widget_->set_build_context_enabled(corpus_current && has_query);
}

void MainWindow::clear_processing_outputs() {
    search_widget_->clear_results();
    term_inspector_widget_->set_selected_chunk(QString());
    term_inspector_widget_->clear_statistics();
    context_widget_->clear_context();
}

}  // namespace aiws::gui
