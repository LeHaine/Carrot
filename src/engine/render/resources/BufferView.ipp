//
// Created by jglrxavpok on 10/03/2021.
//

#include "BufferView.h"

template<typename T>
T* Carrot::BufferView::map() {
    // TODO: proper segmentation
    auto* pData = getBuffer().map<uint8_t>();
    pData += start;
    return reinterpret_cast<T*>(pData);
}