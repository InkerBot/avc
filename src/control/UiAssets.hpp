#pragma once

#include <string_view>

namespace avc::control::ui {

struct Asset {
    std::string_view path;
    std::string_view data;
};

const Asset *find(std::string_view path);

bool empty();

}
