#include "aiws/gui/main_window.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

QString window_label_text(aiws::gui::MainWindow& window,
                          const QString& contains) {
    const auto labels = window.findChildren<QLabel*>();
    for (const QLabel* label : labels) {
        const QString text = label->text();
        if (text.contains(contains, Qt::CaseInsensitive)) {
            return text;
        }
    }
    return {};
}

bool wait_for_label(aiws::gui::MainWindow& window,
                    const QString& contains,
                    int timeout_ms = 5000) {
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents();
        if (!window_label_text(window, contains).isEmpty()) {
            return true;
        }
        QTest::qWait(20);
    } while (timer.elapsed() < timeout_ms);
    return false;
}

bool wait_for_document_count(aiws::gui::MainWindow& window,
                             int expected_count,
                             int timeout_ms = 2000) {
    QElapsedTimer timer;
    timer.start();
    do {
        QCoreApplication::processEvents();
        const auto lists = window.findChildren<QListWidget*>();
        if (!lists.isEmpty() && lists.front()->count() == expected_count) {
            return true;
        }
        QTest::qWait(20);
    } while (timer.elapsed() < timeout_ms);
    return false;
}

QPushButton* named_button(aiws::gui::MainWindow& window,
                          const QString& text) {
    const auto buttons = window.findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

bool write_document(const QString& path, const QByteArray& contents) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    return file.write(contents) == contents.size();
}

class MainWindowTests final : public QObject {
    Q_OBJECT

private slots:
    void document_api_rejects_unreadable_file();
    void stale_completion_is_not_published();
    void build_failure_is_reported();
    void reset_cancels_old_build_before_next_build();
    void gui_remains_responsive_during_build();
    void repeated_builds_complete();
    void close_stops_active_build();
    void idle_shutdown_completes();
    void remote_add_handles_utf8_and_partial_writes();
    void stopping_server_unblocks_partial_request();
};

void MainWindowTests::document_api_rejects_unreadable_file() {
    aiws::gui::MainWindow window;
    QVERIFY(!window.add_document_file(QStringLiteral("missing-m4-document.txt")));
}

void MainWindowTests::stale_completion_is_not_published() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("document.txt"));
    QVERIFY(write_document(path, "responsive corpus document\n"));

    aiws::gui::MainWindow window;
    window.set_build_delay_ms(300);
    QVERIFY(window.add_document_file(path));
    QPushButton* build = named_button(window, QStringLiteral("Build Corpus"));
    QVERIFY(build);
    build->click();
    QVERIFY(wait_for_label(window, QStringLiteral("Building corpus..."), 1000));
    const QString other_path = directory.filePath(QStringLiteral("other.txt"));
    QVERIFY(write_document(other_path, "second document\n"));
    QVERIFY(window.add_document_file(other_path));
    QVERIFY(wait_for_label(window, QStringLiteral("Document added"), 1000));
    QTest::qWait(400);
    QVERIFY(window_label_text(window, QStringLiteral("Corpus: Needs rebuild"))
                .size() > 0);
    QVERIFY(window_label_text(window, QStringLiteral("Corpus built.")).isEmpty());
}

void MainWindowTests::build_failure_is_reported() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("failure.txt"));
    QVERIFY(write_document(path, "duplicate document\n"));

    aiws::gui::MainWindow window;
    QVERIFY(window.add_document_file(path));
    QVERIFY(window.add_document_file(path));
    QPushButton* build = named_button(window, QStringLiteral("Build Corpus"));
    QVERIFY(build);
    build->click();
    QVERIFY(wait_for_label(window, QStringLiteral("Corpus build failed."), 5000));
}

void MainWindowTests::reset_cancels_old_build_before_next_build() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    aiws::gui::MainWindow window;
    window.set_build_delay_ms(250);
    for (int index = 0; index < 7; ++index) {
        const QString path = directory.filePath(
            QStringLiteral("old-%1.txt").arg(index));
        QVERIFY(write_document(path, "old build document\n"));
        QVERIFY(window.add_document_file(path));
    }

    QPushButton* build = named_button(window, QStringLiteral("Build Corpus"));
    QPushButton* reset = named_button(window, QStringLiteral("Reset"));
    QVERIFY(build);
    QVERIFY(reset);
    build->click();
    QVERIFY(wait_for_label(window, QStringLiteral("Building corpus..."), 1000));
    QTest::qWait(80);

    reset->click();
    const QString new_path = directory.filePath(QStringLiteral("new.txt"));
    QVERIFY(write_document(new_path, "new build document\n"));
    QVERIFY(window.add_document_file(new_path));
    QElapsedTimer timer;
    timer.start();
    build->click();
    QVERIFY(wait_for_label(window, QStringLiteral("Corpus built."), 1100));
    QVERIFY(timer.elapsed() < 1100);
}

void MainWindowTests::gui_remains_responsive_during_build() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("responsive.txt"));
    QVERIFY(write_document(path, "responsive build document\n"));

    aiws::gui::MainWindow window;
    window.set_build_delay_ms(1500);
    QVERIFY(window.add_document_file(path));
    QPushButton* build = named_button(window, QStringLiteral("Build Corpus"));
    QPushButton* reset = named_button(window, QStringLiteral("Reset"));
    QVERIFY(build);
    QVERIFY(reset);
    build->click();
    QVERIFY(wait_for_label(window, QStringLiteral("Building corpus..."), 1000));

    QElapsedTimer timer;
    timer.start();
    reset->click();
    QVERIFY(timer.elapsed() < 500);
    QVERIFY(wait_for_label(window, QStringLiteral("Application reset."), 500));
}

void MainWindowTests::repeated_builds_complete() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("repeated.txt"));
    QVERIFY(write_document(path, "repeated build document\n"));

    aiws::gui::MainWindow window;
    window.set_build_delay_ms(50);
    QVERIFY(window.add_document_file(path));
    QPushButton* build = named_button(window, QStringLiteral("Build Corpus"));
    QVERIFY(build);

    for (int iteration = 0; iteration < 3; ++iteration) {
        build->click();
        QVERIFY(wait_for_label(window, QStringLiteral("Corpus built."), 2000));
    }
}

void MainWindowTests::close_stops_active_build() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("closing.txt"));
    QVERIFY(write_document(path, "closing build document\n"));

    aiws::gui::MainWindow window;
    window.set_build_delay_ms(1500);
    QVERIFY(window.add_document_file(path));
    QPushButton* build = named_button(window, QStringLiteral("Build Corpus"));
    QVERIFY(build);
    build->click();
    QVERIFY(wait_for_label(window, QStringLiteral("Building corpus..."), 1000));
    QTest::qWait(80);

    QElapsedTimer timer;
    timer.start();
    QVERIFY(window.close());
    QVERIFY(timer.elapsed() < 1000);
}

void MainWindowTests::idle_shutdown_completes() {
    QProcess probe;
    probe.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--idle-exit-probe")});
    QVERIFY(probe.waitForStarted(3000));
    if (!probe.waitForFinished(1200)) {
        probe.kill();
        probe.waitForFinished();
        QFAIL("closing an idle application did not stop its worker");
    }
    QCOMPARE(probe.exitStatus(), QProcess::NormalExit);
    QCOMPARE(probe.exitCode(), 0);
}

void MainWindowTests::remote_add_handles_utf8_and_partial_writes() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString socket_path = directory.filePath(QStringLiteral("ipc.sock"));
    const QString file_path =
        directory.filePath(QString::fromUtf8("with space\ncaf\xc3\xa9.txt"));
    QVERIFY(write_document(file_path, "remote document\n"));

    aiws::gui::MainWindow window;
    QVERIFY(window.start_ipc_server(socket_path));

    const int client = socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(client >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const QByteArray socket_bytes = socket_path.toUtf8();
    QVERIFY(static_cast<std::size_t>(socket_bytes.size()) <
            sizeof(address.sun_path));
    std::memcpy(address.sun_path, socket_bytes.constData(),
                static_cast<std::size_t>(socket_bytes.size()));
    QVERIFY(connect(client, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) == 0);

    const QByteArray payload = QByteArrayLiteral("ADD ") + file_path.toUtf8();
    const std::uint32_t length = htonl(
        static_cast<std::uint32_t>(payload.size()));
    const char* header = reinterpret_cast<const char*>(&length);
    for (int index = 0; index < 4; ++index) {
        QCOMPARE(send(client, header + index, 1, MSG_NOSIGNAL),
                 static_cast<ssize_t>(1));
        QTest::qWait(20);
    }
    for (qsizetype offset = 0; offset < payload.size(); offset += 3) {
        const qsizetype count =
            std::min<qsizetype>(3, payload.size() - offset);
        QCOMPARE(send(client, payload.constData() + offset,
                      static_cast<std::size_t>(count), MSG_NOSIGNAL),
                 static_cast<ssize_t>(count));
        QTest::qWait(20);
    }

    pollfd ready{client, POLLIN, 0};
    QCOMPARE(poll(&ready, 1, 2000), 1);
    char response[2] = {};
    QCOMPARE(recv(client, response, sizeof(response), 0),
             static_cast<ssize_t>(2));
    QCOMPARE(QByteArray(response, 2), QByteArrayLiteral("OK"));
    ::close(client);
    QVERIFY(wait_for_document_count(window, 1));

    const QString sender_path = QDir(QCoreApplication::applicationDirPath())
                                    .filePath(QStringLiteral("aiws_send"));
    QProcess sender;
    sender.start(sender_path,
                 {QStringLiteral("--socket"), socket_path,
                  QStringLiteral("add"), file_path});
    QVERIFY(sender.waitForFinished(3000));
    QCOMPARE(sender.exitStatus(), QProcess::NormalExit);
    QCOMPARE(sender.exitCode(), 0);
    QVERIFY(wait_for_document_count(window, 2));

    window.stop_ipc_server();
    QVERIFY(!QFileInfo::exists(socket_path));
}

void MainWindowTests::stopping_server_unblocks_partial_request() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString socket_path = directory.filePath(QStringLiteral("partial.sock"));

    aiws::gui::MainWindow window;
    QVERIFY(window.start_ipc_server(socket_path));
    const int client = socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(client >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const QByteArray socket_bytes = socket_path.toUtf8();
    std::memcpy(address.sun_path, socket_bytes.constData(),
                static_cast<std::size_t>(socket_bytes.size()));
    QVERIFY(connect(client, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) == 0);
    const std::uint32_t length = htonl(128);
    QCOMPARE(send(client, &length, sizeof(length), MSG_NOSIGNAL),
             static_cast<ssize_t>(sizeof(length)));
    QTest::qWait(50);

    QElapsedTimer timer;
    timer.start();
    window.stop_ipc_server();
    QVERIFY(timer.elapsed() < 500);
    QVERIFY(!QFileInfo::exists(socket_path));
    ::close(client);
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    if (application.arguments().contains(
            QStringLiteral("--idle-exit-probe"))) {
        aiws::gui::MainWindow window;
        QTest::qWait(100);
        window.close();
        return 0;
    }
    MainWindowTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "student_tests.moc"
