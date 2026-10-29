#include "../tests/test_support.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <future>
#include <iomanip>
#include <iostream>
#include <netinet/tcp.h>
#include <string_view>

namespace {
constexpr int clients = 16;
constexpr int default_pipeline = 32;
constexpr int default_batches = 40;

struct Case {
    const char* name;
    std::vector<std::string> command;
    std::vector<std::vector<std::string>> setup;
};

const std::vector<Case> cases = {
    {"PING", {"PING", "@VALUE@"}, {}},
    {"ECHO", {"ECHO", "@VALUE@"}, {}},
    {"SET", {"SET", "@KEY@", "@VALUE@"}, {}},
    {"MSET", {"MSET", "@KEY@", "@VALUE@", "@OTHER@", "@VALUE@"}, {}},
    {"GET", {"GET", "@KEY@"}, {{"SET", "@KEY@", "@VALUE@"}}},
    {"DEL", {"DEL", "@KEY@"}, {{"SET", "@KEY@", "@VALUE@"}}},
    {"EXISTS", {"EXISTS", "@KEY@"}, {{"SET", "@KEY@", "@VALUE@"}}},
    {"DBSIZE", {"DBSIZE"}, {}},
    {"MGET", {"MGET", "@KEY@", "@OTHER@"},
        {{"SET", "@KEY@", "@VALUE@"}, {"SET", "@OTHER@", "@VALUE@"}}},
    {"EXPIRE", {"EXPIRE", "@KEY@", "3600"}, {{"SET", "@KEY@", "@VALUE@"}}},
    {"PEXPIRE", {"PEXPIRE", "@KEY@", "3600000"}, {{"SET", "@KEY@", "@VALUE@"}}},
    {"EXPIREAT", {"EXPIREAT", "@KEY@", "4102444800"},
        {{"SET", "@KEY@", "@VALUE@"}}},
    {"PEXPIREAT", {"PEXPIREAT", "@KEY@", "4102444800000"},
        {{"SET", "@KEY@", "@VALUE@"}}},
    {"TTL", {"TTL", "@KEY@"},
        {{"SET", "@KEY@", "@VALUE@"}, {"EXPIRE", "@KEY@", "3600"}}},
    {"PTTL", {"PTTL", "@KEY@"},
        {{"SET", "@KEY@", "@VALUE@"}, {"EXPIRE", "@KEY@", "3600"}}},
    {"PERSIST", {"PERSIST", "@KEY@"},
        {{"SET", "@KEY@", "@VALUE@"}, {"EXPIRE", "@KEY@", "3600"}}},
    {"EXPIRETIME", {"EXPIRETIME", "@KEY@"},
        {{"SET", "@KEY@", "@VALUE@"}, {"EXPIRE", "@KEY@", "3600"}}},
    {"PEXPIRETIME", {"PEXPIRETIME", "@KEY@"},
        {{"SET", "@KEY@", "@VALUE@"}, {"EXPIRE", "@KEY@", "3600"}}},
    {"GETDEL", {"GETDEL", "@KEY@"}, {{"SET", "@KEY@", "@VALUE@"}}},
    {"INCR", {"INCR", "@KEY@"}, {{"SET", "@KEY@", "0"}}},
    {"DECR", {"DECR", "@KEY@"}, {{"SET", "@KEY@", "0"}}},
    {"INCRBY", {"INCRBY", "@KEY@", "2"}, {{"SET", "@KEY@", "0"}}},
    {"DECRBY", {"DECRBY", "@KEY@", "2"}, {{"SET", "@KEY@", "0"}}},
    {"LPUSH", {"LPUSH", "@KEY@", "@VALUE@"}, {}},
    {"RPUSH", {"RPUSH", "@KEY@", "@VALUE@"}, {}},
    {"LPOP", {"LPOP", "@KEY@"}, {{"RPUSH", "@KEY@", "@VALUE@"}}},
    {"RPOP", {"RPOP", "@KEY@"}, {{"RPUSH", "@KEY@", "@VALUE@"}}},
    {"LRANGE", {"LRANGE", "@KEY@", "0", "-1"},
        {{"RPUSH", "@KEY@", "@VALUE@"}}},
    {"LLEN", {"LLEN", "@KEY@"}, {{"RPUSH", "@KEY@", "@VALUE@"}}},
    {"LINDEX", {"LINDEX", "@KEY@", "0"}, {{"RPUSH", "@KEY@", "@VALUE@"}}},
    {"HSET", {"HSET", "@KEY@", "@FIELD@", "@VALUE@"}, {}},
    {"HGET", {"HGET", "@KEY@", "@FIELD@"},
        {{"HSET", "@KEY@", "@FIELD@", "@VALUE@"}}},
    {"HDEL", {"HDEL", "@KEY@", "@FIELD@"},
        {{"HSET", "@KEY@", "@FIELD@", "@VALUE@"}}},
    {"HLEN", {"HLEN", "@KEY@"},
        {{"HSET", "@KEY@", "@FIELD@", "@VALUE@"}}},
    {"SADD", {"SADD", "@KEY@", "@VALUE@"}, {}},
    {"SREM", {"SREM", "@KEY@", "@VALUE@"},
        {{"SADD", "@KEY@", "@VALUE@"}}},
    {"SISMEMBER", {"SISMEMBER", "@KEY@", "@VALUE@"},
        {{"SADD", "@KEY@", "@VALUE@"}}},
    {"SCARD", {"SCARD", "@KEY@"}, {{"SADD", "@KEY@", "@VALUE@"}}},
    {"SMEMBERS", {"SMEMBERS", "@KEY@"},
        {{"SADD", "@KEY@", "@VALUE@"}}},
};

std::int64_t parse_number(std::string_view text) {
    std::int64_t result = 0;
    auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    TEST_CHECK(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size());
    return result;
}

bool parse_reply(const std::string& bytes, std::size_t& position, int depth = 0) {
    TEST_CHECK(depth < 16);
    const std::size_t start = position;
    if(position == bytes.size()) return false;
    const char kind = bytes[position++];
    const std::size_t end = bytes.find("\r\n", position);
    if(end == std::string::npos){ position = start; return false; }
    const std::string_view line(bytes.data() + position, end - position);
    position = end + 2;
    if(kind == '+' || kind == '-' || kind == ':') return true;
    TEST_CHECK(kind == '$' || kind == '*');
    const std::int64_t count = parse_number(line);
    TEST_CHECK(count >= -1);
    if(count == -1) return true;
    if(kind == '$'){
        const std::size_t size = static_cast<std::size_t>(count);
        if(bytes.size() - position < size + 2){ position = start; return false; }
        TEST_CHECK(bytes.compare(position + size, 2, "\r\n") == 0);
        position += size + 2;
        return true;
    }
    for(std::int64_t i = 0; i < count; i++){
        if(!parse_reply(bytes, position, depth + 1)){
            position = start;
            return false;
        }
    }
    return true;
}

void read_replies(int fd, int expected_count) {
    std::string pending;
    int received = 0;
    while(received < expected_count){
        std::size_t consumed = 0;
        while(received < expected_count){
            const std::size_t start = consumed;
            if(!parse_reply(pending, consumed)) break;
            TEST_CHECK(pending[start] != '-');
            received++;
        }
        pending.erase(0, consumed);
        if(received == expected_count){
            TEST_CHECK(pending.empty());
            return;
        }
        char buffer[65536];
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if(count < 0 && errno == EINTR) continue;
        TEST_CHECK(count > 0);
        pending.append(buffer, static_cast<std::size_t>(count));
    }
}

std::vector<std::string> substitute(const std::vector<std::string>& arguments,
                                     const std::string& key, const std::string& other,
                                     const std::string& field, const std::string& value) {
    std::vector<std::string> result;
    for(const auto& argument : arguments){
        if(argument == "@KEY@") result.push_back(key);
        else if(argument == "@OTHER@") result.push_back(other);
        else if(argument == "@FIELD@") result.push_back(field);
        else if(argument == "@VALUE@") result.push_back(value);
        else result.push_back(argument);
    }
    return result;
}

std::string request(const std::vector<std::string>& arguments) {
    std::string result = "*" + std::to_string(arguments.size()) + "\r\n";
    for(const auto& argument : arguments){
        result += "$" + std::to_string(argument.size()) + "\r\n";
        result += argument;
        result += "\r\n";
    }
    return result;
}

std::vector<double> run_client(std::uint16_t port, const Case& spec,
                                int client_index, int value_size,
                                int pipeline, int batches) {
    test::Fd connection = test::connect_to(port, 10);
    int no_delay = 1;
    TEST_CHECK(setsockopt(connection.get(), IPPROTO_TCP, TCP_NODELAY,
                          &no_delay, sizeof(no_delay)) == 0);
    const std::string key = std::string(spec.name) + ":" +
                            std::to_string(client_index) + ":" +
                            std::string(static_cast<std::size_t>(value_size), 'k');
    const std::string other = key + ":other";
    const std::string field(static_cast<std::size_t>(value_size), 'f');
    const std::string value(static_cast<std::size_t>(value_size), 'v');
    for(const auto& setup : spec.setup){
        test::send_all(connection.get(), request(
            substitute(setup, key, other, field, value)));
        read_replies(connection.get(), 1);
    }
    const std::string command = request(
        substitute(spec.command, key, other, field, value));
    std::string payload;
    for(int i = 0; i < pipeline; i++) payload += command;
    std::vector<double> round_trips;
    round_trips.reserve(batches);
    for(int i = 0; i < batches; i++){
        const auto start = std::chrono::steady_clock::now();
        test::send_all(connection.get(), payload);
        read_replies(connection.get(), pipeline);
        round_trips.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
    return round_trips;
}

void measure(const std::string& binary, int io_threads, int value_size,
             int pipeline, int batches) {
    test::Server server(binary, {"--io-threads", std::to_string(io_threads)});
    std::cout << "io-threads=" << io_threads << '\n';
    std::vector<double> overall_times;
    overall_times.reserve(cases.size() * clients * batches);
    for(const Case& spec : cases){
        std::vector<std::future<std::vector<double>>> jobs;
        const auto start = std::chrono::steady_clock::now();
        for(int i = 0; i < clients; i++)
            jobs.push_back(std::async(std::launch::async, run_client,
                                      server.port(), std::cref(spec), i, value_size,
                                      pipeline, batches));
        std::vector<double> times;
        for(auto& job : jobs){
            auto client_times = job.get();
            times.insert(times.end(), client_times.begin(), client_times.end());
        }
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        overall_times.insert(overall_times.end(), times.begin(), times.end());
        std::sort(times.begin(), times.end());
        const auto slow = std::count_if(times.begin(), times.end(),
                                        [](double time){ return time >= 20.0; });
        std::cout << std::left << std::setw(14) << spec.name << std::right
                  << " p50=" << std::fixed << std::setprecision(2)
                  << times[times.size() / 2] << "ms p95="
                  << times[static_cast<std::size_t>(times.size() * 0.95)]
                  << "ms p99="
                  << times[static_cast<std::size_t>(times.size() * 0.99)]
                  << "ms >=20ms=" << slow << "/" << times.size()
                  << " throughput=" << std::setprecision(0)
                  << clients * pipeline * batches / seconds << " ops/s\n";
        std::cout.flush();
    }
    std::sort(overall_times.begin(), overall_times.end());
    const auto slow = std::count_if(overall_times.begin(), overall_times.end(),
                                    [](double time){ return time >= 20.0; });
    std::cout << "OVERALL        p50=" << std::fixed << std::setprecision(2)
              << overall_times[overall_times.size() / 2] << "ms p95="
              << overall_times[static_cast<std::size_t>(overall_times.size() * 0.95)]
              << "ms p99="
              << overall_times[static_cast<std::size_t>(overall_times.size() * 0.99)]
              << "ms >=20ms=" << slow << "/" << overall_times.size() << '\n';
}
}

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc >= 2 && argc <= 5);
        const int value_size = argc >= 3 ? std::stoi(argv[2]) : 128;
        const int pipeline = argc >= 4 ? std::stoi(argv[3]) : default_pipeline;
        const int batches = argc == 5 ? std::stoi(argv[4]) : default_batches;
        TEST_CHECK(value_size >= 1 && value_size <= 1024);
        TEST_CHECK(pipeline >= 1 && pipeline <= 64);
        TEST_CHECK(batches >= 1 && batches <= 10000);
        std::cout << cases.size() << " commands, " << clients
                  << " clients, " << pipeline << " commands per pipeline, "
                  << batches << " pipelines per client, " << value_size
                  << "-byte keys/values, AOF off\n";
        for(int io_threads : {1, 4})
            measure(argv[1], io_threads, value_size, pipeline, batches);
        return 0;
    } catch(const std::exception& error){
        std::cerr << error.what() << '\n';
        return 1;
    }
}
