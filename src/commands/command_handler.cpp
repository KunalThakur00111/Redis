#include "command_handler.h"

namespace commands {

    std::string execute_command(
        const std::vector<std::string>& arguments
    ) {
        if (arguments.empty()) {
            return "-ERR empty command\r\n";
        }

        if (arguments[0] == "PING") {
            if (arguments.size() != 1) {
                return "-ERR wrong number of arguments for 'ping' command\r\n";
            }

            return "+PONG\r\n";
        }

        return "-ERR unknown command\r\n";
    }

}