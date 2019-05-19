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

#include <vulkan.frag.h>
#include <vulkan.vert.h>

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

	// pipeline
	VkRenderPass rp;
	VkPipelineLayout pipe_layout;
	VkPipeline pipe;
	VkCommandPool command_pool;
};

struct vk_image {
	struct buffer buffer;

	VkDeviceMemory memory;
	VkImage image;
	VkCommandBuffer cb;
	VkFramebuffer fb;

	VkSemaphore render; // signaled when rendering finishes
	VkFence outfence; // signaled by kernel when image can be reused
};

#define vk_error(res, fmt, ...) error(fmt ": %s (%d)", \
	vulkan_strerror(res), res, ##__VA_ARGS__)

// Returns a VkResult value as string.
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

#ifdef DEBUG
	if (match) {
		VkPhysicalDeviceProperties *props = &phdev_props.properties;
		uint32_t vv_major = (props->apiVersion >> 22);
		uint32_t vv_minor = (props->apiVersion >> 12) & 0x3ff;
		uint32_t vv_patch = (props->apiVersion) & 0xfff;

		uint32_t dv_major = (props->driverVersion >> 22);
		uint32_t dv_minor = (props->driverVersion >> 12) & 0x3ff;
		uint32_t dv_patch = (props->driverVersion) & 0xfff;

		const char* dev_type = "unknown";
		switch(props->deviceType) {
			case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
				dev_type = "integrated";
				break;
			case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
				dev_type = "discrete";
				break;
			case VK_PHYSICAL_DEVICE_TYPE_CPU:
				dev_type = "cpu";
				break;
			case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
				dev_type = "gpu";
				break;
			default:
				break;
		}

		debug("Vulkan device: '%s'", props->deviceName);
		debug("Device type: '%s'", dev_type);
		debug("Supported API version: %u.%u.%u", vv_major, vv_minor, vv_patch);
		debug("Driver version: %u.%u.%u", dv_major, dv_minor, dv_patch);
	}
#endif

	return match;
}

void vk_device_destroy(struct vk_device *device)
{
	if (device->dev) {
		vkDestroyDevice(device->dev, NULL);
	}
	if (device->messenger && device->api.destroyDebugUtilsMessengerEXT) {
		device->api.destroyDebugUtilsMessengerEXT(device->instance,
			device->messenger, NULL);
	}
	if (device->instance) {
		vkDestroyInstance(device->instance, NULL);
	}
	free(device);
}

bool init_pipeline(struct vk_device *dev)
{
	// This corresponds to the XRGB drm format (reversed endianess).
	// The egl format hardcodes this format so we can probably too.
	// It's guaranteed to be supported by the vulkan spec for everything
	// we need.
	VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;

	// render pass
	// NOTE: we don't care about previous contents of the image since
	// we always render the full image. For incremental presentation you
	// have to use LOAD_OP_STORE and a valid image layout.
	VkAttachmentDescription attachment = {0};
	attachment.format = format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	// can basically be anything since we have to manually transition
	// the image afterwards anyways (see depdency reasoning below)
	attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

	VkAttachmentReference color_ref = {0};
	color_ref.attachment = 0u;
	color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {0};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_ref;

	// Note how we don't specify any subpass dependencies. The transfer
	// of an image to an external queue (i.e. transfer logical ownership
	// of the image from the vulkan driver to drm) can't be represented
	// as a subpass dependency, so we have to transition the image
	// after and before a renderpass manually anyways.
	VkRenderPassCreateInfo rp_info = {0};
	rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rp_info.attachmentCount = 1;
	rp_info.pAttachments = &attachment;
	rp_info.subpassCount = 1;
	rp_info.pSubpasses = &subpass;
	VkResult res = vkCreateRenderPass(dev->dev, &rp_info, NULL, &dev->rp);
	if (res != VK_SUCCESS) {
		vk_error(res, "vkCreateRenderPass");
		return false;
	}

	// pipeline layout
	// our simple pipeline doesn't use any descriptor sets or push constants,
	// so the pipeline layout is trivial
	VkPipelineLayoutCreateInfo pli = {0};
	pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	res = vkCreatePipelineLayout(dev->dev, &pli, NULL, &dev->pipe_layout);
	if (res != VK_SUCCESS) {
		vk_error(res, "vkCreatePipelineLayout");
		return false;
	}

	// pipeline
	VkShaderModule vert_module;
	VkShaderModule frag_module;

	VkShaderModuleCreateInfo si;
	si.codeSize = sizeof(vulkan_vert_data);
	si.pCode = vulkan_vert_data;
	res = vkCreateShaderModule(dev->dev, &si, NULL, &vert_module);
	if (res != VK_SUCCESS) {
		vk_error(res, "Failed to create vertex shader module");
		return false;
	}

	si.codeSize = sizeof(vulkan_frag_data);
	si.pCode = vulkan_frag_data;
	res = vkCreateShaderModule(dev->dev, &si, NULL, &frag_module);
	if (res != VK_SUCCESS) {
		vk_error(res, "Failed to create fragment shader module");
		vkDestroyShaderModule(dev->dev, vert_module, NULL);
		return false;
	}

	VkPipelineShaderStageCreateInfo pipe_stages[2] = {{
			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, vert_module, "main", NULL
		}, {
			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag_module, "main", NULL
		}
	};

	// info
	VkPipelineInputAssemblyStateCreateInfo assembly = {0};
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;

	VkPipelineRasterizationStateCreateInfo rasterization = {0};
	rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.f;

	VkPipelineColorBlendAttachmentState blend_attachment = {0};
	blend_attachment.blendEnable = false;
	blend_attachment.colorWriteMask =
		VK_COLOR_COMPONENT_R_BIT |
		VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT |
		VK_COLOR_COMPONENT_A_BIT;

	VkPipelineColorBlendStateCreateInfo blend = {0};
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;

	VkPipelineMultisampleStateCreateInfo multisample = {0};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineViewportStateCreateInfo viewport = {0};
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;

	VkDynamicState dynStates[2] = {
		VK_DYNAMIC_STATE_VIEWPORT,
    	VK_DYNAMIC_STATE_SCISSOR,
	};
	VkPipelineDynamicStateCreateInfo dynamic = {0};
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.pDynamicStates = dynStates;
	dynamic.dynamicStateCount = 2;

	VkPipelineVertexInputStateCreateInfo vertex = {0};
	vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	VkGraphicsPipelineCreateInfo pipe_info = {0};
	pipe_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipe_info.layout = dev->pipe_layout;
	pipe_info.renderPass = dev->rp;
	pipe_info.subpass = 0;
	pipe_info.stageCount = 2;
	pipe_info.pStages = pipe_stages;

	pipe_info.pInputAssemblyState = &assembly;
	pipe_info.pRasterizationState = &rasterization;
	pipe_info.pColorBlendState = &blend;
	pipe_info.pMultisampleState = &multisample;
	pipe_info.pViewportState = &viewport;
	pipe_info.pDynamicState = &dynamic;
	pipe_info.pVertexInputState = &vertex;

	// NOTE: could use a cache here for faster loading
	// store it somewhere like $XDG_CACHE_HOME/wlroots/vk_pipe_cache
	VkPipelineCache cache = VK_NULL_HANDLE;
	res = vkCreateGraphicsPipelines(dev->dev, cache, 1, &pipe_info,
		NULL, &dev->pipe);
	vkDestroyShaderModule(dev->dev, vert_module, NULL);
	vkDestroyShaderModule(dev->dev, frag_module, NULL);
	if (res != VK_SUCCESS) {
		error("failed to create vulkan pipeline: %d", res);
		return false;
	}

	return true;
}

struct vk_device *vk_device_create(struct device *device)
{
	// check for drm device support
	// vulkan requires modifier support to import dma bufs
	if (!device->fb_modifiers) {
		debug("Can't use vulkan since drm doesn't support modifiers");
		return NULL;
	}

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
			avail_exts[j].extensionName);
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

	drmPciBusInfoPtr pci = drm_dev->businfo.pci;
	debug("PCI bus: %04x:%02x:%02x.%x", pci->domain,
		pci->bus, pci->dev, pci->func);

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

	// query extensions
	const char* dev_exts[] = {
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
		VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME, // required by drm ext

		// NOTE: strictly speaking this extension is required to
		// correctly transfer image ownership but since no mesa
		// driver implements its yet (no even an updated patch for that),
		// let's see how far we get without it
		// VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
	};

	for (unsigned i = 0u; i < ARRAY_LENGTH(dev_exts); ++i) {
		if (!has_extension(phdev_exts, phdev_extc, dev_exts[i])) {
			error("Physical device doesn't supported required extension: %s",
				dev_exts[i]);
			goto error;
		}
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
	vk_dev->queue_family = qfam;

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
	dev_info.enabledExtensionCount = ARRAY_LENGTH(dev_exts);
	dev_info.ppEnabledExtensionNames = dev_exts;

	res = vkCreateDevice(phdev, &dev_info, NULL, &vk_dev->dev);
	if (res != VK_SUCCESS){
		vk_error(res, "Failed to create vulkan device");
		goto error;
	}

	vkGetDeviceQueue(vk_dev->dev, vk_dev->queue_family, 0, &vk_dev->queue);
	vk_dev->api.getMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)
		vkGetDeviceProcAddr(vk_dev->dev, "vkGetMemoryFdPropertiesKHR");
	if (!vk_dev->api.getMemoryFdPropertiesKHR) {
		error("Failed to retrieve vkGetMemoryFdPropertiesKHR");
		goto error;
	}

	// command pool
	VkCommandPoolCreateInfo cpi = {0};
	cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpi.queueFamilyIndex = vk_dev->queue_family;
	res = vkCreateCommandPool(vk_dev->dev, &cpi, NULL, &vk_dev->command_pool);
	if (res != VK_SUCCESS) {
		vk_error(res, "vkCreateCommandPool");
		goto error;
	}

	if (!init_pipeline(vk_dev)) {
		goto error;
	}

	// TODO: query and store supported format modifiers (bgra8unorm)
	// check that we can use it as color attachment image

	return vk_dev;

error:
	vk_device_destroy(vk_dev);
	return NULL;
}

struct buffer *buffer_vk_create(struct device *device, struct output *output)
{
	struct vk_image *img = calloc(1, sizeof(*img));
	struct vk_device *vk_dev; // = TODO, retrieve from device

	// create gbm bo with modifiers supported by output and vulkan

	// import gbm bo memory, create image on it

	// create framebuffer for imported image

	// create command buffer

	// record render pass on imported image into command buffer

	// create render fence, export it as syncfile to buffer->render_fence_fd

	// success!
	return &img->buffer;
}

void buffer_vk_destroy(struct device *device, struct buffer *buffer)
{
	struct vk_image *img = (struct vk_image *)buffer;
	// TODO: destroy everything in reverse order
}

void buffer_vk_fill(struct buffer *buffer, int frame_num)
{
	struct vk_image *img = (struct vk_image *)buffer;
	struct vk_device *vk_dev; // = TODO, retrieve from buffer

	// import kms_fence_fd as semaphore
	// TODO: when to destroy the previous kms_fence_fd? is it enough
	//   if we do that here? i guess the fence still has be alive.
	//   Probably requires changes to main.c/kms.c, we close the fence
	//   in fd_replace i guess, main.c:408? maybe create own temporary
	//   duplicate and import that instead as temporary hack for now?

	// reset the fence from the previous frame

	// submit the buffers command buffer
	// - it waits for the kms_fence_fd semaphore
	// - upon completion, it signals the render fence
}
