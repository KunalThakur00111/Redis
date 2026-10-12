#include "server.h"

#include "../commands/command_handler.h"
#include "../protocol/resp_parser.h"
#include "../storage/database.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace net {
namespace {

constexpr std::size_t max_pending_input = 128 * 1024;
constexpr std::size_t max_pending_output = 128 * 1024;

struct Client {
    int socket;
    std::string input;
    std::string output;
    bool read_closed = false;
};

// Put a socket in nonblocking mode so accept/recv/send never wait for data.
bool make_nonblocking(int socket) {
    int flags = fcntl(socket, F_GETFL, 0);// to change the property of open descriptor
    // conceptually it's signature is  ==>  int fcntl(int fd, int operation,...)

    // F_GETFL means get file status flag
    // F_SETFL means set file status flag

    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK /* this just set new flag of non blocking while keeping other flags too, that's why OR*/) == 0; 
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

// Close all client sockets when the event loop stops.
void close_clients(const std::vector<Client>& clients) {
    for (const Client& client : clients) {
        close(client.socket);
    }
}

}  // namespace

int run_server() {
    // Create the TCP socket that accepts new clients.
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        std::cerr << "Socket creation failed: " << errno << '\n';
        return 1;
    }
    if (listener >= FD_SETSIZE) {
        std::cerr << "Listener descriptor exceeds select limit\n";
        close(listener);
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

    // Listen only on this machine, on Redis's usual TCP port.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(6379);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(listener, SOMAXCONN) < 0 || !make_nonblocking(listener)) {
        std::cerr << "Listener setup failed: " << errno << '\n';
        close(listener);
        return 1;
    }

    std::vector<Client> clients;
    storage::Database database;
    int exit_status = 0;
    std::cout << "Server is listening\n";

    // Rebuild the read/write sets and wait until a socket is ready.
    while (true) {
        fd_set reads;
        fd_set writes;
        FD_ZERO(&reads);
        FD_ZERO(&writes);
        FD_SET(listener, &reads);
        int highest = listener;

        for (const Client& client : clients) {
            if (!client.read_closed) {
                FD_SET(client.socket, &reads);
            }
            if (!client.output.empty()) {
                FD_SET(client.socket, &writes);
            }
            if (client.socket > highest) {
                highest = client.socket;
            }
        }
        // fd_set gives bitmap, highest+ or first arguments tell upto how much 
        // len of bitmap we have to check

        int ready = select(highest + 1, &reads, &writes,
                           nullptr, nullptr);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Select failed: " << errno << '\n';
            exit_status = 1;
            break;
        }

        // A readable listener has one or more incoming connections.
        if (FD_ISSET(listener, &reads)) {
            while (true) {
                int connection = accept(listener, nullptr, nullptr);
                if (connection < 0) {
                    if (!would_block() && errno != EINTR) {
                        std::cerr << "Accept failed: " << errno << '\n';
                    }
                    break;
                }

                // select/FD_SET cannot safely represent descriptors beyond this limit.
                if (clients.size() >= FD_SETSIZE - 1 ||
                    connection >= FD_SETSIZE || !make_nonblocking(connection)) {
                    close(connection);
                    continue;
                }
                clients.push_back(Client{connection, {}, {}, false});
            }
        }

        // Service each ready client, then remove closed or failed connections.
        for (std::size_t i = 0; i < clients.size();) {
            Client& client = clients[i];
            bool remove = false;

            if (FD_ISSET(client.socket, &reads)) {
                while (true) {
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
                }
            }

            if (!remove && FD_ISSET(client.socket, &writes)) {
                remove = !send_pending(client);
            }
            if (client.read_closed && client.output.empty()) {
                remove = true;
            }
            if (remove) {
                close(client.socket);
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    close_clients(clients);
    close(listener);
    return exit_status;
}

}  // namespace net
