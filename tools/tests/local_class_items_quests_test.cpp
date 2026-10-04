// Host runner for the local gameplay self-test (src/game/local_selftest.cpp).
#include "game/local_selftest.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "usage: local_class_items_quests_test <world.json> <catalog dir>\n"; return 2; }
    return wowee::game::runLocalGameplaySelfTest(argv[1], argv[2], std::cout, true) ? 0 : 1;
}
