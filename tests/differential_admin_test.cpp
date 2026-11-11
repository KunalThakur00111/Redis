// Compare raw RESP2 replies for the admin commands (TYPE, KEYS, SCAN, RENAME,
// FLUSHDB/FLUSHALL, SELECT, COMMAND, CONFIG, INFO) with Redis Open Source 8.2.9.
// Both servers receive the same commands in the same order on one connection
// each. Every mismatch is printed, then the test fails if there was any.
#include "test_support.h"

#include <algorithm>
#include <iostream>

namespace {

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
std::string sorted_array(const std::string& reply) {
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
std::string sorted_scan(const std::string& reply) {
    if(reply.rfind("*2\r\n", 0) != 0) return reply;
    std::size_t position = 4;
    const std::size_t end = reply.find("\r\n", position);
    const std::size_t length = std::stoul(reply.substr(position + 1, end - position - 1));
    position = end + 2 + length + 2;
    return reply.substr(0, position) + sorted_array(reply.substr(position));
}

std::string printable(const std::string& text) {
    std::string result;
    for(char ch : text){
        if(ch == '\r') result += "\\r";
        else if(ch == '\n') result += "\\n";
        else result += ch;
    }
    return result;
}

enum class Compare { exact, sorted, scan };

struct Case {
    std::vector<std::string> command;
    Compare compare = Compare::exact;
};

}  // namespace

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc == 3);
        test::Server ours(argv[1]);
        test::Server reference(argv[2], {"--bind", "127.0.0.1", "--save", "",
                                         "--appendonly", "no"});
        test::Fd our_connection = test::connect_to(ours.port());
        test::Fd reference_connection = test::connect_to(reference.port());
        ReplyReader our_replies(our_connection.get());
        ReplyReader reference_replies(reference_connection.get());

        const std::vector<Case> cases{
            {{"FLUSHALL"}},
            {{"SET", "s", "v"}}, {{"RPUSH", "l", "a"}}, {{"HSET", "h", "f", "v"}},
            {{"SADD", "z", "m"}},

            {{"TYPE", "s"}}, {{"type", "l"}}, {{"TYPE", "h"}}, {{"TYPE", "z"}},
            {{"TYPE", "missing"}}, {{"TYPE"}}, {{"TYPE", "a", "b"}},

            {{"KEYS", "*"}, Compare::sorted}, {{"KEYS", "[hl]"}, Compare::sorted},
            {{"KEYS", "nothing*"}}, {{"KEYS", "?"}, Compare::sorted}, {{"KEYS"}},
            {{"KEYS", "[^s]"}, Compare::sorted}, {{"KEYS", "\\s"}},

            {{"SCAN"}}, {{"SCAN", "abc"}}, {{"SCAN", "-1"}}, {{"SCAN", "+0"}, Compare::scan},
            {{"SCAN", "0", "COUNT", "0"}}, {{"SCAN", "0", "COUNT", "-1"}},
            {{"SCAN", "0", "COUNT", "x"}}, {{"SCAN", "0", "MATCH"}},
            {{"SCAN", "0", "BOGUS", "1"}}, {{"SCAN", "+"}}, {{"SCAN", "++0"}},
            {{"SCAN", "0", "COUNT", "100"}, Compare::scan},
            {{"SCAN", "0", "MATCH", "[hs]", "COUNT", "100"}, Compare::scan},
            {{"SCAN", "0", "TYPE", "string", "COUNT", "100"}, Compare::scan},
            {{"SCAN", "0", "TYPE", "LIST", "COUNT", "100"}, Compare::scan},
            {{"SCAN", "0", "TYPE", "bogus", "COUNT", "100"}, Compare::scan},

            {{"RENAME", "missing", "x"}}, {{"RENAME", "s", "s"}}, {{"RENAME", "s", "t"}},
            {{"GET", "t"}}, {{"EXISTS", "s"}}, {{"RENAMENX", "t", "l"}},
            {{"RENAMENX", "t", "t"}}, {{"RENAMENX", "missing", "x"}},
            {{"RENAMENX", "t", "u"}}, {{"RENAME", "u", "l"}}, {{"TYPE", "l"}},
            {{"RENAME", "a"}}, {{"RENAMENX", "a", "b", "c"}},
            {{"SET", "e", "v", "EX", "1000"}}, {{"RENAME", "e", "e2"}}, {{"TTL", "e2"}},

            {{"FLUSHALL", "now"}}, {{"FLUSHDB", "a", "b"}}, {{"FLUSHDB", "SYNC"}},
            {{"DBSIZE"}}, {{"SET", "k", "v"}}, {{"FLUSHALL", "async"}}, {{"DBSIZE"}},

            {{"SELECT", "0"}}, {{"SELECT", "x"}}, {{"SELECT", "-1"}}, {{"SELECT", "16"}},
            {{"SELECT"}}, {{"SELECT", "0", "1"}},

            {{"COMMAND", "bad\r\nx"}}, {{"COMMAND", "COUNT", "extra"}},
            {{"COMMAND", "LIST", "extra"}}, {{"COMMAND", "LIST", "FILTERBY"}},

            {{"CONFIG"}}, {{"CONFIG", "GET"}}, {{"CONFIG", "GET", "nope"}},
            {{"CONFIG", "GET", "appendonly"}}, {{"CONFIG", "GET", "APPENDONLY"}},
            {{"CONFIG", "GET", "maxmemory"}}, {{"CONFIG", "BAD"}},
            {{"CONFIG", "GET", "AppendOnly"}}, {{"CONFIG", "GET", "APPENDONL?"}},
            {{"CONFIG", "GET", "appendonly", "appendonly"}},

            {{"INFO", "nosuchsection"}},
        };

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
            if(item.compare == Compare::sorted){
                mine = sorted_array(mine);
                theirs = sorted_array(theirs);
            }
            else if(item.compare == Compare::scan){
                mine = sorted_scan(mine);
                theirs = sorted_scan(theirs);
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
                  << cases.size() << " replies match Redis Open Source 8.2.9\n";
        return mismatches == 0 ? 0 : 1;
    } catch(const std::exception& error){
        std::cerr << error.what() << '\n';
        return 1;
    }
}
