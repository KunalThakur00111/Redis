#include "resp_parser.h"
#include <iostream>

namespace protocol{
    ParseResult parse_request(const std::string& input){
        ParseResult result;
        if(input.empty()){
            return result;
        }
        
        if(input[0]!='*'){
            result.status=ParseStatus::invalid;
            result.error="Expected an array";
            return result;
        }
        size_t line_end=input.find("\r\n",1);

        if(line_end == std::string::npos){
            return result;
        }
        // logic
        constexpr std::size_t max_arguments=1024;
        std::size_t argument_count=0;
        if(line_end==1){
            result.status=ParseStatus::invalid;
            result.error="Missing array count";
            return result;
        }

        for(std::size_t i=1;i<line_end;i++){
            char ch=input[i];
            if(ch<'0' || ch>'9'){
                result.status=ParseStatus::invalid;
                result.error="Invalid array count";
                return result;
            }

            std::size_t digit= static_cast<std::size_t>(ch-'0');
            if(argument_count > (max_arguments - digit)/10){
                result.status=ParseStatus::invalid;
                result.error="Too many arguments";
                return result;
            }
            argument_count=argument_count*10+digit;
        }
        if(argument_count==0){
            result.status=ParseStatus::invalid;
            result.error="Command array must not be empty";
            return result;
        }

        std::size_t position = line_end+2;
        constexpr std::size_t max_bulk_length = 64 * 1024;

        for (std::size_t argument = 0; argument < argument_count; argument++) {
            if (position >= input.size()) {
                return result;  // Need more bytes.
            }

            if (input[position] != '$') {
                result.status = ParseStatus::invalid;
                result.error = "Expected a bulk string";
                return result;
            }

            std::size_t length_start = position + 1;
            std::size_t length_end = input.find("\r\n", length_start);

            if (length_end == std::string::npos) {
                return result;
            }

            if (length_end == length_start) {
                result.status = ParseStatus::invalid;
                result.error = "Missing bulk string length";
                return result;
            }

            std::size_t bulk_length = 0;

            for (std::size_t i = length_start; i < length_end; ++i) {
                char ch = input[i];

                if (ch < '0' || ch > '9') {
                    result.status = ParseStatus::invalid;
                    result.error = "Invalid bulk string length";
                    return result;
                }

                std::size_t digit = static_cast<std::size_t>(ch - '0');

                if (bulk_length > (max_bulk_length - digit) / 10) {
                    result.status = ParseStatus::invalid;
                    result.error = "Bulk string too large";
                    return result;
                }

                bulk_length = bulk_length* 10 + digit;
            }

            std::size_t content_start = length_end + 2;

            // Next step: read bulk_length bytes and verify their terminator.
            std::size_t available = input.size() - content_start;

            if (bulk_length > available) {
                return result;  // Content is incomplete.
            }

            if (available - bulk_length < 2) {
                return result;  // Need the two terminator bytes.
            }

            std::size_t content_end = content_start + bulk_length;

            if (input[content_end] != '\r' ||
                input[content_end + 1] != '\n') {
                result.status = ParseStatus::invalid;
                result.error = "Expected CRLF after bulk string content";
                return result;
            }

            result.arguments.push_back(
                input.substr(content_start, bulk_length)
            );

            position = content_end + 2;
            //return result;  // Temporary: content parsing is not implemented yet.
        }
        result.status=ParseStatus::complete;
        result.consumed=position;
        return result;
    }
}