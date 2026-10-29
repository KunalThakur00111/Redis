// Check that SIGINT exits cleanly and AOF data survives an immediate restart.
#include "test_support.h"

#include <iostream>

int main(int argc, char* argv[]) {
    try {
        TEST_CHECK(argc >= 2);
        test::TemporaryDirectory directory;
        std::string aof_path = directory.path() + "/data.aof";
        std::vector<std::string> options{"--aof", aof_path};
        options.insert(options.end(), argv + 2, argv + argc);

        {
            test::Server server(argv[1], options);
            TEST_CHECK(test::exchange_request(server.port(), {"SET", "name", "Kunal"}) ==
                       "+OK\r\n");
            server.interrupt();
        }
        {
            test::Server server(argv[1], options);
            TEST_CHECK(test::exchange_request(server.port(), {"GET", "name"}) ==
                       "$5\r\nKunal\r\n");
        }
        std::cout << "AOF SIGINT restart test passed\n";
        return 0;
    } catch(const std::exception& error){
        std::cerr << error.what() << '\n';
        return 1;
    }
}
