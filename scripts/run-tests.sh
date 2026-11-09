set -e
g++ -std=c++20 -Wall -Wextra -I src -I third_party src/anti_rush.cpp tests/anti_rush_test.cpp -o /tmp/t1 && /tmp/t1
g++ -std=c++20 -Wall -Wextra -I src -I third_party src/config.cpp tests/config_test.cpp -o /tmp/t2 && /tmp/t2
g++ -std=c++20 -Wall -Wextra -I src -I third_party src/messages.cpp tests/messages_test.cpp -o /tmp/t3 && /tmp/t3
g++ -std=c++20 -Wall -Wextra -I src -I third_party tests/update_check_test.cpp -o /tmp/t4 && /tmp/t4
