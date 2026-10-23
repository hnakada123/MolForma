#pragma once
#include <QByteArray>
#include <QObject>
class QSocketNotifier;
namespace mf {
class McpServer;
// Nonblocking POSIX streams; the Qt event loop remains responsive to GUI input.
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
    McpServer *server;
    QSocketNotifier *reader = nullptr, *writer = nullptr;
    QByteArray input, output;
    int inputFlags = -1, outputFlags = -1, exitCode = 0;
    bool eof = false, scheduled = false, stopped = false;
    void readInput();
    void dispatch();
    void writeOutput();
    void stop(const char *message);
};
} // namespace mf
