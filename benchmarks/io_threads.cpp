// Compare one and four I/O threads with the same pipelined PING workload.
#include "../tests/test_support.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <iomanip>
#include <iostream>
#include <netinet/tcp.h>
#include <numeric>

namespace {
constexpr int clients = 16;
constexpr int pipeline = 32;
constexpr int batches = 2000;
constexpr int trials = 3;

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    std::size_t middle = values.size() / 2;
    if(values.size() % 2 == 0) return (values[middle - 1] + values[middle]) / 2;
    return values[middle];
}

std::vector<double> run_client(std::uint16_t port) {
    test::Fd connection = test::connect_to(port, 10);
    int no_delay = 1;
    TEST_CHECK(setsockopt(connection.get(), IPPROTO_TCP, TCP_NODELAY,
                          &no_delay, sizeof(no_delay)) == 0);
    std::string payload;
    std::string expected;
    for(int i = 0; i < pipeline; i++){
        payload += test::request({"PING"});
        expected += "+PONG\r\n";
    }
    std::vector<double> round_trips;
    round_trips.reserve(batches);
    for(int i = 0; i < batches; i++){
        auto start = std::chrono::steady_clock::now();
        test::send_all(connection.get(), payload);
        TEST_CHECK(test::read_exact(connection.get(), expected.size()) == expected);
        auto elapsed = std::chrono::steady_clock::now() - start;
        round_trips.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
    }
    return round_trips;
}

struct Result {
    double throughput;
    double median_rtt;
    double p95_rtt;
};

Result measure(const std::string& binary, int io_threads) {
    test::Server server(binary, {"--io-threads", std::to_string(io_threads)});
    auto start = std::chrono::steady_clock::now();
    std::vector<std::future<std::vector<double>>> jobs;
    for(int i = 0; i < clients; i++)
        jobs.push_back(std::async(std::launch::async, run_client, server.port()));
    std::vector<double> round_trips;
    for(auto& job : jobs){
        auto client = job.get();
        round_trips.insert(round_trips.end(), client.begin(), client.end());
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    std::sort(round_trips.begin(), round_trips.end());
    return {clients * pipeline * batches /
                std::chrono::duration<double>(elapsed).count(),
            median(round_trips), round_trips[static_cast<std::size_t>(round_trips.size() * 0.95)]};
}
}

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc == 2);
        std::cout << "Workload: " << clients << " persistent clients, " << pipeline
                  << " PINGs per pipeline, " << batches << " pipelines per client, "
                  << trials << " trials\n" << std::fixed << std::setprecision(3);
        for(int io_threads : {1, 4}){
            std::vector<double> throughputs, medians, p95s;
            for(int trial = 0; trial < trials; trial++){
                Result result = measure(argv[1], io_threads);
                throughputs.push_back(result.throughput);
                medians.push_back(result.median_rtt);
                p95s.push_back(result.p95_rtt);
            }
            std::cout << "io-threads=" << io_threads
                      << ": throughput median=" << std::setprecision(0)
                      << median(throughputs) << " ops/s; pipeline RTT median="
                      << std::setprecision(3) << median(medians)
                      << " ms; pipeline RTT p95 median=" << median(p95s)
                      << " ms; throughput trials=[";
            for(std::size_t i = 0; i < throughputs.size(); i++){
                if(i) std::cout << ", ";
                std::cout << std::setprecision(0) << throughputs[i];
            }
            std::cout << "]\n" << std::setprecision(3);
        }
        return 0;
    } catch(const std::exception& error){
        std::cerr << error.what() << '\n';
        return 1;
    }
}
