#include <mail_system/back/mailServer/imaps_server.h>
#include <limits>
// #include <boost/mysql.hpp>
using namespace mail_system;
int main() {
    std::cin.tie(nullptr);
    try
    {
        ServerConfig config;
        if(!config.loadFromFile("../config/imapsConfig.json")) {
            std::cerr << "Failed to load config from file." << std::endl;
            return 1;
        }

        config.show();
        char check[16] = {0};
        std::cout << "Start server? (y/n)" << std::endl;
        std::cin >> check;
        if(check[0] != 'y' && check[0] != 'Y') {
            return 0;
        }

        ImapsServer server(config);
        server.start();
        char cmd[256];
        while(true){
            memset(cmd,0,256);
            std::cout << "waiting for command:\n";
            std::cin.ignore(256, '\n');
            std::cin.getline(cmd,255);
            if(cmd[0] == 'q' || cmd[0] == 'Q') {
                server.stop();
                std::cout << "Server quit.\n";
                break;
            }
        }
    }
    catch (const boost::system::system_error& e) {
        std::cerr << "Boost system error: " << e.what() << std::endl;
        return 1;
    }
    catch (const std::runtime_error& e) {
        std::cerr << "Runtime error: " << e.what() << std::endl;
        return 1;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}