#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace protocol {

    enum class ParseStatus {// enum class gives us named outcomes such as complete, incomplete, or invalid
        complete,
        incomplete,
        invalid
    };

    struct ParseResult {
        ParseStatus status = ParseStatus::incomplete;
        std::vector<std::string> arguments;// like {"set","name","kunal"}
        std::size_t consumed = 0;// how many bytes this request consumes,
        std::string error;// explanation when the request is invalid
    };

    ParseResult parse_request(const std::string& input);

}