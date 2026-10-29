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
    bool read_paused = false; // temperory stop reading more bytes from this client
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
    std::uint32_t wanted = (client.read_closed || client.closed_after_reply || client.read_paused) ? 0U : static_cast<std::uint32_t>(EPOLLIN);
    
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

// "batch" is group of complete commands from one client , put into one message 

constexpr std::size_t max_pending_requests = 256;// at most 256 batches in the shared queue awaiting main thread execution
constexpr std::size_t max_pending_connections = 256;// at most 256 newly assigned sockets waiting in one worker's connection inbox
constexpr std::size_t max_in_flight_per_worker = 256;// at most 256 batches sent by one worker but not yet answered.
constexpr std::size_t max_commands_per_batch = 64;// one request message can carry up to 64 parsed commands.

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

// Workers send parsed commands to the one thread that owns Database and AOF.
struct ThreadedCommand {
    std::vector<std::string> arguments;
    std::string protocol_error;
};

struct ThreadedRequest {// basically one batch
    std::size_t worker_index;
    int socket;
    std::uint64_t client_id;
    std::vector<ThreadedCommand> commands;
};

struct ThreadedReply {
    int socket;
    std::uint64_t client_id;// linux can reuse an integer descriptor after a socket closes.
    std::string reply;
};

// The mutex protects the queue, not the database or any client's buffers.
struct RequestQueue {
    int wake_fd = -1;// the bell that workers uses to wake main thread
    std::mutex mutex;// protects pending
    std::deque<ThreadedRequest> pending;
    std::atomic<bool> failed{false};
};

enum class QueueResult { accepted, full, failed };

QueueResult queue_request(RequestQueue& queue, ThreadedRequest&& request) {
    bool wake = false;

    {
        std::lock_guard<std::mutex> lock(queue.mutex);
        if(queue.pending.size() == max_pending_requests) return QueueResult::full;
        wake = queue.pending.empty();// empty to non empty requires wake up
        queue.pending.push_back(std::move(request));
    }

    if(wake && !notify_eventfd(queue.wake_fd)){
        queue.failed.store(true, std::memory_order_relaxed);
        stop_requested.store(true, std::memory_order_relaxed);
        return QueueResult::failed;
    }
    return QueueResult::accepted;
}

struct ThreadedClient {
    Client client;
    std::uint64_t id;
    std::size_t in_flight = 0;
};

// After assignment, only this worker thread reads or changes its client sockets.
class IoWorker {
    std::size_t index_;// its index/number in the worker vector
    RequestQueue& requests_;// refrence to the worker-to-main queue
    int epoll_fd_ = -1;// this worker's own epoll watcher
    int wake_fd_ = -1;// this worker's own main-to-worker bell
    std::thread thread_;// the OS thread running this worker
    std::mutex inbox_mutex_;// protects it's connection and reply inboxes
    std::deque<int> connections_;// new socket assigned by main
    std::deque<ThreadedReply> replies_;// replies produced by main
    std::unordered_map<int, ThreadedClient> clients_;// it's currently owned client records
    std::uint64_t next_client_id_ = 1;// new id for each new connection
    std::size_t in_flight_total_ = 0;// batches sent by this worker, waiting for replies

    void remove_client(int socket) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, socket, nullptr);
        close(socket);
        clients_.erase(socket);
    }

    bool process_input(ThreadedClient& tracked) {
        Client& client = tracked.client;
        while(!client.closed_after_reply){
            if(in_flight_total_ == max_in_flight_per_worker){
                client.read_paused = true;
                return true;
            }

            ThreadedRequest request{index_, client.socket, tracked.id, {}};
            std::string remaining = client.input;
            std::size_t consumed = 0;
            bool invalid = false;

            while(request.commands.size() < max_commands_per_batch){
                auto parsed = protocol::parse_request(remaining);
                if(parsed.status == protocol::ParseStatus::incomplete) break;

                if(parsed.status == protocol::ParseStatus::invalid){
                    request.commands.push_back(ThreadedCommand{
                        {}, "-ERR Protocol error: " + parsed.error + "\r\n"});
                    invalid = true;
                    break;
                }

                consumed += parsed.consumed;
                remaining.erase(0, parsed.consumed);
                request.commands.push_back(ThreadedCommand{
                    std::move(parsed.arguments), {}});
            }
            if(request.commands.empty()) return true;

            QueueResult queued = queue_request(requests_, std::move(request));
            if(queued == QueueResult::failed) return false;
            if(queued == QueueResult::full){
                client.read_paused = true;
                return true;
            }
            tracked.in_flight++;
            in_flight_total_++;

            if(invalid){
                client.input.clear();
                client.closed_after_reply = true;
                return true;
            }
            client.input.erase(0, consumed);
        }
        return true;
    }

    bool handle_client_event(int socket, std::uint32_t events) {
        auto it = clients_.find(socket);
        if(it == clients_.end()) return true;
        ThreadedClient& tracked = it->second;
        Client& client = tracked.client;
        bool remove = (events & EPOLLERR) != 0;

        if(!remove && !client.read_closed && !client.read_paused &&
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
                    if(!process_input(tracked)){
                        remove = true;
                        break;
                    }

                    if(client.read_paused || client.closed_after_reply) break;
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
           client.output.empty() && tracked.in_flight == 0) remove = true;

        if(!remove) remove = !update_client_events(epoll_fd_, client);

        if(remove) remove_client(socket);

        return !requests_.failed.load(std::memory_order_relaxed);
    }

    bool process_inbox() {// main has send something to this worker
        std::deque<int> connections;
        std::deque<ThreadedReply> replies;
        {
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            connections.swap(connections_);
            replies.swap(replies_);// swap moves the worker's pending inbox contents into these local deques.
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
            clients_.emplace(socket, ThreadedClient{
                Client{socket, {}, {}, false, false, EPOLLIN, false},
                next_client_id_++});
        }

        for(auto& completed : replies){
            if(in_flight_total_ > 0) in_flight_total_--;

            auto it = clients_.find(completed.socket);
            if(it == clients_.end() || it->second.id != completed.client_id) continue;

            ThreadedClient& tracked = it->second;
            Client& client = tracked.client;

            if(tracked.in_flight > 0) tracked.in_flight--;

            if(completed.reply.size() > max_pending_output - client.output.size()){
                std::cerr << "Client output buffer limit reached\n";
                remove_client(completed.socket);
                continue;
            }

            client.output += completed.reply;

            if((client.read_closed || client.closed_after_reply) &&
               client.output.empty() && tracked.in_flight == 0){
                remove_client(completed.socket);
            } 
            
            else if(!update_client_events(epoll_fd_, client)){
                remove_client(completed.socket);
            }
        }
        return true;
    }

    void resume_paused_clients() {
        for(auto it = clients_.begin(); it != clients_.end();){
            int socket = it->first;
            ThreadedClient& tracked = it->second;
            it++;
            Client& client = tracked.client;
            if(!client.read_paused) continue;
            client.read_paused = false;
            if(!process_input(tracked) || !update_client_events(epoll_fd_, client)){
                remove_client(socket);
            }
        }
    }

    void run() {
        std::array<epoll_event, 128> ready_events{};

        while(!stop_requested.load(std::memory_order_relaxed)){

            int ready = epoll_wait(epoll_fd_, ready_events.data(),
                                   static_cast<int>(ready_events.size()), 100);

            if(ready < 0){
                if(errno == EINTR) continue;
                std::cerr << "I/O worker epoll wait failed: " << errno << '\n';
                requests_.failed.store(true, std::memory_order_relaxed);
                stop_requested.store(true, std::memory_order_relaxed);
                break;
            }

            for(int i = 0; i < ready; i++){
                if(stop_requested.load(std::memory_order_relaxed)) break;

                int socket = ready_events[static_cast<std::size_t>(i)].data.fd;

                if(socket == wake_fd_){// main puts a connection or reply in its inbox
                    if(!drain_eventfd(wake_fd_) || !process_inbox()){
                        requests_.failed.store(true, std::memory_order_relaxed);
                        stop_requested.store(true, std::memory_order_relaxed);
                        break;
                    }
                } 

                else if(!handle_client_event(socket,
                             ready_events[static_cast<std::size_t>(i)].events)){
                    break;
                }
            }

            if(stop_requested.load(std::memory_order_relaxed)) break;
            resume_paused_clients();
        }

        for(const auto& entry : clients_) close(entry.first);
        clients_.clear();
    }

public:
    IoWorker(std::size_t index, RequestQueue& requests)
        : index_(index), requests_(requests) {}

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
            wake = connections_.empty() && replies_.empty();
            connections_.push_back(socket);

        }
        if(wake && !notify_eventfd(wake_fd_)){
            requests_.failed.store(true, std::memory_order_relaxed);
            stop_requested.store(true, std::memory_order_relaxed);
        }
        return true;
    }

    void add_reply(ThreadedReply&& reply) {
        bool wake = false;
        {
            std::lock_guard<std::mutex> lock(inbox_mutex_);
            wake = connections_.empty() && replies_.empty();
            replies_.push_back(std::move(reply));
        }
        if(wake && !notify_eventfd(wake_fd_)){
            requests_.failed.store(true, std::memory_order_relaxed);
            stop_requested.store(true, std::memory_order_relaxed);
        }
    }

    void wake() { notify_eventfd(wake_fd_); }
    void join() { if(thread_.joinable()) thread_.join(); }
};

// The main thread accepts clients, executes queued commands, and routes replies.
int run_threaded_loop(int listener, int epoll_fd, storage::Database& database,
                      persistence::Aof& aof, std::uint16_t port, int io_threads) {

    RequestQueue requests;
    requests.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if(requests.wake_fd < 0){
        std::cerr << "Request wake creation failed: " << errno << '\n';
        return 1;
    }
    epoll_event request_event{};
    request_event.events = EPOLLIN;
    request_event.data.fd = requests.wake_fd;
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, requests.wake_fd, &request_event) < 0){
        std::cerr << "Request wake registration failed: " << errno << '\n';
        close(requests.wake_fd);
        return 1;
    }

    std::vector<std::unique_ptr<IoWorker>> workers;
    workers.reserve(static_cast<std::size_t>(io_threads - 1));// one is main thread so that's why (io_threads - 1)
    bool started = true;
    for(int i = 1; i < io_threads; i++){
        auto worker = std::make_unique<IoWorker>(workers.size(), requests);
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
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, requests.wake_fd, nullptr);
        close(requests.wake_fd);
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

        if(requests.failed.load(std::memory_order_relaxed)){
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

            if(socket == requests.wake_fd){// main thread have to run command and give reply
                if(!drain_eventfd(requests.wake_fd)){
                    std::cerr << "Request wake read failed: " << errno << '\n';
                    exit_status = 1;
                    break;
                }
                continue;
            }

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
                if(!make_nonblocking(connection)){
                    close(connection);
                    continue;
                }

                IoWorker& worker = *workers[next_worker];
                next_worker = (next_worker + 1) % workers.size();
                if(!worker.add_connection(connection)) close(connection);
            }
        }

        if(exit_status != 0 || stop_requested.load(std::memory_order_relaxed)) break;

        std::deque<ThreadedRequest> batch;
        bool was_full = false;

        {
            std::lock_guard<std::mutex> lock(requests.mutex);
            was_full = requests.pending.size() == max_pending_requests;
            batch.swap(requests.pending);
        }

        for(auto& request : batch){
            std::string reply;
            for(auto& command : request.commands){
                if(!command.protocol_error.empty()){
                    reply += command.protocol_error;
                    break;
                }

                std::string command_reply = commands::execute_command(
                    command.arguments, database);

                if(!aof.append_after_command(command.arguments, command_reply,
                                             database, aof_error)){
                    std::cerr << aof_error << '\n';
                    exit_status = 1;
                    break;
                }

                reply += command_reply;
            }
            if(exit_status != 0) break;
            workers[request.worker_index]->add_reply(
                ThreadedReply{request.socket, request.client_id, std::move(reply)});
        }

        if(was_full){
            for(auto& worker : workers) worker->wake();
        }
        if(exit_status != 0 || requests.failed.load(std::memory_order_relaxed)){
            exit_status = 1;
            break;
        }

        database.expire_some(16);
    }

    stop_requested.store(true, std::memory_order_relaxed);
    for(auto& worker : workers) worker->wake();
    for(auto& worker : workers) worker->join();

    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, requests.wake_fd, nullptr);
    
    close(requests.wake_fd);
    if(requests.failed.load(std::memory_order_relaxed)) exit_status = 1;
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
                        std::size_t count = static_cast<std::size_t>(received);
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
item to a normal Ct* degue does not automatically wake Linux's epoll wait . Writing to an eventfd does,
because main registered that descriptor with epoll.

*/