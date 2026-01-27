#pragma once
#include <algorithm>
#include <string>
#include <iterator>
#include "../utils/StringUtils.h"

namespace vdc {

    template<typename Container, typename KeyFunc>
    inline auto FindByKey(Container& c, const std::string& key, KeyFunc keyFunc)
        -> decltype(std::begin(c))
    {
        const std::string nk = vdc::ToLower(key);
        return std::find_if(std::begin(c), std::end(c),
            [&](const auto& item){ return vdc::ToLower(keyFunc(item)) == nk; });
    }

    template<typename It, typename KeyFunc>
    inline It FindByKey(It first, It last, const std::string& key, KeyFunc keyFunc) {
        const std::string nk = vdc::ToLower(key);
        return std::find_if(first, last, [&](const auto& item){ return vdc::ToLower(keyFunc(item)) == nk; });
    }

}
