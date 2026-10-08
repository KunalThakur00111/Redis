#include "server.h"
#include <winsock2.h>
#include <iostream>
#include <vector>
#include "../protocol/resp_parser.h"
#include "../commands/command_handler.h"
namespace net{
    struct Client{
        SOCKET socket;
        std::string input;
        std::string output;
        bool read_closed=false;
    };

    void build_socket_sets(SOCKET listner, const std::vector<Client>& clients, fd_set& readset, fd_set& writeset){
        FD_ZERO(&readset);
        FD_ZERO(&writeset);
        FD_SET(listner,&readset);
        for(const Client& client: clients){
            if(!client.read_closed){
                FD_SET(client.socket, &readset);
            }
            if(!client.output.empty()){
                FD_SET(client.socket, &writeset);
            }
        }
    }

    bool send_pending(Client& client) {
        if (client.output.empty()) {
            return true;
        }

        int bytes_sent = send(
            client.socket,
            client.output.data(),
            static_cast<int>(client.output.size()),
            0
        );

        if (bytes_sent > 0) {
            std::cout << "Sent: " << bytes_sent << " bytes\n";
            client.output.erase(0, static_cast<size_t>(bytes_sent));
            return true;
        }

        if (bytes_sent == SOCKET_ERROR) {
            int error = WSAGetLastError();

            if (error == WSAEWOULDBLOCK) {
                return true;  // Keep pending output; retry when writable.
            }

            std::cerr << "Send failed: " << error << '\n';
            return false;
        }

        // A nonempty send made no progress. Close this client.
        std::cerr << "Send returned zero for nonempty output\n";
        return false;
    }

    bool process_input(Client& client, std::size_t max_pending_output) {
        while (true) {
            auto result = protocol::parse_request(client.input);

            if (result.status == protocol::ParseStatus::incomplete) {
                return true;  // Keep input and wait for more bytes.
            }

            if (result.status == protocol::ParseStatus::invalid) {
                std::cerr << "Protocol error: " << result.error << '\n';
                return false;  // Caller closes this client.
            }

            std::string reply =
                commands::execute_command(result.arguments);

            if (reply.size() > max_pending_output - client.output.size()) {
                std::cerr << "Client output buffer limit reached\n";
                return false;
            }

            client.output += reply;
            client.input.erase(0, result.consumed);
        }
    }

    int run_server(){
        WSADATA data{};
        int result= WSAStartup(MAKEWORD(2,2), &data);// makeword(2,2) request version 2.2
        if(result != 0){
            std::cerr<<"WSAStartup failed: "<<result<<std::endl;
            return 1;
        }
        std::cout<<"WSAstartup successfull...\n";
        SOCKET listner= socket(AF_INET,SOCK_STREAM, 0);

        // AF_INET: use of ivp4 family
        // SOCK_STREAM: use a byte-stream socket
        // 0 : default tcp(if sock_stream)
        if(listner== INVALID_SOCKET){
            int error=WSAGetLastError();
            std::cerr<<" TCP socket creation failed: "<<error<<std::endl;
            WSACleanup();
            return 1;
        }

        std::cout<<"socket created successfully..\n";
        sockaddr_in serversocket{};
        serversocket.sin_family=AF_INET;
        serversocket.sin_port=htons(6379);// hnot for converting the big endian family format
        serversocket.sin_addr.s_addr=htonl(INADDR_ANY);

        if(
            bind(
                listner,
                reinterpret_cast<sockaddr*>(&serversocket),
                sizeof(serversocket)
            )
            ==
            SOCKET_ERROR
        ){
            int error=WSAGetLastError();
            std::cout<<"bind failed: "<<error;
            closesocket(listner);
            WSACleanup();
            return 1;
        }

        if(listen(listner, SOMAXCONN)==SOCKET_ERROR){
            int error=WSAGetLastError();
            std::cerr<<"Listen Failed: "<<error<<std::endl;
            closesocket(listner);
            WSACleanup();
            return 1;
        } // somaxconn means max os waiting queue allowed for connection

        u_long mode=1;
        if(ioctlsocket(listner,FIONBIO,&mode)==SOCKET_ERROR){
            int error=WSAGetLastError();
            std::cerr<<"Making socket non blocking Failed: "<<error<<std::endl;
            closesocket(listner);
            WSACleanup();
            return 1;
        }

        std::cout<<"Now Socket is non-blocking..\n";
        std::vector<Client> clients;
        int exit_status=0;

        constexpr size_t max_pending_output = 64 * 1024;
        constexpr size_t max_pending_input = 64 * 1024;

        while(true){
            fd_set readset;
            fd_set writeset;
            build_socket_sets(listner,clients,readset,writeset);

            int ready=select(
                0,
                &readset,
                &writeset,
                nullptr,
                nullptr
            );
            
            if(ready==SOCKET_ERROR){
                int error=WSAGetLastError();
                std::cerr<<"Select failed: "<<error<<std::endl;
                exit_status=1;
                break;
            }

            if(FD_ISSET(listner,&readset)){
                while(true){
                    SOCKET newconnection=accept(listner,nullptr,nullptr);

                    if(newconnection!=INVALID_SOCKET){
                        if(clients.size()>=FD_SETSIZE-1){
                            std::cerr<<"Client limit reached\n";
                            closesocket(newconnection);// select can only moniter 63 clients and 1 listner at a time
                            break;
                        }

                        if(ioctlsocket(newconnection,FIONBIO,&mode)==SOCKET_ERROR){
                            int error=WSAGetLastError();
                            std::cerr<<"Making connectedsocket non blocking Failed: "<<error<<std::endl;
                            closesocket(newconnection);
                            continue;
                        }

                        clients.push_back(Client{newconnection,{},{},false});
                        std::cout<<"client connected\n";
                    }
                    else{
                        int error=WSAGetLastError();

                        if(error==WSAEWOULDBLOCK){
                            // no one is here to connect
                            break;
                        }

                        else{
                            std::cerr<<"Accept error: "<<error<<std::endl;
                            break;
                        }
                    }
                }
            }

            for(size_t i=0;i<clients.size();){
                bool removeclient=0;

                if(FD_ISSET(clients[i].socket,&readset)){
                    while(true){
                        char buffer[1024]{};

                        int byterecived=recv(
                            clients[i].socket,
                            buffer,
                            sizeof(buffer),
                            0
                        );

                        if(byterecived>0){
                            std::cout<<"Recived: ";
                            std::cout.write(buffer,byterecived);

                            std::string& input= clients[i].input;
                            size_t received = static_cast<size_t>(byterecived);

                            if(received>max_pending_input-input.size()){
                                std::cerr<<"Client input buffer limit reached\n";
                                removeclient=true;
                                break;
                            }

                            input.append(buffer,received);
                            std::cout<<std::endl;

                            if(!process_input(clients[i],max_pending_output)){
                                removeclient=true;
                                break;
                            }
                        }

                        else if(byterecived==0){// no more bytes will arrive in future
                            std::cout<<"client finished sending forever..\n";
                            clients[i].read_closed=true;
                            break;
                        }

                        else{

                            int error=WSAGetLastError();
                            if(error==WSAEWOULDBLOCK){
                                // bhejne k liye kuch nhi h
                                break;
                            }

                            else{
                                std::cerr<<"Recieve error: "<<error<<std::endl;
                                removeclient=1;
                                break;
                            }
                        }
                    }
                }

                if(!removeclient && FD_ISSET(clients[i].socket,&writeset)){
                    if(!send_pending(clients[i])){
                        removeclient=true;
                    }
                }

                if(clients[i].read_closed && clients[i].output.empty()){
                    removeclient=true;
                }

                if(removeclient){
                    closesocket(clients[i].socket);
                    clients.erase(clients.begin()+i);
                }

                else{
                    i++;
                }
            }
        }

        for(const Client& current: clients){
            closesocket(current.socket);
        }
        closesocket(listner);
        WSACleanup();
        return exit_status;
    }
}