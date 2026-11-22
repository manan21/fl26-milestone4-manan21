#pragma once

#include <QMainWindow>

#include "aiws/processing_core.hpp"
#include "aiws/workspace.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

class QLabel;
class QPushButton;
class QString;
class QWidget;

namespace aiws::gui {

class ContextWidget;
class CorpusState;
class EmptyCorpusState;
class BuildingCorpusState;
class NeedsBuildCorpusState;
class CurrentCorpusState;
class StoppedCorpusState;
struct BuildCompletion;
class SearchWidget;
class TermInspectorWidget;
class WorkspaceWidget;

/**
 * @brief Main application window and coordinator for the AI Workspace GUI.
 *
 * MainWindow owns the application Workspace and ProcessingCore, composes the
 * major GUI components, and coordinates application-wide state changes and
 * backend operations in response to component events.
 */
class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    /**
     * @brief Function-pointer type used for the application Exit action.
     *
     * The callback receives the main window as a parent for any exit-related
     * interaction. In M3, the registered callback asks the user to confirm the
     * exit. Declining the confirmation leaves the running application and its
     * current state unchanged.
     */
    using ExitCallback = void (*)(QWidget* parent);

    /**
     * @brief Constructs the main AI Workspace application window.
     * @param parent Optional Qt parent widget.
     */
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /**
     * @brief Registers the callback invoked when the user requests Exit.
     * @param callback Function pointer to invoke for the Exit action. A null
     * callback means that an Exit request has no registered callback to invoke.
     */
    void set_exit_callback(ExitCallback callback) noexcept;

    /** Adds a readable text file using the same path as the Add Document action. */
    bool add_document_file(const QString& path);

    /** Sets the additional build delay applied per source document. */
    void set_build_delay_ms(int ms_per_document) noexcept;

    /** Starts the CR-1 Unix-domain socket server. */
    bool start_ipc_server(const QString& socket_path);

    /** Stops the CR-1 server and removes its socket file. */
    void stop_ipc_server();

private slots:
    /** @brief Handles a request to select and add a document to the workspace. */
    void add_document();

    /** Applies an already-validated remote document on the GUI thread. */
    void add_remote_document(const QString& path);

    /** @brief Rebuilds the processing corpus from the current non-empty workspace. */
    void build_corpus();

    /**
     * @brief Updates the selected-document display for the requested workspace row.
     * @param row Zero-based document row, or a negative value when no row is selected.
     */
    void document_selected(int row);

    /**
     * @brief Performs a search of the current corpus and presents the returned results.
     * @param query Query text to search for.
     * @param k Maximum number of results requested.
     */
    void search_requested(const QString& query, int k);

    /**
     * @brief Responds to a query edit by updating application action availability.
     * @param query Updated query text.
     */
    void query_changed(const QString& query);

    /**
     * @brief Builds and displays context for the current query and search settings.
     * @param token_budget Maximum token budget requested for the context.
     */
    void build_context_requested(int token_budget);
    /**
     * @brief Selects and displays the workspace document identified by a search result.
     * @param document_id Identifier of the source document to reveal.
     */
    void source_document_requested(const QString& document_id);

    /**
     * @brief Queries and presents term statistics for the current corpus.
     * @param term Term to inspect.
     * @param chunk_id Selected result chunk identifier; may be empty, in which
     * case document frequency is still available but chunk frequency is not.
     */
    void term_inspection_requested(const QString& term, const QString& chunk_id);

    /**
     * @brief Restores the application and its major GUI components to initial state.
     */
    void reset_application();

    /**
     * @brief Invokes the registered Exit callback for the current exit request.
     */
    void exit_application();

private:
    friend class EmptyCorpusState;
    friend class BuildingCorpusState;
    friend class NeedsBuildCorpusState;
    friend class CurrentCorpusState;
    friend class StoppedCorpusState;

    struct BuildRequest {
        std::uint64_t id;
        std::uint64_t revision;
        Workspace snapshot;
        int delay_ms;
    };

    void closeEvent(class QCloseEvent* event) override;
    void worker_loop();
    void stop_worker();
    void wait_for_build_step(std::uint64_t request_id, int delay_ms);
    std::uint64_t enqueue_build();
    void cancel_build();
    void handle_build_completion(
        std::shared_ptr<const BuildCompletion> completion);
    void begin_build();
    void invalidate_workspace();
    void reset_state();
    void publish_build(std::shared_ptr<const ProcessingCore> core);
    void report_build_failure(const std::string& error);
    void transition_to(std::shared_ptr<CorpusState> state);

    /**
     * @brief Updates availability of actions that depend on current application state.
     */
    void update_action_states();

    /**
     * @brief Clears displayed search and context output that depends on processing state.
     */
    void clear_processing_outputs();

    Workspace workspace_;
    std::shared_ptr<const ProcessingCore> processing_core_;
    std::uint64_t workspace_revision_{0};
    int build_delay_ms_{0};
    ExitCallback exit_callback_{nullptr};
    std::shared_ptr<CorpusState> corpus_state_;

    std::mutex build_mutex_;
    std::condition_variable build_condition_;
    std::optional<BuildRequest> pending_build_;
    std::uint64_t build_generation_{0};
    bool worker_busy_{false};
    bool worker_stopping_{false};
    std::thread build_worker_;

    std::thread ipc_thread_;
    std::atomic<bool> ipc_stop_{false};
    QString ipc_socket_path_;
    std::atomic<int> ipc_client_socket_{-1};

    WorkspaceWidget* workspace_widget_{nullptr};
    SearchWidget* search_widget_{nullptr};
    TermInspectorWidget* term_inspector_widget_{nullptr};
    ContextWidget* context_widget_{nullptr};
    QLabel* status_label_{nullptr};
    QPushButton* reset_button_{nullptr};
    QPushButton* exit_button_{nullptr};
};

}  // namespace aiws::gui
