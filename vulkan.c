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
#include <inttypes.h>

#include <vulkan.frag.h>
#include <vulkan.vert.h>

// TODO: use srgb format?
// This corresponds to the XRGB drm format.
// The egl format hardcodes this format so we can probably too.
// It's guaranteed to be supported by the vulkan spec for everything
// we need.
static const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;

struct vk_device {
	VkInstance instance;
	VkDebugUtilsMessengerEXT messenger;

	struct {
		PFN_vkCreateDebugUtilsMessengerEXT createDebugUtilsMessengerEXT;
		PFN_vkDestroyDebugUtilsMessengerEXT destroyDebugUtilsMessengerEXT;
		PFN_vkGetMemoryFdPropertiesKHR getMemoryFdPropertiesKHR;
		PFN_vkGetFenceFdKHR getFenceFdKHR;
		PFN_vkImportSemaphoreFdKHR importSemaphoreFdKHR;
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

	VkDeviceMemory memories[4]; // worst case: 4 planes, 4 memory objects
	VkImage image;
	VkImageView image_view;
	VkCommandBuffer cb;
	VkFramebuffer fb;

	VkSemaphore buffer_semaphore; // signaled by kernal when image can be reused
	VkFence render_fence; // signaled by vulkan when rendering finishes
};

// #define vk_error(res, fmt, ...)
#define vk_error(res, fmt) error(fmt ": %s (%d)", vulkan_strerror(res), res)

// Returns a VkResult value as string.
static const char *vulkan_strerror(VkResult err) {
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

static VkImageAspectFlagBits mem_plane_ascpect(unsigned i)
{
	switch(i) {
		case 0: return VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT;
		case 1: return VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT;
		case 2: return VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT;
		case 3: return VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT;
		default: assert(false); // unreachable
	}
}

int find_mem_type(VkPhysicalDevice phdev,
	VkMemoryPropertyFlags flags, uint32_t req_bits)
{

	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(phdev, &props);

	for (unsigned i = 0u; i < props.memoryTypeCount; ++i) {
		if (req_bits & (1 << i)) {
			if ((props.memoryTypes[i].propertyFlags & flags) == flags) {
				return i;
			}
		}
	}

	return -1;
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
	// static const char *const ignored[] = {};
	// if (debug_data->pMessageIdName) {
	// 	for (unsigned i = 0; i < sizeof(ignored) / sizeof(ignored[0]); ++i) {
	// 		if (!strcmp(debug_data->pMessageIdName, ignored[i])) {
	// 			return false;
	// 		}
	// 	}
	// }

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

	vk_dev->phdev = phdev;

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

	vk_dev->api.getFenceFdKHR = (PFN_vkGetFenceFdKHR)
		vkGetDeviceProcAddr(vk_dev->dev, "vkGetFenceFdKHR");
	if (!vk_dev->api.getFenceFdKHR) {
		error("Failed to retrieve vkGetFenceFdKHR");
		goto error;
	}

	vk_dev->api.importSemaphoreFdKHR = (PFN_vkImportSemaphoreFdKHR)
		vkGetDeviceProcAddr(vk_dev->dev, "vkImportSemaphoreFdKHR");
	if (!vk_dev->api.importSemaphoreFdKHR) {
		error("Failed to retrieve vkImportSemaphoreFdKHR");
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

	// TODO: we could relax those requirements when we don't force explicit
	// syncing (via manual waiting).

	// semaphore import/export support
	// we import kms_fence_fd as semaphore and add that as wait semaphore
	// to a render submission so that we only render a buffer when
	// kms signals that it's finished with it.
	VkPhysicalDeviceExternalSemaphoreInfo esi = {0};
	esi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
	esi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

	VkExternalSemaphoreProperties esp = {0};
	esp.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
	vkGetPhysicalDeviceExternalSemaphoreProperties(phdev, &esi, &esp);

	if((esp.externalSemaphoreFeatures &
			VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) == 0) {
		error("Vulkan can't import sync_fd semaphores");
		goto error;
	}

	// fence export support
	// we export the fence for our render submission as sync_fd
	// and pass that as render_fence_fd to the kernel, signaling
	// that the buffer can only be used when that fence is signaled,
	// i.e. we are finished with rendering and all barriers.
	VkPhysicalDeviceExternalFenceInfo efi = {0};
	efi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO;
	efi.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;

	VkExternalFenceProperties efp = {0};
	esp.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES;
	vkGetPhysicalDeviceExternalFenceProperties(phdev, &efi, &efp);

	if((efp.externalFenceFeatures &
			VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT) == 0) {
		error("Vulkan can't export sync_fd fences");
		goto error;
	}

	// init renderpass and pipeline
	if (!init_pipeline(vk_dev)) {
		goto error;
	}

	device->vk_device = vk_dev;
	return vk_dev;

error:
	vk_device_destroy(vk_dev);
	return NULL;
}

bool output_vulkan_setup(struct output *output) {
	struct vk_device *vk_dev = output->device->vk_device;
	assert(vk_dev);
	VkResult res;

	// TODO: i guess we could make it work without explicit fencing
	// if we just stall the gpu before submitting and only re-use images
	// when they are ready to be re-used.
	if (!output->explicit_fencing) {
		error("Vulkan requires explicit fencing, output doesn't support it");
		return false;
	}

	if (output->num_modifiers == 0) {
		error("Output doesn't support any modifiers, vulkan requires modifiers");
		return false;
	}

	// check format support
	// we simply iterate over all the modifiers supported by drm (stored
	// in output) and query with vulkan if the modifier can be used
	// for rendering via vkGetPhysicalDeviceImageFormatProperties2.
	// We are allowed to query it this way (even for modifiers the driver
	// doesn't even know), the function will simply return format_not_supported
	// when it doesn't support/know the modifier.
	// - input -
	VkPhysicalDeviceImageDrmFormatModifierInfoEXT modi = {0};
	modi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;

	VkPhysicalDeviceExternalImageFormatInfo efmti = {0};
	efmti.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
	efmti.pNext = &modi;
	efmti.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

	VkPhysicalDeviceImageFormatInfo2 fmti = {0};
	fmti.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
	fmti.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	fmti.type = VK_IMAGE_TYPE_2D;
	fmti.format = format;
	fmti.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	fmti.pNext = &efmti;

	// - output -
	VkExternalImageFormatProperties efmtp = {0};
	efmtp.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;

	VkImageFormatProperties2 ifmtp = {0};
	ifmtp.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
	ifmtp.pNext = &efmtp;

	// supported modifiers
	uint32_t smod_count = 0;
	uint64_t *smods = calloc(output->num_modifiers, sizeof(*smods));
	assert(smods);
	for(unsigned i = 0u; i < output->num_modifiers; ++i) {
		uint64_t mod = output->modifiers[i];
		modi.drmFormatModifier = mod;
		res = vkGetPhysicalDeviceImageFormatProperties2(vk_dev->phdev,
			&fmti, &ifmtp);
		if (res == VK_ERROR_FORMAT_NOT_SUPPORTED) {
			continue;
		} else if (res != VK_SUCCESS) {
			vk_error(res, "vkGetPhysicalDeviceImageFormatProperties2");
			return false;
		}

		// we need dmabufs with the given format and modifier to be importable
		// otherwise we can't use the modifier
		if ((efmtp.externalMemoryProperties.compatibleHandleTypes &
				VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) == 0) {
			continue;
		}
		if ((efmtp.externalMemoryProperties.externalMemoryFeatures &
				VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0) {
			continue;
		}

		smods[smod_count++] = mod;

		// TODO: query and check basic image properties such as
		// maxExtent. Stored in ifmtp
	}

	if (smod_count == 0) {
		error("No modifier supported by kms and vulkan");
		return false;
	}

	free(output->modifiers);
	output->num_modifiers = smod_count;
	output->modifiers = smods;

	return true;
}

struct buffer *buffer_vk_create(struct device *device, struct output *output)
{
	struct vk_image *img = calloc(1, sizeof(*img));
	struct vk_device *vk_dev = device->vk_device;
	assert(vk_dev);
	uint32_t num_planes;
	int dma_buf_fds[4] = {-1, -1, -1, -1};
	VkResult res;

	// fill buffer info
	img->buffer.output = output;
	img->buffer.render_fence_fd = -1;
	img->buffer.kms_fence_fd = -1;
	img->buffer.format = DRM_FORMAT_XRGB8888;
	img->buffer.format = DRM_FORMAT_XRGB8888;
	img->buffer.width = output->mode.hdisplay;
	img->buffer.height = output->mode.vdisplay;
	uint32_t width = img->buffer.width;
	uint32_t height = img->buffer.height;

	// create gbm bo with modifiers supported by output and vulkan
	img->buffer.gbm.bo = gbm_bo_create_with_modifiers(device->gbm_device,
	   output->mode.hdisplay, output->mode.vdisplay, DRM_FORMAT_XRGB8888,
	   output->modifiers, output->num_modifiers);
	if (!img->buffer.gbm.bo) {
		error("failed to create %u x %u BO\n", output->mode.hdisplay,
			output->mode.vdisplay);
		goto err;
	}

	struct gbm_bo *bo = img->buffer.gbm.bo;
	img->buffer.modifier = gbm_bo_get_modifier(bo);
	num_planes = gbm_bo_get_plane_count(bo);
	VkSubresourceLayout plane_layouts[4] = {0};
	for (unsigned i = 0; i < num_planes; i++) { // like egl-gles.c
		union gbm_bo_handle h;

		/* In hindsight, we got this API wrong. */
		h = gbm_bo_get_handle_for_plane(bo, i);
		if (h.u32 == 0 || h.s32 == -1) {
			error("failed to get handle for BO plane %d (modifier 0x%" PRIx64 ")\n",
			      i, img->buffer.modifier);
			goto err;
		}
		img->buffer.gem_handles[i] = h.u32;

		dma_buf_fds[i] = handle_to_fd(device, img->buffer.gem_handles[i]);
		if (dma_buf_fds[i] == -1) {
			error("failed to get file descriptor for BO plane %d (modifier 0x%" PRIx64 ")\n",
			      i, img->buffer.modifier);
			goto err;
		}

		img->buffer.pitches[i] = gbm_bo_get_stride_for_plane(bo, i);
		if (img->buffer.pitches[i] == 0) {
			error("failed to get stride for BO plane %d (modifier 0x%" PRIx64 ")\n",
			      i, img->buffer.modifier);
			goto err;
		}

		img->buffer.offsets[i] = gbm_bo_get_offset(bo, i);

		plane_layouts[i].offset = img->buffer.offsets[i];
		plane_layouts[i].rowPitch = img->buffer.pitches[i];
		plane_layouts[i].arrayPitch = 0;
		plane_layouts[i].depthPitch = 0;
		plane_layouts[i].size = 0; // vulkan spec says must be 0
	}

	// TODO: could all planes point to the same dma_buf object?
	// we can compare that via the SYS_kcmp syscall on linux,
	// is that valid? In that case we can get away with 1 memory
	// despite multiple planes i guess
	bool disjoint = (num_planes > 1);

	// create image (for dedicated allocation)
	VkImageCreateInfo img_info = {0};
	img_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	img_info.imageType = VK_IMAGE_TYPE_2D;
	img_info.format = format;
	img_info.mipLevels = 1;
	img_info.arrayLayers = 1;
	img_info.samples = VK_SAMPLE_COUNT_1_BIT;
	img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	img_info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	img_info.extent.width = width;
	img_info.extent.height = height;
	img_info.extent.depth = 1;
	img_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	if (disjoint) {
		img_info.flags = VK_IMAGE_CREATE_DISJOINT_BIT;
	}

	VkExternalMemoryImageCreateInfo eimg = {0};
	eimg.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	eimg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	img_info.pNext = &eimg;

	VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info = {0};
	mod_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
	mod_info.drmFormatModifierPlaneCount = num_planes;
	mod_info.drmFormatModifier = img->buffer.modifier;
	mod_info.pPlaneLayouts = plane_layouts;
	eimg.pNext = &mod_info;

	res = vkCreateImage(vk_dev->dev, &img_info, NULL, &img->image);
	if (res != VK_SUCCESS) {
		vk_error(res, "vkCreateImage");
		goto err;
	}

	// import gbm bo memory (planes) and bind them to the image
	unsigned mem_count = disjoint ? num_planes : 1u;
	VkBindImageMemoryInfo bindi[4] = {0};
	VkBindImagePlaneMemoryInfo planei[4] = {0};
	for (unsigned i = 0u; i < mem_count; ++i) {
		struct VkMemoryFdPropertiesKHR fdp = {0};
		fdp.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
		res = vk_dev->api.getMemoryFdPropertiesKHR(vk_dev->dev,
			VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
			dma_buf_fds[i], &fdp);
		if (res != VK_SUCCESS) {
			vk_error(res, "getMemoryFdPropertiesKHR");
			goto err;
		}

		VkImageMemoryRequirementsInfo2 memri = {0};
		memri.image = img->image;
		memri.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;

		VkImagePlaneMemoryRequirementsInfo planeri = {0};
		if (disjoint) {
			planeri.sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO;
			planeri.planeAspect = mem_plane_ascpect(i);
			memri.pNext = &planeri;
		}

		VkMemoryRequirements2 memr = {0};
		memr.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;

		vkGetImageMemoryRequirements2(vk_dev->dev, &memri, &memr);
		int mem = find_mem_type(vk_dev->phdev, 0,
			memr.memoryRequirements.memoryTypeBits & fdp.memoryTypeBits);
		if (mem < 0) {
			error("no valid memory type index");
			goto err;
		}

		VkMemoryAllocateInfo memi = {0};
		memi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		memi.allocationSize = memr.memoryRequirements.size;
		memi.memoryTypeIndex = mem;

		VkImportMemoryFdInfoKHR importi = {0};
		importi.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
		importi.fd = dma_buf_fds[i];
		importi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		memi.pNext = &importi;

		VkMemoryDedicatedAllocateInfo dedi = {0};
		dedi.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
		dedi.image = img->image;
		importi.pNext = &dedi;

		res = vkAllocateMemory(vk_dev->dev, &memi, NULL, &img->memories[i]);
		if (res != VK_SUCCESS) {
			vk_error(res, "vkAllocateMemory failed");
			goto err;
		}

		// fill bind info
		bindi[i].image = img->image;
		bindi[i].memory = img->memories[i];
		bindi[i].memoryOffset = 0;
		bindi[i].sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;

		if (disjoint) {
			planei[i].sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO;
			planei[i].planeAspect = planeri.planeAspect;
			bindi[i].pNext = &planei[i];
		}
	}

	res = vkBindImageMemory2(vk_dev->dev, mem_count, bindi);
	if (res != VK_SUCCESS) {
		vk_error(res, "vkBindMemory failed");
		goto err;
	}

	// create image view and framebuffer for imported image
	VkImageViewCreateInfo view_info = {0};
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = format;
	view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.baseArrayLayer = 0;
	view_info.subresourceRange.layerCount = 1;
	view_info.subresourceRange.baseMipLevel = 0;
	view_info.subresourceRange.levelCount = 1;
	view_info.image = img->image;

	res = vkCreateImageView(vk_dev->dev, &view_info, NULL, &img->image_view);
	if (res != VK_SUCCESS) {
		vk_error(res, "vkCreateImageView failed");
		goto err;
	}

	VkFramebufferCreateInfo fb_info = {0};
	fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fb_info.attachmentCount = 1;
	fb_info.pAttachments = &img->image_view;
	fb_info.renderPass = vk_dev->rp;
	fb_info.width = width;
	fb_info.height = height;
	fb_info.layers = 1;
	res = vkCreateFramebuffer(vk_dev->dev, &fb_info, NULL, &img->fb);
	if (res != VK_SUCCESS) {
		vk_error(res, "vkCreateFramebuffer");
		goto err;
	}

	// create and record render command buffer
	VkCommandBufferAllocateInfo cmd_buf_info = {0};
	cmd_buf_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmd_buf_info.commandPool = vk_dev->command_pool;
	cmd_buf_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmd_buf_info.commandBufferCount = 1u;
	res = vkAllocateCommandBuffers(vk_dev->dev, &cmd_buf_info, &img->cb);
	if (res != VK_SUCCESS) {
		vk_error(res, "vkAllocateCommandBuffers");
		goto err;
	}

	VkCommandBufferBeginInfo begin_info = {0};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(img->cb, &begin_info);

	// acquire ownership of the image we want to render
	// TODO: as already mentioned on device creation, strictly
	// speaking we need queue_family_foreign here. But since that
	// isn't supported on any mesa driver yet (not even a pr) we
	// try our luck with queue_family_external (which should work for
	// same gpu i guess?). But again: THIS IS NOT GUARANTEED TO WORK,
	// THE STANDARD DOESN'T SUPPORT IT. JUST A TEMPORARY DROP-IN UNTIL
	// THE REAL THING IS SUPPORTED
	uint32_t ext_qfam = VK_QUEUE_FAMILY_EXTERNAL;

	VkImageMemoryBarrier barrier = {0};
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	barrier.srcQueueFamilyIndex = ext_qfam;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL; // doesn't matter really
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.dstQueueFamilyIndex = vk_dev->queue_family;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.layerCount = 1;
	barrier.subresourceRange.levelCount = 1;
	// NOTE: not completely sure about the stages
	vkCmdPipelineBarrier(img->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL,
		0, NULL, 1, &barrier);

	// render pass
	// Renderpass currently specifies don't care as loadOp (since we
	// render the full framebuffer anyways), so we don't need
	// clear values
	// VkClearValue clear_value;
	// clear_value.color.float32[0] = 0.05f;
	// clear_value.color.float32[1] = 0.05f;
	// clear_value.color.float32[2] = 0.05f;
	// clear_value.color.float32[3] = 1.f;
	VkRect2D rect = {{0, 0}, {width, height}};

	VkRenderPassBeginInfo rp_info = {0};
	rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rp_info.renderArea = rect;
	rp_info.renderPass = vk_dev->rp;
	rp_info.framebuffer = img->fb;
	// rp_info.clearValueCount = 1;
	// rp_info.pClearValues = &clear_value;
	vkCmdBeginRenderPass(img->cb, &rp_info, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport vp = {0.f, 0.f, (float) width, (float) height, 0.f, 1.f};
	vkCmdSetViewport(img->cb, 0, 1, &vp);
	vkCmdSetScissor(img->cb, 0, 1, &rect);

	vkCmdBindPipeline(img->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_dev->pipe);
	vkCmdDraw(img->cb, 4, 1, 0, 0);

	// release ownership of the image we want to render
	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barrier.srcQueueFamilyIndex = vk_dev->queue_family;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	barrier.dstQueueFamilyIndex = ext_qfam;
	vkCmdPipelineBarrier(img->cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL,
		0, NULL, 1, &barrier);

	vkEndCommandBuffer(img->cb);

	// create semaphore that will be used for importing bufer->kms_fence_fd
	VkSemaphoreCreateInfo sem_info = {0};
	sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	res = vkCreateSemaphore(vk_dev->dev, &sem_info, NULL, &img->buffer_semaphore);
	if (res != VK_SUCCESS) {
		vk_error(res, "vkCreateSemaphore");
		goto err;
	}

	// create render fence, export it as syncfile to buffer->render_fence_fd
	VkExportFenceCreateInfo efi = {0};
	efi.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO;
	efi.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;

	VkFenceCreateInfo fence_info = {0};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_info.pNext = &efi;
	res = vkCreateFence(vk_dev->dev, &fence_info, NULL, &img->render_fence);
	if (res != VK_SUCCESS) {
		vk_error(res, "vkCreateFence");
		goto err;
	}

	VkFenceGetFdInfoKHR fdi = {0};
	fdi.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR;
	fdi.fence = img->render_fence;
	fdi.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT;
	res = vk_dev->api.getFenceFdKHR(vk_dev->dev, &fdi, &img->buffer.render_fence_fd);
	if (res != VK_SUCCESS) {
		vk_error(res, "vkGetFenceFdKHR");
		goto err;
	}

	// success!
	return &img->buffer;

err:
	for (size_t i = 0; i < ARRAY_LENGTH(dma_buf_fds); i++) {
		if (dma_buf_fds[i] != -1) {
			close(dma_buf_fds[i]);
		}
	}
	buffer_destroy(&img->buffer);
	return NULL;
}

void buffer_vk_destroy(struct device *device, struct buffer *buffer)
{
	struct vk_image *img = (struct vk_image *)buffer;
	// TODO: destroy everything in reverse order
}

bool buffer_vk_fill(struct buffer *buffer, int frame_num)
{
	((void) frame_num);
	struct vk_image *img = (struct vk_image *)buffer;
	struct vk_device *vk_dev = buffer->output->device->vk_device;
	assert(vk_dev);
	VkResult res;

	// importing semaphore transfers ownership to vulkan
	// importing it as temporary (which is btw the only supported way
	// for sync_fd semaphores) means that after the next wait operation,
	// the semaphore is reset to its prior state, i.e. we can import
	// a new semaphore next frame.
	// NOTE: as mentioned in the egl backend, the whole kms_fence_fd
	// is not needed with the currnet architecture of the application
	// since we only re-use buffers after kms is finished with them.
	// Should probably work on that.
	VkImportSemaphoreFdInfoKHR isi = {0};
	isi.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
	isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
	isi.fd = dup(buffer->kms_fence_fd);
	isi.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
	isi.fd = buffer->kms_fence_fd;
	isi.semaphore = img->buffer_semaphore;
	res = vk_dev->api.importSemaphoreFdKHR(vk_dev->dev, &isi);
	if (res != VK_SUCCESS) {
		vk_error(res, "vkImportSemaphoreFdKHR");
		return false;
	}

	// reset the fence from the previous frame
	res = vkResetFences(vk_dev->dev, 1, &img->render_fence);
	if (res != VK_SUCCESS) {
		vk_error(res, "vkResetFences");
		return false;
	}

	// submit the buffers command buffer
	// - it waits for the kms_fence_fd semaphore
	// - upon completion, it signals the render fence
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submission = {0};
	submission.commandBufferCount = 1;
	submission.pCommandBuffers = &img->cb;
	submission.waitSemaphoreCount = 1;
	submission.pWaitDstStageMask = &stage;
	submission.pWaitSemaphores = &img->buffer_semaphore;
	res = vkQueueSubmit(vk_dev->queue, 1, &submission, img->render_fence);
	if (res != VK_SUCCESS) {
		vk_error(res, "vkQueueSubmit");
		return false;
	}

	return true;
}
