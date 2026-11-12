#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace test {

inline void require(bool condition, const char* expression, const char* file, int line) {
    if(!condition) throw std::runtime_error(std::string(file) + ":" +
                                            std::to_string(line) + ": " + expression);
}

#define TEST_CHECK(expression) ::test::require(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

class Fd {
    int value_ = -1;
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { if(value_ >= 0) close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if(this != &other){
            if(value_ >= 0) close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    int get() const { return value_; }
};

inline std::uint16_t available_port() {
    Fd socket_fd(socket(AF_INET, SOCK_STREAM, 0));
    if(socket_fd.get() < 0) throw std::runtime_error("socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if(bind(socket_fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
        throw std::runtime_error("port probe bind failed");
    socklen_t length = sizeof(address);
    if(getsockname(socket_fd.get(), reinterpret_cast<sockaddr*>(&address), &length) < 0)
        throw std::runtime_error("port probe getsockname failed");
    return ntohs(address.sin_port);
}

inline Fd connect_to(std::uint16_t port, int timeout_seconds = 2) {
    Fd socket_fd(socket(AF_INET, SOCK_STREAM, 0));
    if(socket_fd.get() < 0) throw std::runtime_error("socket failed");
    timeval timeout{timeout_seconds, 0};
    if(setsockopt(socket_fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
       setsockopt(socket_fd.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
        throw std::runtime_error("socket timeout setup failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if(connect(socket_fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
        throw std::runtime_error(std::string("connect failed: ") + std::strerror(errno));
    return socket_fd;
}

inline void send_all(int fd, std::string_view bytes) {
    while(!bytes.empty()){
        ssize_t count = send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if(count < 0 && errno == EINTR) continue;
        if(count <= 0) throw std::runtime_error("send failed");
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
}

inline std::string read_exact(int fd, std::size_t size) {
    std::string result(size, '\0');
    std::size_t received = 0;
    while(received < size){
        ssize_t count = recv(fd, result.data() + received, size - received, 0);
        if(count < 0 && errno == EINTR) continue;
        if(count <= 0) throw std::runtime_error("connection closed before full reply");
        received += static_cast<std::size_t>(count);
    }
    return result;
}

inline std::string read_to_eof(int fd) {
    std::string result;
    char buffer[65536];
    while(true){
        ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if(count > 0) result.append(buffer, static_cast<std::size_t>(count));
        else if(count == 0) return result;
        else if(errno != EINTR) throw std::runtime_error("receive failed");
    }
}

inline std::string request(std::initializer_list<std::string_view> arguments) {
    std::string result = "*" + std::to_string(arguments.size()) + "\r\n";
    for(std::string_view argument : arguments){
        result += "$" + std::to_string(argument.size()) + "\r\n";
        result.append(argument.data(), argument.size());
        result += "\r\n";
    }
    return result;
}

inline std::string exchange(std::uint16_t port, std::string_view payload) {
    Fd connection = connect_to(port);
    send_all(connection.get(), payload);
    if(shutdown(connection.get(), SHUT_WR) < 0)
        throw std::runtime_error("socket shutdown failed");
    return read_to_eof(connection.get());
}

inline std::string exchange_request(std::uint16_t port,
                                    std::initializer_list<std::string_view> arguments) {
    return test::exchange(port, request(arguments));
}

class Server {
    pid_t pid_ = -1;
    std::uint16_t port_;

    bool exited(int& status) {
        if(pid_ < 0) return true;
        pid_t result = waitpid(pid_, &status, WNOHANG);
        if(result == pid_){ pid_ = -1; return true; }
        if(result < 0 && errno != EINTR) throw std::runtime_error("waitpid failed");
        return false;
    }

public:
    Server(const std::string& binary, std::vector<std::string> options = {})
        : port_(available_port()) {
        options.insert(options.begin(), {binary, "--port", std::to_string(port_)});
        pid_ = fork();
        if(pid_ < 0) throw std::runtime_error("fork failed");
        if(pid_ == 0){
            Fd null_fd(open("/dev/null", O_WRONLY));
            if(null_fd.get() >= 0) dup2(null_fd.get(), STDOUT_FILENO);
            std::vector<char*> arguments;
            for(auto& option : options) arguments.push_back(option.data());
            arguments.push_back(nullptr);
            execv(binary.c_str(), arguments.data());
            _exit(127);
        }
        try {
            // Up to 10 s: replaying a large AOF under sanitizers can take a few seconds.
            for(int attempt = 0; attempt < 500; attempt++){
                int status = 0;
                if(exited(status)) throw std::runtime_error("server exited during startup");
                try {
                    Fd probe = connect_to(port_);
                    return;
                } catch(const std::runtime_error&){
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
            throw std::runtime_error("server did not start");
        } catch(...){
            stop();
            throw;
        }
    }
    ~Server() { stop(); }
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    std::uint16_t port() const { return port_; }
    pid_t pid() const { return pid_; }

    int wait_for_exit(int timeout_ms) {
        for(int elapsed = 0; elapsed < timeout_ms; elapsed += 10){
            int status = 0;
            if(exited(status)) return status;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("server did not exit in time");
    }

    void interrupt() {
        if(pid_ < 0) throw std::runtime_error("server already exited");
        if(kill(pid_, SIGINT) < 0) throw std::runtime_error("SIGINT failed");
        int status = wait_for_exit(5000);
        TEST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    void stop() noexcept {
        if(pid_ < 0) return;
        kill(pid_, SIGTERM);
        for(int attempt = 0; attempt < 200; attempt++){
            int status = 0;
            pid_t result = waitpid(pid_, &status, WNOHANG);
            if(result == pid_ || (result < 0 && errno == ECHILD)){
                pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        kill(pid_, SIGKILL);
        int status = 0;
        while(waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
    }
};

class TemporaryDirectory {
    std::string path_;
public:
    TemporaryDirectory() {
        char pattern[] = "/tmp/redis-test-XXXXXX";
        char* result = mkdtemp(pattern);
        if(!result) throw std::runtime_error("mkdtemp failed");
        path_ = result;
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    const std::string& path() const { return path_; }
};

} // namespace test
