#include "server.h"

#include "../commands/command_handler.h"
#include "../protocol/resp_parser.h"
#include "../storage/database.h"

#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <unordered_map>

namespace net {
namespace {

constexpr std::size_t max_pending_input = 128 * 1024;
constexpr std::size_t max_pending_output = 128 * 1024;

struct Client {
    int socket;
    std::string input;
    std::string output;
    bool read_closed = false;
    std::uint32_t watched_events = EPOLLIN;
};

// Put a socket in nonblocking mode so accept/recv/send never wait for data.
bool make_nonblocking(int socket) {
    int flags = fcntl(socket, F_GETFL, 0);// to change the property of open descriptor
    // conceptually it's signature is  ==>  int fcntl(int fd, int operation,...)

    // F_GETFL means get file status flag
    // F_SETFL means set file status flag

    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK /* this just sets the new flag of non blocking while keeping other flags too, that's why OR*/) == 0; 
    // 0 on success and -1 on failure that's why ==0 check
}

// EAGAIN and EWOULDBLOCK mean the socket can be retried later.
bool would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

// Parse every complete request currently buffered and queue its reply.
bool process_input(Client& client, storage::Database& database) {
    while (true) {
        auto request = protocol::parse_request(client.input);
        if (request.status == protocol::ParseStatus::incomplete) {
            return true;
        }
        if (request.status == protocol::ParseStatus::invalid) {
            std::cerr << "Protocol error: " << request.error << '\n';
            return false;
        }

        std::string reply = commands::execute_command(request.arguments, database);
        if (reply.size() > max_pending_output - client.output.size()) {
            std::cerr << "Client output buffer limit reached\n";
            return false;
        }
        client.output += reply;
        client.input.erase(0, request.consumed);
    }
}

// Send one chunk of queued output, retaining any bytes not yet sent.
bool send_pending(Client& client) {
    if (client.output.empty()) {
        return true;
    }

    ssize_t sent = send(client.socket, client.output.data(),
                        client.output.size(), MSG_NOSIGNAL);
    if (sent > 0) {
        client.output.erase(0, static_cast<std::size_t>(sent));
        return true;
    }
    if (sent < 0 && (would_block() || errno == EINTR)) {
        return true;
    }
    if (sent == 0) {
        std::cerr << "Send returned zero for nonempty output\n";
    } else {
        std::cerr << "Send failed: " << errno << '\n';
    }
    return false;
}

// Watch for writes only while a reply is queued, and stop watching reads after EOF.
bool update_client_events(int epoll_fd, Client& client) {
    std::uint32_t wanted = client.read_closed ? 0U : static_cast<std::uint32_t>(EPOLLIN);
    if (!client.output.empty()) {
        wanted |= EPOLLOUT;
    }
    if (wanted == client.watched_events) {
        return true;
    }

    epoll_event event{};
    event.events = wanted;
    event.data.fd = client.socket;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD/*for modification*/, client.socket, &event) < 0) {
        std::cerr << "Epoll client update failed: " << errno << '\n';
        return false;
    }
    client.watched_events = wanted;
    return true;
}

// Close all client sockets when the event loop stops.
void close_clients(const std::unordered_map<int, Client>& clients) {
    for (const auto& entry : clients) {
        close(entry.first);
    }
}

}  // namespace

int run_server(std::uint16_t port) {
    // Create the TCP socket that accepts new clients.
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        std::cerr << "Socket creation failed: " << errno << '\n';
        return 1;
    }
    // Allow a quick restart after a previous server instance exits.
    int reuse_address = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                   sizeof(reuse_address)) < 0) {
        std::cerr << "SO_REUSEADDR failed: " << errno << '\n';
        close(listener);
        return 1;
    }

    // Accept IPv4 connections on every local network interface.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(listener, SOMAXCONN) < 0 || !make_nonblocking(listener)) {
        std::cerr << "Listener setup failed: " << errno << '\n';
        close(listener);
        return 1;
    }

    // Create the kernel event watcher; its descriptor must be closed on shutdown.
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        std::cerr << "Epoll creation failed: " << errno << '\n';
        close(listener);
        return 1;
    }

    // Ask epoll to report when the listener has a connection ready to accept.
    epoll_event listener_event{};
    listener_event.events = EPOLLIN; // Report when accept() can receive a connection. and it is bitmap
    listener_event.data.fd = listener; // Identify this socket in epoll_wait() results.
    // Add the listener to this epoll instance with the event mask above.
    if (epoll_ctl(epoll_fd/*which epoll instance to change*/, EPOLL_CTL_ADD/*for adding the events*/, listener/*which descriptor to add*/, &listener_event/*what kind of that socket is*/) < 0) {
        std::cerr << "Epoll listener registration failed: " << errno << '\n';
        close(epoll_fd);
        close(listener);
        return 1;
    }

    std::unordered_map<int, Client> clients;
    storage::Database database;
    int exit_status = 0;
    std::cout << "Listening on 0.0.0.0:" << port << '\n';

    // One epoll_wait() returns readiness for both the listener and clients.
    std::array<epoll_event, 128> ready_events{};
    while (true) {
        int ready = epoll_wait(
                  epoll_fd, 
                  ready_events.data(),// a pointer where linux write the data on return; and it is overwrite on every iteration.
                  static_cast<int>(ready_events.size()),// max size of data return
                  100// its argument is a timeout in milliseconds.
                  );
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Epoll wait failed: " << errno << '\n';
            exit_status = 1;
            break;
        }

        for (int i = 0; i < ready; i++) {
            const epoll_event& ready_event = ready_events[static_cast<std::size_t>(i)];
            int socket = ready_event.data.fd;

            if (socket == listener) {
                // Level triggering reports the listener again if 16 accepts leave work.
                int accepted = 0;
                while (accepted < 16) {
                    int connection = accept(listener, nullptr, nullptr);
                    if (connection < 0) {
                        if (!would_block() && errno != EINTR) {
                            std::cerr << "Accept failed: " << errno << '\n';
                        }
                        break;
                    }
                    accepted++;

                    if (!make_nonblocking(connection)) {
                        close(connection);
                        continue;
                    }

                    // The new socket needs its own registration for read events.
                    epoll_event client_event{};
                    client_event.events = EPOLLIN;
                    client_event.data.fd = connection;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, connection,
                                  &client_event) < 0) {
                        std::cerr << "Epoll client registration failed: " << errno << '\n';
                        close(connection);
                        continue;
                    }
                    clients.emplace(connection, Client{connection, {}, {}, false, EPOLLIN});
                }
                continue;
            }

            auto it = clients.find(socket);
            if (it == clients.end()) {
                continue;
            }
            Client& client = it->second;
            bool remove = (ready_event.events & EPOLLERR) != 0;

            if (!remove && !client.read_closed &&
                (ready_event.events & (EPOLLIN | EPOLLHUP)) != 0) {
                // At most four successful reads per event so other clients get time.
                int reads_this_turn = 0;
                while (reads_this_turn < 4) {
                    char buffer[4096];
                    ssize_t received = recv(client.socket, buffer, sizeof(buffer), 0);
                    if (received > 0) {
                        std::size_t count = static_cast<std::size_t>(received);
                        if (count > max_pending_input - client.input.size()) {
                            std::cerr << "Client input buffer limit reached\n";
                            remove = true;
                            break;
                        }
                        client.input.append(buffer, count);
                        if (!process_input(client, database)) {
                            remove = true;
                            break;
                        }
                    } else if (received == 0) {
                        client.read_closed = true;
                        break;
                    } else if (would_block() || errno == EINTR) {
                        break;
                    } else {
                        std::cerr << "Receive failed: " << errno << '\n';
                        remove = true;
                        break;
                    }
                    reads_this_turn++;
                }
            }

            if (!remove && (ready_event.events & EPOLLOUT) != 0) {
                remove = !send_pending(client);
            }
            if (client.read_closed && client.output.empty()) {
                remove = true;
            }
            if (!remove) {
                remove = !update_client_events(epoll_fd, client);
            }
            if (remove) {
                // Remove the registration before closing and forgetting the client.
                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client.socket, nullptr);
                close(client.socket);
                clients.erase(it);
            }
        }
        database.expire_some(16);
    }
    
    close_clients(clients);
    close(epoll_fd);
    close(listener);
    return exit_status;
}

}  // namespace net
