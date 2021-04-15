//
// Created by jglrxavpok on 15/04/2021.
//

#pragma once
#include <cstdint>

class SwapchainAware {
public:
    /**
     * Fired before onSwapchainSizeChange
     * @param newCount
     */
    virtual void onSwapchainImageCountChange(size_t newCount) {}

    /**
     * Fired after onSwapchainImageCountChange, safe to assume image count won't change before next swapchain recreation
     * @param newWidth
     * @param newHeight
     */
    virtual void onSwapchainSizeChange(int newWidth, int newHeight) {}
};


