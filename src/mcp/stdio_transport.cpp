#include "mcp/stdio_transport.h"
#include "mcp/server.h"
#include <QJsonDocument>
#include <QSocketNotifier>
#include <QTimer>
#include <cstdio>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mf {
McpStdioTransport::McpStdioTransport(McpServer *s, QObject *parent) : QObject(parent), server(s) {}
McpStdioTransport::~McpStdioTransport() {
#ifdef Q_OS_UNIX
    if (reader)
        reader->setEnabled(false);
    if (writer)
        writer->setEnabled(false);
    if (inputFlags >= 0)
        fcntl(STDIN_FILENO, F_SETFL, inputFlags);
    if (outputFlags >= 0)
        fcntl(STDOUT_FILENO, F_SETFL, outputFlags);
#endif
}
void McpStdioTransport::start() {
#ifdef Q_OS_UNIX
    if (reader)
        fail("MCP transport already started");
    inputFlags = fcntl(STDIN_FILENO, F_GETFL);
    outputFlags = fcntl(STDOUT_FILENO, F_GETFL);
    if (inputFlags < 0 || outputFlags < 0 || fcntl(STDIN_FILENO, F_SETFL, inputFlags | O_NONBLOCK) < 0 ||
        fcntl(STDOUT_FILENO, F_SETFL, outputFlags | O_NONBLOCK) < 0)
        fail("Cannot open nonblocking MCP standard streams");
    // Report EPIPE through the event loop instead of terminating during a write.
    std::signal(SIGPIPE, SIG_IGN);
    reader = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    writer = new QSocketNotifier(STDOUT_FILENO, QSocketNotifier::Write, this);
    writer->setEnabled(false);
    connect(reader, &QSocketNotifier::activated, this, [this] { readInput(); });
    connect(writer, &QSocketNotifier::activated, this, [this] { writeOutput(); });
    QTimer::singleShot(0, this, [this] { readInput(); });
#else
    fail("MCP stdio currently requires a POSIX platform (Linux/macOS).");
#endif
}
void McpStdioTransport::stop(const char *message) {
    if (stopped)
        return;
    stopped = true;
    if (reader)
        reader->setEnabled(false);
    if (writer)
        writer->setEnabled(false);
    std::fprintf(stderr, "MolForma MCP: %s\n", message);
    emit finished(1);
}
void McpStdioTransport::readInput() {
#ifdef Q_OS_UNIX
    if (stopped || eof)
        return;
    char buffer[65536];
    const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
    if (count > 0)
        input.append(buffer, count);
    else if (count == 0) {
        eof = true;
        reader->setEnabled(false);
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        stop("Failed to read stdin");
        return;
    }
    dispatch();
#endif
}
void McpStdioTransport::dispatch() {
    if (stopped || scheduled)
        return;
    for (int count = 0; count < 16; ++count) {
        const auto end = input.indexOf('\n');
        if (end < 0) {
            if (input.size() > MaxMessageBytes)
                stop("Message exceeds 8 MiB limit");
            else if (eof && !input.isEmpty()) {
                std::fprintf(stderr, "MolForma MCP: Truncated message at EOF\n");
                input.clear();
                exitCode = 1;
            }
            break;
        }
        if (end > MaxMessageBytes) {
            stop("Message exceeds 8 MiB limit");
            return;
        }
        auto line = input.left(end);
        input.remove(0, end + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        auto reply = server->receive(line);
        if (!reply.isEmpty()) {
            auto bytes = QJsonDocument(reply).toJson(QJsonDocument::Compact);
            if (output.size() + bytes.size() > 32 * 1024 * 1024) {
                stop("Client is not reading responses; output limit exceeded");
                return;
            }
            output.append(bytes);
            output.append('\n');
        }
    }
    if (!stopped && (input.contains('\n') || (eof && !input.isEmpty()))) {
        reader->setEnabled(false);
        scheduled = true;
        QTimer::singleShot(0, this, [this] {
            scheduled = false;
            dispatch();
            if (!stopped && !eof && !scheduled)
                reader->setEnabled(true);
        });
    }
    writeOutput();
}
void McpStdioTransport::writeOutput() {
#ifdef Q_OS_UNIX
    if (stopped)
        return;
    while (!output.isEmpty()) {
        const auto count = ::write(STDOUT_FILENO, output.constData(), output.size());
        if (count > 0)
            output.remove(0, count);
        else if (count < 0 && errno == EINTR)
            continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            writer->setEnabled(true);
            return;
        } else {
            stop("Failed to write stdout");
            return;
        }
    }
    writer->setEnabled(false);
    if (eof && input.isEmpty() && !scheduled) {
        stopped = true;
        emit finished(exitCode);
    }
#endif
}
} // namespace mf
