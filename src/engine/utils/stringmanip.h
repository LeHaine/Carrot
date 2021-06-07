//
// Created by jglrxavpok on 01/12/2020.
//

#pragma once
#include <vector>
#include <string>

using namespace std;

namespace Carrot {
    vector<string> splitString(const string& toSplit, const string& delimiter);

    std::string toLowerCase(const string& str);
    std::string toUpperCase(const string& str);
}