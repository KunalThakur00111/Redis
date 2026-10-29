// Exercise ordered replies and one shared database through several I/O workers.
#include "test_support.h"

#include <algorithm>
#include <future>
#include <iostream>
#include <numeric>

namespace {
constexpr int clients = 32;
constexpr int commands_per_client = 128;

std::vector<int> increment_many(std::uint16_t port) {
    std::string payload;
    const std::string command = test::request({"INCR", "counter"});
    for(int i = 0; i < commands_per_client; i++) payload += command;
    test::Fd connection = test::connect_to(port, 10);
    test::send_all(connection.get(), payload);
    TEST_CHECK(shutdown(connection.get(), SHUT_WR) == 0);
    std::string reply = test::read_to_eof(connection.get());
    std::vector<int> values;
    std::size_t position = 0;
    while(position < reply.size()){
        TEST_CHECK(reply[position] == ':');
        std::size_t end = reply.find("\r\n", position);
        TEST_CHECK(end != std::string::npos);
        values.push_back(std::stoi(reply.substr(position + 1, end - position - 1)));
        position = end + 2;
    }
    TEST_CHECK(values.size() == commands_per_client);
    TEST_CHECK(std::is_sorted(values.begin(), values.end()));
    return values;
}
}

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc >= 2);
        test::TemporaryDirectory directory;
        std::string aof_path = directory.path() + "/data.aof";
        std::vector<std::string> options{"--aof", aof_path};
        options.insert(options.end(), argv + 2, argv + argc);
        const int total = clients * commands_per_client;

        {
            test::Server server(argv[1], options);
            std::vector<std::future<std::vector<int>>> jobs;
            for(int i = 0; i < clients; i++)
                jobs.push_back(std::async(std::launch::async, increment_many, server.port()));
            std::vector<int> values;
            for(auto& job : jobs){
                auto client_values = job.get();
                values.insert(values.end(), client_values.begin(), client_values.end());
            }
            std::sort(values.begin(), values.end());
            TEST_CHECK(values.size() == static_cast<std::size_t>(total));
            for(int i = 0; i < total; i++) TEST_CHECK(values[i] == i + 1);
            TEST_CHECK(test::exchange_request(server.port(), {"GET", "counter"}) ==
                       "$4\r\n" + std::to_string(total) + "\r\n");
            server.interrupt();
        }
        {
            test::Server server(argv[1], options);
            TEST_CHECK(test::exchange_request(server.port(), {"GET", "counter"}) ==
                       "$4\r\n" + std::to_string(total) + "\r\n");
        }
        std::cout << "Threaded I/O test passed\n";
        return 0;
    } catch(const std::exception& error){
        std::cerr << error.what() << '\n';
        return 1;
    }
}
