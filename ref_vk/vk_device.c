/*
Copyright (C) 2018-2019 Krzysztof Kondrak

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "vk_local.h"

static const char *devExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

static qboolean deviceExtensionsSupported(const VkPhysicalDevice *physicalDevice, const char **requested, int count)
{
	uint32_t extCount;
	vkEnumerateDeviceExtensionProperties(*physicalDevice, NULL, &extCount, NULL);

	if (extCount > 0)
	{
		VkExtensionProperties *extensions = (VkExtensionProperties *)malloc(extCount * sizeof(VkExtensionProperties));
		vkEnumerateDeviceExtensionProperties(*physicalDevice, NULL, &extCount, extensions);

		for (size_t i = 0; i < count; ++i)
		{
			int available = 0;
			for (uint32_t j = 0; j < extCount; ++j)
			{
				available |= !strcmp(extensions[j].extensionName, requested[i]);
			}

			// all requested extensions must be available
			if (!available)
			{
				free(extensions);
				return false;
			}
		}

		free(extensions);
	}

	return true;
}

static void getBestPhysicalDevice(const VkPhysicalDevice *devices, int preferredIdx, int count)
{
	VkPhysicalDeviceProperties deviceProperties;
	VkPhysicalDeviceFeatures deviceFeatures;
	uint32_t queueFamilyCount = 0;

	for (int i = 0; i < count; ++i)
	{
		vkGetPhysicalDeviceProperties(devices[i], &deviceProperties);
		vkGetPhysicalDeviceFeatures(devices[i], &deviceFeatures);
		vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queueFamilyCount, NULL);

		if (queueFamilyCount == 0)
			continue;

		// prefer discrete GPU but if it's the only one available then don't be picky
		// also - if the user specifies a preferred device, select it
		qboolean bestProperties = deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
		if (preferredIdx == i || (bestProperties && preferredIdx < 0) || count == 1)
		{
			uint32_t formatCount = 0;
			uint32_t presentModesCount = 0;

			// check if requested device extensions are present
			qboolean extSupported = deviceExtensionsSupported(&devices[i], devExtensions, sizeof(devExtensions)/sizeof(devExtensions[0]));

			// no required extensions? try next device
			if (!extSupported || !deviceFeatures.samplerAnisotropy || !deviceFeatures.fillModeNonSolid)
				continue;

			// if extensions are fine, query surface formats and present modes to see if the device can be used
			vkGetPhysicalDeviceSurfaceFormatsKHR(devices[i], vk_surface, &formatCount, NULL);
			vkGetPhysicalDeviceSurfacePresentModesKHR(devices[i], vk_surface, &presentModesCount, NULL);

			if (formatCount == 0 || presentModesCount == 0)
				continue;

			VkQueueFamilyProperties *queueFamilies = (VkQueueFamilyProperties *)malloc(queueFamilyCount * sizeof(VkQueueFamilyProperties));
			vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queueFamilyCount, queueFamilies);

			// secondary check - device is OK if there's at least on queue with VK_QUEUE_GRAPHICS_BIT set
			for (uint32_t j = 0; j < queueFamilyCount; ++j)
			{
				// check if this queue family has support for presentation
				VkBool32 presentSupported;
				VK_VERIFY(vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], j, vk_surface, &presentSupported));

				// good optimization would be to find a queue where presentIdx == gfxQueueIdx for less overhead
				if (vk_device.presentFamilyIndex < 0 && queueFamilies[j].queueCount > 0 && presentSupported)
				{
					vk_device.presentFamilyIndex = j;
				}

				if (vk_device.gfxFamilyIndex < 0 && queueFamilies[j].queueCount > 0 && (queueFamilies[j].queueFlags & VK_QUEUE_GRAPHICS_BIT))
				{
					vk_device.gfxFamilyIndex = j;
				}

				if (vk_device.transferFamilyIndex < 0 && queueFamilies[j].queueCount > 0 && !(queueFamilies[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (queueFamilies[j].queueFlags & VK_QUEUE_TRANSFER_BIT))
				{
					vk_device.transferFamilyIndex = j;
				}
			}

			free(queueFamilies);

			// accept only device that has support for presentation and drawing
			if (vk_device.presentFamilyIndex >= 0 && vk_device.gfxFamilyIndex >= 0)
			{
				if (vk_device.transferFamilyIndex < 0)
				{
					vk_device.transferFamilyIndex = vk_device.gfxFamilyIndex;
				}

				vk_device.physical = devices[i];
				vk_device.properties = deviceProperties;
				vk_device.features = deviceFeatures;
				return;
			}
		}
	}
}

qboolean selectPhysicalDevice(int preferredDeviceIdx)
{
	uint32_t physicalDeviceCount = 0;
	VK_VERIFY(vkEnumeratePhysicalDevices(vk_instance, &physicalDeviceCount, NULL));

	if (physicalDeviceCount == 0)
	{
		ri.Con_Printf(PRINT_ALL, "No Vulkan-capable devices found!\n");
		return false;
	}

	ri.Con_Printf(PRINT_ALL, "...found %d Vulkan-capable device(s)\n", physicalDeviceCount);

	VkPhysicalDevice *physicalDevices = (VkPhysicalDevice *)malloc(physicalDeviceCount * sizeof(VkPhysicalDevice));
	VK_VERIFY(vkEnumeratePhysicalDevices(vk_instance, &physicalDeviceCount, physicalDevices));

	getBestPhysicalDevice(physicalDevices, preferredDeviceIdx < physicalDeviceCount ? preferredDeviceIdx : -1, physicalDeviceCount);
	free(physicalDevices);

	if (vk_device.physical == VK_NULL_HANDLE)
	{
		ri.Con_Printf(PRINT_ALL, "Could not find a suitable physical device!\n");
		return false;
	}

	return true;
}


static VkResult createLogicalDevice()
{
	// at least one queue (graphics and present combined) has to be present
	uint32_t numQueues = 1;
	float queuePriority = 1.f;
	VkDeviceQueueCreateInfo queueCreateInfo[3];
	queueCreateInfo[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueCreateInfo[0].pNext = NULL;
	queueCreateInfo[0].flags = 0;
	queueCreateInfo[0].queueFamilyIndex = vk_device.gfxFamilyIndex;
	queueCreateInfo[0].queueCount = 1;
	queueCreateInfo[0].pQueuePriorities = &queuePriority;
	queueCreateInfo[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueCreateInfo[1].pNext = NULL;
	queueCreateInfo[1].flags = 0;
	queueCreateInfo[1].queueFamilyIndex = 0;
	queueCreateInfo[1].queueCount = 1;
	queueCreateInfo[1].pQueuePriorities = &queuePriority;
	queueCreateInfo[2].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueCreateInfo[2].pNext = NULL;
	queueCreateInfo[2].flags = 0;
	queueCreateInfo[2].queueFamilyIndex = 0;
	queueCreateInfo[2].queueCount = 1;
	queueCreateInfo[2].pQueuePriorities = &queuePriority;

	VkPhysicalDeviceFeatures wantedDeviceFeatures = {
		.samplerAnisotropy = vk_device.features.samplerAnisotropy,
		.fillModeNonSolid  = vk_device.features.fillModeNonSolid,  // for wireframe rendering
		.sampleRateShading = vk_device.features.sampleRateShading, // for sample shading
	};

	// a graphics and present queue are different - two queues have to be created
	if (vk_device.gfxFamilyIndex != vk_device.presentFamilyIndex)
	{
		queueCreateInfo[numQueues++].queueFamilyIndex = vk_device.presentFamilyIndex;
	}

	// a separate transfer queue exists that's different from present and graphics queue?
	if (vk_device.transferFamilyIndex != vk_device.gfxFamilyIndex && vk_device.transferFamilyIndex != vk_device.presentFamilyIndex)
	{
		queueCreateInfo[numQueues++].queueFamilyIndex = vk_device.transferFamilyIndex;
	}

	VkDeviceCreateInfo deviceCreateInfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pEnabledFeatures = &wantedDeviceFeatures,
		.ppEnabledExtensionNames = devExtensions,
		.enabledExtensionCount = (uint32_t)(sizeof(devExtensions) / sizeof(devExtensions[0])),
		.enabledLayerCount = 0,
		.ppEnabledLayerNames = NULL,
		.queueCreateInfoCount = numQueues,
		.pQueueCreateInfos = queueCreateInfo
	};

	const char *validationLayers[] = { "VK_LAYER_LUNARG_standard_validation" };
	if (vk_validation->value)
	{
		deviceCreateInfo.enabledLayerCount = sizeof(validationLayers)/sizeof(validationLayers[0]);
		deviceCreateInfo.ppEnabledLayerNames = validationLayers;
	}

	return vkCreateDevice(vk_device.physical, &deviceCreateInfo, NULL, &vk_device.logical);
}

static const char *deviceTypeString(VkPhysicalDeviceType dType)
{
#define DEVTYPESTR(r) case VK_ ##r: return "VK_"#r
	switch (dType)
	{
		DEVTYPESTR(PHYSICAL_DEVICE_TYPE_OTHER);
		DEVTYPESTR(PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
		DEVTYPESTR(PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
		DEVTYPESTR(PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU);
		DEVTYPESTR(PHYSICAL_DEVICE_TYPE_CPU);
		default: return "<unknown>";
	}
#undef DEVTYPESTR
	return "UNKNOWN DEVICE";
}

static const char *vendorNameString(uint32_t vendorId)
{
	switch (vendorId)
	{
	case 0x1002: return "AMD";
	case 0x1010: return "ImgTec";
	case 0x10DE: return "NVIDIA";
	case 0x13B5: return "ARM";
	case 0x5143: return "Qualcomm";
	case 0x8086: return "Intel";
	default:     return "unknown";
	}
}

qboolean QVk_CreateDevice(int preferredDeviceIdx)
{
	if (!selectPhysicalDevice(preferredDeviceIdx))
		return false;

	vk_config.vendor_name = vendorNameString(vk_device.properties.vendorID);
	vk_config.device_type = deviceTypeString(vk_device.properties.deviceType);

	VkResult res = createLogicalDevice();
	if (res != VK_SUCCESS)
	{
		ri.Con_Printf(PRINT_ALL, "Could not create Vulkan logical device: %s\n", QVk_GetError(res));
		return false;
	}

	vkGetDeviceQueue(vk_device.logical, vk_device.gfxFamilyIndex, 0, &vk_device.gfxQueue);
	vkGetDeviceQueue(vk_device.logical, vk_device.presentFamilyIndex, 0, &vk_device.presentQueue);
	vkGetDeviceQueue(vk_device.logical, vk_device.transferFamilyIndex, 0, &vk_device.transferQueue);

	return true;
}
