#include "aiws/gui/main_window.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>

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

class MainWindowTests final : public QObject {
    Q_OBJECT

private slots:
    void document_api_rejects_unreadable_file();
    void stale_completion_is_not_published();
    void build_failure_is_reported();
};

void MainWindowTests::document_api_rejects_unreadable_file() {
    aiws::gui::MainWindow window;
    QVERIFY(!window.add_document_file(QStringLiteral("missing-m4-document.txt")));
}

void MainWindowTests::stale_completion_is_not_published() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("document.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("responsive corpus document\n");
    file.close();

    aiws::gui::MainWindow window;
    window.set_build_delay_ms(200);
    QVERIFY(window.add_document_file(path));
    QPushButton* build = named_button(window, QStringLiteral("Build Corpus"));
    QVERIFY(build);
    build->click();
    QVERIFY(wait_for_label(window, QStringLiteral("Building corpus..."), 1000));
    const QString other_path = directory.filePath(QStringLiteral("other.txt"));
    QFile other(other_path);
    QVERIFY(other.open(QIODevice::WriteOnly | QIODevice::Text));
    other.write("second document\n");
    other.close();
    QVERIFY(window.add_document_file(other_path));
    QVERIFY(wait_for_label(window, QStringLiteral("Corpus changed"), 5000));
}

void MainWindowTests::build_failure_is_reported() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("failure.txt"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("duplicate document\n");
    file.close();

    aiws::gui::MainWindow window;
    QVERIFY(window.add_document_file(path));
    QVERIFY(window.add_document_file(path));
    QPushButton* build = named_button(window, QStringLiteral("Build Corpus"));
    QVERIFY(build);
    build->click();
    QVERIFY(wait_for_label(window, QStringLiteral("Corpus build failed."), 5000));
}

}  // namespace

QTEST_MAIN(MainWindowTests)
#include "student_tests.moc"
