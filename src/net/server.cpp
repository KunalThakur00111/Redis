#include "server.h"

#include "../commands/command_handler.h"
#include "../persistence/aof.h"
#include "../protocol/resp_parser.h"
#include "../storage/database.h"

#include <array>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <string>
#include <system_error>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace net {
namespace {

constexpr std::size_t max_pending_input = 128 * 1024;
constexpr std::size_t max_pending_output = 128 * 1024;

static_assert(std::atomic<bool>::is_always_lock_free,
              "SIGINT shutdown needs a lock-free atomic flag");
std::atomic<bool> stop_requested{false};

void request_stop(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}

struct Client {
    int socket;
    std::string input;
    std::string output;
    bool read_closed = false;// client have nothing to send, in that case this is used( after clearing output buffer, close socket)
    bool closed_after_reply = false;// client send the invalid arguments, after replying close this socket
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

bool enable_tcp_nodelay(int socket) {
    int enabled = 1;
    return setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                      &enabled, sizeof(enabled)) == 0;
}

// EAGAIN and EWOULDBLOCK mean the socket can be retried later.
bool would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

// Parse every complete request currently buffered and queue its reply.
bool process_input(Client& client, storage::Database& database,
                   persistence::Aof& aof, bool& aof_failed) {
    while (true) {
        auto request = protocol::parse_request(client.input);
        if (request.status == protocol::ParseStatus::incomplete) {
            return true;
        }

        if (request.status == protocol::ParseStatus::invalid) {
            std::string replyy = "-ERR Protocol error: " + request.error + "\r\n";
            if(replyy.size() > max_pending_output - client.output.size()){
                std::cerr << "Client output buffer limit reached\n";
                return false;
            }
            client.output += replyy;
            client.closed_after_reply = true;
            return true;// if we return false, then that will immideatly remove this client, that's why return true
        }

        std::string reply = commands::execute_command(request.arguments, database);
        std::string aof_error;
        if(!aof.append_after_command(request.arguments, reply, database, aof_error)){
            std::cerr << aof_error << '\n';
            aof_failed = true;
            return false;
        }

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
    } 
    else {
        std::cerr << "Send failed: " << errno << '\n';
    }
    return false;
}

// Watch for writes only while a reply is queued, and stop watching reads after EOF.
bool update_client_events(int epoll_fd, Client& client) {
    std::uint32_t wanted = (client.read_closed || client.closed_after_reply) ? 0U : static_cast<std::uint32_t>(EPOLLIN);
    
    if (!client.output.empty()) {
        wanted |= EPOLLOUT;
    }

    if (wanted == client.watched_events) {
        return true;
    }

    if(wanted == 0){
        if(epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client.socket, nullptr) < 0){
            std::cerr << "Epoll client pause failed: " << errno << '\n';
            return false;
        }
        client.watched_events = 0;
        return true;
    }

    epoll_event event{};
    event.events = wanted;
    event.data.fd = client.socket;// if it was removed during a pause, ADD registers it again
    if (epoll_ctl(epoll_fd, client.watched_events == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD/*for modification*/, client.socket, &event) < 0) {
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

constexpr std::size_t max_pending_connections = 256;// at most 256 newly assigned sockets waiting in one worker's connection inbox
constexpr std::size_t max_commands_per_batch = 64;// process up to 64 commands before removing consumed input.

bool notify_eventfd(int fd) {// to wake eventfd
    const std::uint64_t one = 1;
    while(true){
        ssize_t written = write(fd, &one, sizeof(one));
        if(written == static_cast<ssize_t>(sizeof(one))) return true;
        if(written < 0 && errno == EINTR) continue;
        return written < 0 && errno == EAGAIN;
    }
}

bool drain_eventfd(int fd) {
    std::uint64_t count;
    while(true){
        ssize_t received = read(fd, &count, sizeof(count));
        if(received == static_cast<ssize_t>(sizeof(count))) continue;
        if(received < 0 && errno == EINTR) continue;
        return received < 0 && errno == EAGAIN;
    }
}

struct DirectExecution {
    storage::Database& database;
    persistence::Aof& aof;
    std::mutex mutex;
    std::atomic<bool> failed{false};
};

// After assignment, only this worker thread reads or changes its client sockets.
class IoWorker {
    DirectExecution& direct_execution_;
    int epoll_fd_ = -1;// this worker's own epoll watcher
    int wake_fd_ = -1;// this worker's own main-to-worker bell
    std::thread thread_;// the OS thread running this worker
    std::mutex inbox_mutex_;// protects its connection inbox
    std::deque<int> connections_;// new socket assigned by main
    std::unordered_map<int, Client> clients_;// it's currently owned client records

    void remove_client(int socket) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, socket, nullptr);
        close(socket);
        clients_.erase(socket);
    }

    bool process_input(Client& client) {
        while(!client.closed_after_reply){
            std::size_t consumed = 0;
            std::size_t commands_this_batch = 0;
            std::string reply;

            while(commands_this_batch < max_commands_per_batch){
                auto parsed = protocol::parse_request(client.input, consumed);
                if(parsed.status == protocol::ParseStatus::incomplete) break;

                if(parsed.status == protocol::ParseStatus::invalid){
                    reply += "-ERR Protocol error: " + parsed.error + "\r\n";
                    if(reply.size() > max_pending_output - client.output.size()){
                        std::cerr << "Client output buffer limit reached\n";
                        return false;
                    }
                    client.output += reply;
                    client.input.clear();
                    client.closed_after_reply = true;
                    return true;
                }

                std::string aof_error;
                std::string command_reply;
                {
                    std::lock_guard<std::mutex> lock(direct_execution_.mutex);
                    command_reply = commands::execute_command(
                        parsed.arguments, direct_execution_.database);

                    if(!direct_execution_.aof.append_after_command(
                            parsed.arguments, command_reply,
                            direct_execution_.database, aof_error)){

                        std::cerr << aof_error << '\n';
                        
                        direct_execution_.failed.store(true, std::memory_order_relaxed);
                        stop_requested.store(true, std::memory_order_relaxed);
                        return false;
                    }
                }

                reply += command_reply;
                if(reply.size() > max_pending_output - client.output.size()){
                    std::cerr << "Client output buffer limit reached\n";
                    return false;
                }
                consumed += parsed.consumed;
                commands_this_batch++;
            }

            if(consumed == 0) return true;
            client.output += reply;
            client.input.erase(0, consumed);
        }
        return true;
    }

    bool handle_client_event(int socket, std::uint32_t events) {
        auto it = clients_.find(socket);
        if(it == clients_.end()) return true;
        Client& client = it->second;
        bool remove = (events & EPOLLERR) != 0;

        if(!remove && !client.read_closed &&
           !client.closed_after_reply && (events & (EPOLLIN | EPOLLHUP)) != 0){

            int reads_this_turn = 0;
            while(reads_this_turn < 4){

                char buffer[4096];
                ssize_t received = recv(socket, buffer, sizeof(buffer), 0);

                if(received > 0){
                    std::size_t count = static_cast<std::size_t>(received);
                    if(count > max_pending_input - client.input.size()){
                        std::cerr << "Client input buffer limit reached\n";
                        remove = true;
                        break;
                    }

                    client.input.append(buffer, count);
                    if(!process_input(client)){
                        remove = true;
                        break;
                    }

                    if(client.closed_after_reply) break;
                } 
                else if(received == 0){
                    client.read_closed = true;
                    break;
                } 
                else if(would_block() || errno == EINTR){
                    break;
                } 
                else {
                    std::cerr << "Receive failed: " << errno << '\n';
                    remove = true;
                    break;
                }
                reads_this_turn++;
            }
        }

        if(!remove && (events & EPOLLOUT) != 0) remove = !send_pending(client);

        if((client.read_closed || client.closed_after_reply) &&
           client.output.empty()) remove = true;

        if(!remove) remove = !update_client_events(epoll_fd_, client);

        if(remove) remove_client(socket);

        return !direct_execution_.failed.load(std::memory_order_relaxed);
    }

    bool process_inbox() {// main has send something to this worker
        std::deque<int> connections;
        {
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            connections.swap(connections_);// swap moves pending connections into the local deque.
            //                      damn!! this makes the locked section very short
        }

        for(int socket : connections){
            epoll_event event{};
            event.events = EPOLLIN;
            event.data.fd = socket;
            if(epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, socket, &event) < 0){
                std::cerr << "Epoll client registration failed: " << errno << '\n';
                close(socket);
                continue;
            }
            clients_.emplace(socket, Client{socket, {}, {}, false, false, EPOLLIN});
        }
        return true;
    }

    void run() {
        std::array<epoll_event, 128> ready_events{};

        while(!stop_requested.load(std::memory_order_relaxed)){

            int ready = epoll_wait(epoll_fd_, ready_events.data(),
                                   static_cast<int>(ready_events.size()), 100);

            if(ready < 0){
                if(errno == EINTR) continue;
                std::cerr << "I/O worker epoll wait failed: " << errno << '\n';
                direct_execution_.failed.store(true, std::memory_order_relaxed);
                stop_requested.store(true, std::memory_order_relaxed);
                break;
            }

            for(int i = 0; i < ready; i++){
                if(stop_requested.load(std::memory_order_relaxed)) break;

                int socket = ready_events[static_cast<std::size_t>(i)].data.fd;

                if(socket == wake_fd_){// main puts a connection in its inbox
                    if(!drain_eventfd(wake_fd_) || !process_inbox()){
                        direct_execution_.failed.store(true, std::memory_order_relaxed);
                        stop_requested.store(true, std::memory_order_relaxed);
                        break;
                    }
                } 

                else if(!handle_client_event(socket, ready_events[static_cast<std::size_t>(i)].events)){
                    break;
                }
            }

            if(stop_requested.load(std::memory_order_relaxed)) break;
        }

        for(const auto& entry : clients_) close(entry.first);
        clients_.clear();
    }

public:
    IoWorker(DirectExecution& direct_execution)
        : direct_execution_(direct_execution) {}

    ~IoWorker() {
        if(thread_.joinable()) thread_.join();
        for(int socket : connections_) close(socket);
        if(wake_fd_ >= 0) close(wake_fd_);
        if(epoll_fd_ >= 0) close(epoll_fd_);
    }

    bool start() {
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);// eventfd is a bell that wakes the thread after work is put in its queue.
        if(epoll_fd_ < 0 || wake_fd_ < 0){
            std::cerr << "I/O worker setup failed: " << errno << '\n';
            return false;
        }
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = wake_fd_;
        if(epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &event) < 0){
            std::cerr << "I/O worker wake registration failed: " << errno << '\n';
            return false;
        }
        try {
            thread_ = std::thread(&IoWorker::run, this);
        } 
        catch(const std::system_error& failure){
            std::cerr << "I/O worker start failed: " << failure.what() << '\n';
            return false;
        }
        return true;
    }

    bool add_connection(int socket) {
        bool wake = false;
        {

            std::lock_guard<std::mutex> lock(inbox_mutex_);

            if(connections_.size() == max_pending_connections) return false;
            wake = connections_.empty();
            connections_.push_back(socket);

        }
        if(wake && !notify_eventfd(wake_fd_)){
            direct_execution_.failed.store(true, std::memory_order_relaxed);
            stop_requested.store(true, std::memory_order_relaxed);
        }
        return true;
    }

    void wake() { notify_eventfd(wake_fd_); }
    void join() { if(thread_.joinable()) thread_.join(); }
};

// The main thread accepts clients.
int run_threaded_loop(int listener, int epoll_fd, storage::Database& database,
                      persistence::Aof& aof, std::uint16_t port, int io_threads) {

    DirectExecution direct_execution{database, aof, {}};

    std::vector<std::unique_ptr<IoWorker>> workers;
    workers.reserve(static_cast<std::size_t>(io_threads - 1));// one is main thread so that's why (io_threads - 1)
    bool started = true;
    for(int i = 1; i < io_threads; i++){
        auto worker = std::make_unique<IoWorker>(direct_execution);
        if(!worker->start()){
            started = false;
            break;
        }
        workers.push_back(std::move(worker));
    }

    if(!started){
        stop_requested.store(true, std::memory_order_relaxed);
        for(auto& worker : workers) worker->wake();
        for(auto& worker : workers) worker->join();
        return 1;
    }

    int exit_status = 0;
    std::size_t next_worker = 0;
    std::array<epoll_event, 128> ready_events{};
    std::cout << "Listening on 0.0.0.0:" << port << " with " << io_threads
              << " I/O threads (including acceptor)\n";

    while(!stop_requested.load(std::memory_order_relaxed)){

        int ready = epoll_wait(epoll_fd, ready_events.data(),
                               static_cast<int>(ready_events.size()), 100);

        if(ready < 0){
            if(errno == EINTR) continue;
            std::cerr << "Epoll wait failed: " << errno << '\n';
            exit_status = 1;
            break;
        }

        if(direct_execution.failed.load(std::memory_order_relaxed)){
            exit_status = 1;
            break;
        }

        std::string aof_error;
        if(!aof.check_sync_error(aof_error)){
            std::cerr << aof_error << '\n';
            exit_status = 1;
            break;
        }

        for(int i = 0; i < ready; i++){
            
            if(stop_requested.load(std::memory_order_relaxed)) break;

            int socket = ready_events[static_cast<std::size_t>(i)].data.fd;

            if(socket != listener) continue;
            int accepted = 0;
            while(accepted < 16){
                int connection = accept(listener, nullptr, nullptr);
                if(connection < 0){
                    if(!would_block() && errno != EINTR)
                        std::cerr << "Accept failed: " << errno << '\n';
                    break;
                }
                accepted++;
                if(!make_nonblocking(connection) || !enable_tcp_nodelay(connection)){
                    close(connection);
                    continue;
                }

                IoWorker& worker = *workers[next_worker];
                next_worker = (next_worker + 1) % workers.size();
                if(!worker.add_connection(connection)) close(connection);
            }
        }

        if(exit_status != 0 || stop_requested.load(std::memory_order_relaxed)) break;

        if(direct_execution.failed.load(std::memory_order_relaxed)){
            exit_status = 1;
            break;
        }

        std::lock_guard<std::mutex> lock(direct_execution.mutex);
        database.expire_some(16);
    }

    stop_requested.store(true, std::memory_order_relaxed);
    for(auto& worker : workers) worker->wake();
    for(auto& worker : workers) worker->join();

    if(direct_execution.failed.load(std::memory_order_relaxed)) exit_status = 1;
    return exit_status;
}

}  // namespace

int run_server(std::uint16_t port, const std::string& aof_path,
               bool sync_every_second, int io_threads) {
    storage::Database database;
    persistence::Aof aof(sync_every_second ? persistence::SyncPolicy::every_second
                                            : persistence::SyncPolicy::always);

    stop_requested.store(false, std::memory_order_relaxed);
    struct sigaction interrupt_action{};
    interrupt_action.sa_handler = request_stop;// We are giving Linux the function to call for SIGINT.

    if(sigemptyset(&interrupt_action.sa_mask) < 0 ||
       sigaction(SIGINT, &interrupt_action, nullptr) < 0){// tells linux when this process recieves ctrl + c, call request_stop. 
        std::cerr << "SIGINT handler setup failed: " << errno << '\n';
        return 1;
    }

    std::string aof_error;
    if(!aof.open_and_replay(aof_path, database, aof_error)){
        std::cerr << aof_error << '\n';
        return 1;
    }

    if(stop_requested.load(std::memory_order_relaxed)) return 0;

    // Create the TCP socket that accepts new clients.
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        std::cerr << "Socket creation failed: " << errno << '\n';
        return 1;
    }
    // Allow a quick restart after a previous server instance exits.
    int reuse_address = 1;
    if (setsockopt(listener, SOL_SOCKET/* it means this is on socket level*/, SO_REUSEADDR, &reuse_address,
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

    if(io_threads > 1){
        int exit_status = run_threaded_loop(listener, epoll_fd, database, aof,
                                             port, io_threads);
        close(epoll_fd);
        close(listener);
        if(!aof.stop_and_sync(aof_error)){
            std::cerr << aof_error << '\n';
            exit_status = 1;
        }
        return exit_status;
    }

    std::unordered_map<int, Client> clients;
    int exit_status = 0;
    bool aof_failed = false;
    std::cout << "Listening on 0.0.0.0:" << port << '\n';

    // One epoll_wait() returns readiness for both the listener and clients.
    std::array<epoll_event, 128> ready_events{};
    while (!stop_requested.load(std::memory_order_relaxed)) {
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
        if(!aof.check_sync_error(aof_error)){
            std::cerr << aof_error << '\n';
            exit_status = 1;
            break;
        }

        for (int i = 0; i < ready; i++) {
            if(stop_requested.load(std::memory_order_relaxed)) break;
            if(aof_failed) break;
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

                    if (!make_nonblocking(connection) || !enable_tcp_nodelay(connection)) {
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
                    clients.emplace(connection, Client{connection, {}, {}, false,false,EPOLLIN});
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
                (ready_event.events & (EPOLLIN | EPOLLHUP)) != 0 && !client.closed_after_reply) {
                // At most four successful reads per event so other clients get time.
                int reads_this_turn = 0;
                while (reads_this_turn < 4) {
                    char buffer[4096];
                    ssize_t received = recv(client.socket, buffer, sizeof(buffer), 0);
                    if (received > 0) {
                        auto count = static_cast<std::size_t>(received);
                        if (count > max_pending_input - client.input.size()) {
                            std::cerr << "Client input buffer limit reached\n";
                            remove = true;
                            break;
                        }
                        client.input.append(buffer, count);
                        if (!process_input(client, database, aof, aof_failed)) {
                            remove = true;
                            break;
                        }
                        if(client.closed_after_reply) break;
                    } 
                    else if (received == 0) {
                        client.read_closed = true;
                        break;
                    } 
                    else if (would_block() || errno == EINTR) {
                        break;
                    } 
                    else {
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
            if(client.closed_after_reply && client.output.empty()){
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
        if(aof_failed){
            exit_status = 1;
            break;
        }
        database.expire_some(16);
    }
    
    close_clients(clients);
    close(epoll_fd);
    close(listener);
    if(!aof.stop_and_sync(aof_error)){
        std::cerr << aof_error << '\n';
        exit_status = 1;
    }
    return exit_status;
}

}  // namespace net


/*

about eventfd:-
Why is a bell necessary? Main can be asleep inside epoll wait , waiting for network activity. A worker adding an
item to a normal Ct* deque does not automatically wake Linux's epoll wait . Writing to an eventfd does,
because main registered that descriptor with epoll.

*/