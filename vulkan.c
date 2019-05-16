/*
 * Copyright © 2019 nyorain
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Author: nyorain <nyorain@gmail.com>
 */

#include "kms-quads.h"
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define vk_error(res, fmt, ...) error(fmt ": %s (%d)", \
	vulkan_strerror(res), res, ##__VA_ARGS__)

const char *vulkan_strerror(VkResult err) {
	#define ERR_STR(r) case VK_ ##r: return #r
	switch (err) {
		ERR_STR(SUCCESS);
		ERR_STR(NOT_READY);
		ERR_STR(TIMEOUT);
		ERR_STR(EVENT_SET);
		ERR_STR(EVENT_RESET);
		ERR_STR(INCOMPLETE);
		ERR_STR(SUBOPTIMAL_KHR);
		ERR_STR(ERROR_OUT_OF_HOST_MEMORY);
		ERR_STR(ERROR_OUT_OF_DEVICE_MEMORY);
		ERR_STR(ERROR_INITIALIZATION_FAILED);
		ERR_STR(ERROR_DEVICE_LOST);
		ERR_STR(ERROR_MEMORY_MAP_FAILED);
		ERR_STR(ERROR_LAYER_NOT_PRESENT);
		ERR_STR(ERROR_EXTENSION_NOT_PRESENT);
		ERR_STR(ERROR_FEATURE_NOT_PRESENT);
		ERR_STR(ERROR_INCOMPATIBLE_DRIVER);
		ERR_STR(ERROR_TOO_MANY_OBJECTS);
		ERR_STR(ERROR_FORMAT_NOT_SUPPORTED);
		ERR_STR(ERROR_SURFACE_LOST_KHR);
		ERR_STR(ERROR_NATIVE_WINDOW_IN_USE_KHR);
		ERR_STR(ERROR_OUT_OF_DATE_KHR);
		ERR_STR(ERROR_FRAGMENTED_POOL);
		ERR_STR(ERROR_INCOMPATIBLE_DISPLAY_KHR);
		ERR_STR(ERROR_VALIDATION_FAILED_EXT);
		ERR_STR(ERROR_INVALID_EXTERNAL_HANDLE);
		ERR_STR(ERROR_OUT_OF_POOL_MEMORY);
		ERR_STR(ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
		default:
			return "<unknown>";
	}
	#undef STR
}

struct vk_device {
	VkInstance instance;
	VkDebugUtilsMessengerEXT messenger;

	struct {
		PFN_vkCreateDebugUtilsMessengerEXT createDebugUtilsMessengerEXT;
		PFN_vkDestroyDebugUtilsMessengerEXT destroyDebugUtilsMessengerEXT;
		PFN_vkGetMemoryFdPropertiesKHR getMemoryFdPropertiesKHR;
	} api;

	VkPhysicalDevice phdev;
	VkDevice dev;

	uint32_t queue_family;
	VkQueue queue;
};

static bool has_extension(const VkExtensionProperties *avail,
	uint32_t availc, const char *req)
{
	// check if all required extensions are supported
	for (size_t j = 0; j < availc; ++j) {
		if (!strcmp(avail[j].extensionName, req)) {
			return true;
		}
	}

	return false;
}

static VkBool32 debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT type,
		const VkDebugUtilsMessengerCallbackDataEXT *debug_data,
		void *data) {

	((void) data);
	((void) type);

	// we ignore some of the non-helpful warnings
	static const char *const ignored[] = {};
	if (debug_data->pMessageIdName) {
		for (unsigned i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
			if (!strcmp(debug_data->pMessageIdName, ignored[i])) {
				return false;
			}
		}
	}

	const char* importance = "UNKNOWN";
	switch(severity) {
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
			importance = "ERROR";
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
			importance = "WARNING";
			break;
		case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
			importance = "INFO";
			break;
		default:
			break;
	}

	debug("%s: %s (%s)", importance, debug_data->pMessage,
		debug_data->pMessageIdName);
	if (debug_data->queueLabelCount > 0) {
		const char *name = debug_data->pQueueLabels[0].pLabelName;
		if (name) {
			debug("    last queue label '%s'", name);
		}
	}

	if (debug_data->cmdBufLabelCount > 0) {
		const char *name = debug_data->pCmdBufLabels[0].pLabelName;
		if (name) {
			debug("    last cmdbuf label '%s'", name);
		}
	}

	for (unsigned i = 0; i < debug_data->objectCount; ++i) {
		if (debug_data->pObjects[i].pObjectName) {
			debug("    involving '%s'", debug_data->pMessage);
		}
	}

	return false;
}

// Returns whether the given drm device pci info matches the given physical
// device. Will write/realloc the given extensions count and data of
// the queried physical device.
bool match(drmPciBusInfoPtr pci_bus_info, VkPhysicalDevice phdev,
	uint32_t *extc, VkExtensionProperties **exts)
{
	VkResult res;
	res = vkEnumerateDeviceExtensionProperties(phdev, NULL,
		extc, NULL);
	if ((res != VK_SUCCESS) || (exts == 0)) {
		exts = 0;
		vk_error(res, "Could not enumerate device extensions (1)");
		return false;
	}

	exts = realloc(exts, sizeof(**exts) * *extc);
	res = vkEnumerateDeviceExtensionProperties(phdev, NULL,
		extc, *exts);
	if (res != VK_SUCCESS) {
		vk_error(res, "Could not enumerate device extensions (2)");
		return false;
	}

	if (!has_extension(*exts, *extc, VK_EXT_PCI_BUS_INFO_EXTENSION_NAME)) {
		error("Physical device has not support for VK_EXT_pci_bus_info");
		return false;
	}

	VkPhysicalDevicePCIBusInfoPropertiesEXT pci_props;
	pci_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;

	VkPhysicalDeviceProperties2 phdev_props;
	phdev_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	phdev_props.pNext = &pci_props;

	vkGetPhysicalDeviceProperties2(phdev, &phdev_props);
	bool match = pci_props.pciBus == pci_bus_info->bus &&
		pci_props.pciDevice == pci_bus_info->dev &&
		pci_props.pciDomain == pci_bus_info->domain &&
		pci_props.pciFunction == pci_bus_info->func;
	return match;
}

struct vulkan_device *vk_device_create(struct device *device)
{
	// query extension support
	uint32_t avail_extc = 0;
	VkResult res;
	res = vkEnumerateInstanceExtensionProperties(NULL, &avail_extc, NULL);
	if ((res != VK_SUCCESS) || (avail_extc == 0)) {
		vk_error(res, "Could not enumerate instance extensions (1)");
		return NULL;
	}

	VkExtensionProperties avail_exts[avail_extc + 1];
	res = vkEnumerateInstanceExtensionProperties(NULL, &avail_extc,
		avail_exts);
	if (res != VK_SUCCESS) {
		vk_error(res, "Could not enumerate instance extensions (2)");
		return NULL;
	}

	for (size_t j = 0; j < avail_extc; ++j) {
		debug("Vulkan Instance extensions %s",
			avail_ext_props[j].extensionName);
	}

	struct vk_device *vk_dev = calloc(1, sizeof(*vk_dev));
	assert(vk_dev);

	// create instance
	const char *req = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
	const char** enable_exts = NULL;
	uint32_t enable_extc = 0;
	if (has_extension(avail_exts, avail_extc, req)) {
		enable_exts = &req;
		enable_exts[enable_extc++] = req;
	}

	VkApplicationInfo application_info = {0};
	application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application_info.pApplicationName = "kms-vulkan";
	application_info.applicationVersion = 1;
	application_info.pEngineName = "kms-vulkan";
	application_info.engineVersion = 1;
	// will only run on the latest drivers anyways so we can require
	// vulkan 1.1 without problems
	application_info.apiVersion = VK_MAKE_VERSION(1,1,0);

	// layer reports error in api usage to debug callback
	const char *layers[] = {
		"VK_LAYER_KHRONOS_validation",
	};

	VkInstanceCreateInfo instance_info = {0};
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &application_info;
	instance_info.enabledExtensionCount = enable_extc;
	instance_info.ppEnabledExtensionNames = enable_exts;
	instance_info.enabledLayerCount = ARRAY_LENGTH(layers);
	instance_info.ppEnabledLayerNames = layers;

	res = vkCreateInstance(&instance_info, NULL, &vk_dev->instance);
	if (res != VK_SUCCESS) {
		vk_error(res, "Could not create instance");
		goto error;
	}

	// debug callback
	if (enable_extc) {
		vk_dev->api.createDebugUtilsMessengerEXT =
			(PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(
					vk_dev->instance, "vkCreateDebugUtilsMessengerEXT");
		vk_dev->api.destroyDebugUtilsMessengerEXT =
			(PFN_vkDestroyDebugUtilsMessengerEXT) vkGetInstanceProcAddr(
					vk_dev->instance, "vkDestroyDebugUtilsMessengerEXT");

		if (vk_dev->api.createDebugUtilsMessengerEXT) {
			VkDebugUtilsMessageSeverityFlagsEXT severity =
				// VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			VkDebugUtilsMessageTypeFlagsEXT types =
				// VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
				VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

			VkDebugUtilsMessengerCreateInfoEXT debug_info = {0};
			debug_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
			debug_info.messageSeverity = severity;
			debug_info.messageType = types;
			debug_info.pfnUserCallback = &debug_callback;

			vk_dev->api.createDebugUtilsMessengerEXT(vk_dev->instance,
				&debug_info, NULL, &vk_dev->messenger);
		} else {
			error("vkCreateDebugUtilsMessengerEXT not found");
		}
	}

	// enumerate physical devices to find the one matching the given
	// gbm device.
	uint32_t num_phdevs;
	res = vkEnumeratePhysicalDevices(vk_dev->instance, &num_phdevs, NULL);
	if (res != VK_SUCCESS || num_phdevs == 0) {
		vk_error(res, "Could not retrieve physical device");
		goto error;
	}

	VkPhysicalDevice *phdevs = calloc(num_phdevs, sizeof(*phdevs));
	res = vkEnumeratePhysicalDevices(vk_dev->instance, &num_phdevs, phdevs);
	if (res != VK_SUCCESS || num_phdevs == 0) {
		vk_error(res, "Could not retrieve physical device");
		goto error;
	}

	// get pci information of device
	drmDevicePtr drm_dev;
	drmGetDevice(device->kms_fd, &drm_dev);
	if(drm_dev->bustype != DRM_BUS_PCI) {
		// NOTE: we could check that/gather the pci information
		// on device creation
		error("Given device isn't a pci device");
		goto error;
	}

	VkExtensionProperties *phdev_exts;
	uint32_t phdev_extc;
	VkPhysicalDevice phdev = VK_NULL_HANDLE;
	for (unsigned i = 0u; i < num_phdevs; ++i) {
		VkPhysicalDevice phdevi = phdevs[i];
		if (match(drm_dev->businfo.pci, phdevi, &phdev_extc, &phdev_exts)) {
			phdev = phdevi;
			break;
		}
	}

	if (phdev == VK_NULL_HANDLE) {
		error("Can't find vulkan physical device for drm dev");
		goto error;
	}

	// create device
	// queue families
	uint32_t qfam_count;
	vkGetPhysicalDeviceQueueFamilyProperties(phdev, &qfam_count, NULL);
	VkQueueFamilyProperties *qprops = calloc(sizeof(*qprops), qfam_count);
	vkGetPhysicalDeviceQueueFamilyProperties(phdev, &qfam_count, qprops);

	uint32_t qfam = 0xFFFFFFu; // graphics queue family
	for (unsigned i = 0u; i < qfam_count; ++i) {
		if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			qfam = i;
			break;
		}
	}

	// vulkan standard guarantees that the must be at least one graphics
	// queue family
	assert(qfam != 0xFFFFFFFFu);

	// info
	float prio = 1.f;
	VkDeviceQueueCreateInfo qinfo;
	qinfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qinfo.queueFamilyIndex = qfam;
	qinfo.queueCount = 1;
	qinfo.pQueuePriorities = &prio;

	VkDeviceCreateInfo dev_info = {0};
	dev_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dev_info.queueCreateInfoCount = 1;
	dev_info.pQueueCreateInfos = &qinfo;
	dev_info.enabledExtensionCount = dev->extension_count;
	dev_info.ppEnabledExtensionNames = dev->extensions;

	res = vkCreateDevice(phdev, &dev_info, NULL, &vk_dev->dev);
	if (res != VK_SUCCESS){
		vk_error(res, "Failed to create vulkan device");
		goto error;
	}

error:
	vk_device_destroy(vk_dev);
}
