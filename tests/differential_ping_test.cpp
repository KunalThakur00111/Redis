// Compare one RESP2 PING reply with Redis Open Source 8.2.9.
#include "test_support.h"

#include <iostream>

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc == 3);
        test::Server ours(argv[1]);
        test::Server reference(argv[2], {"--bind", "127.0.0.1", "--save", "",
                                         "--appendonly", "no"});
        const std::string ping = test::request({"PING"});
        auto reply = [&](std::uint16_t port){
            test::Fd connection = test::connect_to(port);
            test::send_all(connection.get(), ping);
            std::string result;
            while(result.size() < 2 || result.substr(result.size() - 2) != "\r\n")
                result += test::read_exact(connection.get(), 1);
            return result;
        };
        std::string our_reply = reply(ours.port());
        std::string reference_reply = reply(reference.port());
        TEST_CHECK(reference_reply == "+PONG\r\n");
        TEST_CHECK(our_reply == reference_reply);
        std::cout << "PING reply matches Redis Open Source 8.2.9\n";
        return 0;
    } catch(const std::exception& error){
        std::cerr << error.what() << '\n';
        return 1;
    }
}
