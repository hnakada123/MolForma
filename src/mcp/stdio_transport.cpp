// SPDX-License-Identifier: GPL-2.0-or-later
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
#ifdef Q_OS_WIN
#include <condition_variable>
#include <mutex>
#include <thread>
#define NOMINMAX
#include <windows.h>
#endif

namespace mf {
// Windows pipes cannot be watched by the event loop: one thread blocks in ReadFile, another drains the
// output, and both post to the event loop. The threads are detached and keep this state alive; they
// post nothing once the transport is gone, and a client that stops reading never blocks the GUI.
struct McpStdioTransport::Threads {
#ifdef Q_OS_WIN
    std::mutex mutex;
    std::condition_variable wake;
    McpStdioTransport *owner = nullptr; // Null once the transport is destroyed.
    QByteArray pending;
    template <typename F> void post(F f) {
        std::lock_guard lock(mutex);
        if (auto *transport = owner)
            QMetaObject::invokeMethod(transport, [transport, f] { f(transport); }, Qt::QueuedConnection);
    }
#endif
};
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
#ifdef Q_OS_WIN
    if (threads) {
        {
            std::lock_guard lock(threads->mutex);
            threads->owner = nullptr;
        }
        threads->wake.notify_all();
    }
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
#elif defined(Q_OS_WIN)
    if (threads)
        fail("MCP transport already started");
    const auto in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!in || in == INVALID_HANDLE_VALUE || !out || out == INVALID_HANDLE_VALUE)
        fail("Cannot open MCP standard streams");
    threads = std::make_shared<Threads>();
    threads->owner = this;
    std::thread([state = threads, in] {
        QByteArray buffer(65536, Qt::Uninitialized);
        for (;;) {
            DWORD count = 0;
            const bool read = ReadFile(in, buffer.data(), DWORD(buffer.size()), &count, nullptr);
            if (!read || count == 0) {
                // A closed pipe reports ERROR_BROKEN_PIPE, the end of a file zero bytes.
                const auto error = read ? DWORD(ERROR_HANDLE_EOF) : GetLastError();
                const bool failed = error != ERROR_BROKEN_PIPE && error != ERROR_HANDLE_EOF;
                state->post([failed](McpStdioTransport *t) {
                    if (failed) {
                        t->stop("Failed to read stdin");
                        return;
                    }
                    t->eof = true;
                    t->dispatch();
                });
                return;
            }
            state->post([bytes = buffer.left(count)](McpStdioTransport *t) {
                if (t->stopped || t->eof)
                    return;
                t->input.append(bytes);
                t->dispatch();
            });
        }
    }).detach();
    std::thread([state = threads, out] {
        for (;;) {
            QByteArray chunk;
            {
                std::unique_lock lock(state->mutex);
                state->wake.wait(lock, [&] { return !state->pending.isEmpty() || !state->owner; });
                if (!state->owner)
                    return;
                chunk.swap(state->pending);
            }
            for (qsizetype done = 0; done < chunk.size();) {
                DWORD count = 0;
                if (!WriteFile(out, chunk.constData() + done, DWORD(chunk.size() - done), &count, nullptr)) {
                    state->post([](McpStdioTransport *t) { t->stop("Failed to write stdout"); });
                    return;
                }
                done += count;
            }
            state->post([written = chunk.size()](McpStdioTransport *t) {
                t->unflushed -= written;
                t->writeOutput();
            });
        }
    }).detach();
#else
    fail("MCP stdio requires Linux, macOS or Windows.");
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
            if (output.size() + unflushed + bytes.size() > 32 * 1024 * 1024) {
                stop("Client is not reading responses; output limit exceeded");
                return;
            }
            output.append(bytes);
            output.append('\n');
        }
    }
    if (!stopped && (input.contains('\n') || (eof && !input.isEmpty()))) {
        if (reader)
            reader->setEnabled(false);
        scheduled = true;
        QTimer::singleShot(0, this, [this] {
            scheduled = false;
            dispatch();
            if (reader && !stopped && !eof && !scheduled)
                reader->setEnabled(true);
        });
    }
    writeOutput();
}
void McpStdioTransport::writeOutput() {
    if (stopped)
        return;
#ifdef Q_OS_UNIX
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
#elif defined(Q_OS_WIN)
    if (!output.isEmpty()) {
        unflushed += output.size();
        {
            std::lock_guard lock(threads->mutex);
            threads->pending.append(output);
        }
        output.clear();
        threads->wake.notify_one();
    }
    if (unflushed)
        return;
#endif
    if (eof && input.isEmpty() && !scheduled) {
        stopped = true;
        emit finished(exitCode);
    }
}
} // namespace mf
