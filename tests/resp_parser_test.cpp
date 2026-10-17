#include "../src/protocol/resp_parser.h"
#include <cassert>
#include <iostream>
#include <string>

int main() {
    const std::string request =
        "*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n";

    auto complete = protocol::parse_request(request);

    assert(complete.status == protocol::ParseStatus::complete);
    assert(complete.arguments.size() == 2);
    assert(complete.arguments[0] == "ECHO");
    assert(complete.arguments[1] == "hello");
    assert(complete.consumed == request.size());

    // Every shorter prefix should require more bytes.
    for (std::size_t length = 0; length < request.size(); ++length) {
        auto partial = protocol::parse_request(request.substr(0, length));

        assert(partial.status == protocol::ParseStatus::incomplete);
        assert(partial.consumed == 0);
    }

    // Parse only the first request when two arrive together.
    auto pipelined = protocol::parse_request(request + request);

    assert(pipelined.status == protocol::ParseStatus::complete);
    assert(pipelined.arguments == complete.arguments);
    assert(pipelined.consumed == request.size());

    // Content must be followed by CRLF.
    auto invalid = protocol::parse_request("*1\r\n$4\r\nPINGxx");

    assert(invalid.status == protocol::ParseStatus::invalid);
    assert(invalid.consumed == 0);

    std::cout << "Parser tests passed\n";
}