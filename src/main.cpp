#include "aiws/gui/main_window.hpp"

#include <QApplication>
#include <QMessageBox>
#include <QString>
#include <unistd.h>

namespace {

QString default_ipc_socket() {
    const QByteArray user = qgetenv("USER");
    if (!user.isEmpty()) {
        return QStringLiteral("/tmp/aiws-%1.sock").arg(QString::fromUtf8(user));
    }
    return QStringLiteral("/tmp/aiws-%1.sock").arg(QString::number(geteuid()));
}

void confirm_exit(QWidget* parent) {
    const auto answer = QMessageBox::question(
        parent,
        QObject::tr("Exit AI Workspace"),
        QObject::tr("Are you sure you want to exit?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (answer == QMessageBox::Yes) {
        QApplication::quit();
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);

    aiws::gui::MainWindow window;
    window.set_exit_callback(&confirm_exit);

    const QStringList arguments = application.arguments();
    QString ipc_socket = default_ipc_socket();
    const int ipc_index = arguments.indexOf(QStringLiteral("--ipc-socket"));
    if (ipc_index >= 0 && ipc_index + 1 < arguments.size()) {
        ipc_socket = arguments.at(ipc_index + 1);
    }
    const int delay_index = arguments.indexOf(QStringLiteral("--build-delay-ms"));
    if (delay_index >= 0 && delay_index + 1 < arguments.size()) {
        bool valid = false;
        const int delay_ms = arguments.at(delay_index + 1).toInt(&valid);
        if (valid && delay_ms >= 0) {
            window.set_build_delay_ms(delay_ms);
        }
    }
    if (!window.start_ipc_server(ipc_socket)) {
        return 1;
    }
    window.show();

    return application.exec();
}
