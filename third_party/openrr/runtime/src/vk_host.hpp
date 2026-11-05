// SPDX-License-Identifier: MIT
#pragma once
#include <vulkan/vulkan.h>

#include <string>

namespace onr_host {

// in plus our extensions, features and queue into out (in's own chain changed in place until finish); 0 or -1 with why
int prepare(VkPhysicalDevice phys, const VkDeviceCreateInfo* in, VkDeviceCreateInfo* out, void** token, std::string& why);
// puts the game's structures back; our queue's family and index, what was added; 0 when device is set
int finish(void* token, VkDevice device, uint32_t* family, uint32_t* index, std::string* added);

}  // namespace onr_host
