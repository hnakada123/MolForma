// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QByteArray>
#include <QObject>
#include <memory>
class QSocketNotifier;
namespace mf {
class McpServer;
// Nonblocking POSIX streams, or reader/writer threads on Windows; the Qt event loop remains
// responsive to GUI input.
class McpStdioTransport : public QObject {
    Q_OBJECT
  public:
    static constexpr qsizetype MaxMessageBytes = 8 * 1024 * 1024;
    explicit McpStdioTransport(McpServer *server, QObject *parent = nullptr);
    ~McpStdioTransport() override;
    void start();
  signals:
    void finished(int exitCode);

  private:
    struct Threads;
    McpServer *server;
    QSocketNotifier *reader = nullptr, *writer = nullptr;
    std::shared_ptr<Threads> threads;
    QByteArray input, output;
    qsizetype unflushed = 0; // Handed to the Windows writer thread, not yet written.
    int inputFlags = -1, outputFlags = -1, exitCode = 0;
    bool eof = false, scheduled = false, stopped = false;
    void readInput();
    void dispatch();
    void writeOutput();
    void stop(const char *message);
};
} // namespace mf
