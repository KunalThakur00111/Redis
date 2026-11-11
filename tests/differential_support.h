// Shared code for the tests that compare raw RESP2 replies with Redis 8.2.9.
#pragma once

#include "test_support.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace differential {

// Reads whole RESP2 replies from one connection.
class ReplyReader {
    int fd_;
    std::string buffer_;

    void fill() {
        char chunk[4096];
        while(true){
            const ssize_t count = recv(fd_, chunk, sizeof(chunk), 0);
            if(count > 0){
                buffer_.append(chunk, static_cast<std::size_t>(count));
                return;
            }
            if(count < 0 && errno == EINTR) continue;
            throw std::runtime_error("connection closed before full reply");
        }
    }

    std::string line(std::size_t& position) {
        while(buffer_.find("\r\n", position) == std::string::npos) fill();
        const std::size_t end = buffer_.find("\r\n", position);
        std::string text = buffer_.substr(position, end - position);
        position = end + 2;
        return text;
    }

    // Parse one value at position, reading more bytes as needed.
    void value(std::size_t& position) {
        while(position >= buffer_.size()) fill();
        const char type = buffer_[position];
        const std::string header = line(position);
        if(type == '+' || type == '-' || type == ':') return;
        const long long length = std::stoll(header.substr(1));
        if(type == '$'){
            if(length < 0) return;
            while(buffer_.size() < position + static_cast<std::size_t>(length) + 2) fill();
            position += static_cast<std::size_t>(length) + 2;
            return;
        }
        if(type == '*'){
            for(long long i = 0; i < length; i++) value(position);
            return;
        }
        throw std::runtime_error("unexpected RESP type");
    }

public:
    explicit ReplyReader(int fd) : fd_(fd) {}

    std::string next() {
        std::size_t position = 0;
        value(position);
        std::string reply = buffer_.substr(0, position);
        buffer_.erase(0, position);
        return reply;
    }
};

// Split a flat array of bulk strings into its encoded elements, sort them, and
// rebuild it, so replies whose order is unspecified (KEYS, SCAN keys) compare equal.
inline std::string sorted_array(const std::string& reply) {
    if(reply.empty() || reply[0] != '*') return reply;
    std::size_t position = reply.find("\r\n") + 2;
    std::vector<std::string> elements;
    while(position < reply.size()){
        const std::size_t end = reply.find("\r\n", position);
        const long long length = std::stoll(reply.substr(position + 1, end - position - 1));
        const std::size_t size = end + 2 - position + (length < 0 ? 0 : static_cast<std::size_t>(length) + 2);
        elements.push_back(reply.substr(position, size));
        position += size;
    }
    std::sort(elements.begin(), elements.end());
    std::string rebuilt = reply.substr(0, reply.find("\r\n") + 2);
    for(const auto& element : elements) rebuilt += element;
    return rebuilt;
}

// SCAN replies: keep the cursor, sort the keys.
inline std::string sorted_scan(const std::string& reply) {
    if(reply.rfind("*2\r\n", 0) != 0) return reply;
    std::size_t position = 4;
    const std::size_t end = reply.find("\r\n", position);
    const std::size_t length = std::stoul(reply.substr(position + 1, end - position - 1));
    position = end + 2 + length + 2;
    return reply.substr(0, position) + sorted_array(reply.substr(position));
}

inline std::string printable(const std::string& text) {
    std::string result;
    for(char ch : text){
        if(ch == '\r') result += "\\r";
        else if(ch == '\n') result += "\\n";
        else result += ch;
    }
    return result;
}

// exact: byte for byte. sorted: a flat array whose order is unspecified.
// scan: a SCAN reply, cursor kept and keys sorted. pairs: a flat field, value
// array (HGETALL) sorted by pair. shape: only the first byte, the reply type,
// for replies that are random by design.
enum class Compare { exact, sorted, scan, pairs, shape };

struct Case {
    std::vector<std::string> command;
    Compare compare = Compare::exact;
};

// HGETALL replies: sort field, value pairs as units.
inline std::string sorted_pairs(const std::string& reply) {
    if(reply.empty() || reply[0] != '*') return reply;
    std::size_t position = reply.find("\r\n") + 2;
    std::vector<std::string> elements;
    while(position < reply.size()){
        const std::size_t end = reply.find("\r\n", position);
        const long long length = std::stoll(reply.substr(position + 1, end - position - 1));
        const std::size_t size = end + 2 - position + (length < 0 ? 0 : static_cast<std::size_t>(length) + 2);
        elements.push_back(reply.substr(position, size));
        position += size;
    }
    std::vector<std::string> pairs;
    for(std::size_t i = 0; i + 1 < elements.size(); i += 2) pairs.push_back(elements[i] + elements[i + 1]);
    std::sort(pairs.begin(), pairs.end());
    std::string rebuilt = reply.substr(0, reply.find("\r\n") + 2);
    for(const auto& pair : pairs) rebuilt += pair;
    return rebuilt;
}

// Send each case to both servers, in order, on one connection each, and print
// every reply that differs. Returns the process exit status.
inline int run(const char* ours_binary, const char* reference_binary,
               const std::vector<Case>& cases, const std::string& what) {
    test::Server ours(ours_binary);
    test::Server reference(reference_binary, {"--bind", "127.0.0.1", "--save", "",
                                              "--appendonly", "no"});
    test::Fd our_connection = test::connect_to(ours.port());
    test::Fd reference_connection = test::connect_to(reference.port());
    ReplyReader our_replies(our_connection.get());
    ReplyReader reference_replies(reference_connection.get());

    int mismatches = 0;
    for(const auto& item : cases){
        std::string payload = "*" + std::to_string(item.command.size()) + "\r\n";
        for(const auto& argument : item.command){
            payload += "$" + std::to_string(argument.size()) + "\r\n" + argument + "\r\n";
        }
        test::send_all(our_connection.get(), payload);
        test::send_all(reference_connection.get(), payload);
        std::string mine = our_replies.next();
        std::string theirs = reference_replies.next();
        switch(item.compare){
            case Compare::exact: break;
            case Compare::sorted: mine = sorted_array(mine); theirs = sorted_array(theirs); break;
            case Compare::scan: mine = sorted_scan(mine); theirs = sorted_scan(theirs); break;
            case Compare::pairs: mine = sorted_pairs(mine); theirs = sorted_pairs(theirs); break;
            case Compare::shape: mine = mine.substr(0, 1); theirs = theirs.substr(0, 1); break;
        }
        if(mine != theirs){
            mismatches++;
            std::string shown;
            for(const auto& argument : item.command) shown += " " + printable(argument);
            std::cout << "MISMATCH" << shown << "\n  ours:  " << printable(mine)
                      << "\n  redis: " << printable(theirs) << "\n";
        }
    }
    std::cout << cases.size() - static_cast<std::size_t>(mismatches) << " of "
              << cases.size() << " " << what << " replies match Redis Open Source 8.2.9\n";
    return mismatches == 0 ? 0 : 1;
}

}  // namespace differential
