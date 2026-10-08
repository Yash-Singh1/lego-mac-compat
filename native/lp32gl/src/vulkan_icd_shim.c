/* libvulkan.1.dylib for LP32GL: Zink dlopens "@rpath/libvulkan.1.dylib" and
 * only needs vkGetInstanceProcAddr/vkGetDeviceProcAddr from it.  Rather than
 * shipping the Khronos loader and an ICD manifest, this forwards straight to
 * the KosmicKrisp ICD sitting next to it. */
#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan_core.h>

#define EXPORT __attribute__((visibility("default")))

typedef VkResult (*negotiate_fn)(uint32_t *version);

static PFN_vkGetInstanceProcAddr icd_get_instance_proc_addr;
static PFN_vkGetDeviceProcAddr icd_get_device_proc_addr;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void load_icd(void)
{
    Dl_info info;
    char path[PATH_MAX];
    const char *override = getenv("LP32GL_VULKAN_ICD");
    if (override && override[0]) {
        snprintf(path, sizeof path, "%s", override);
    } else if (dladdr((void *)load_icd, &info) && info.dli_fname) {
        snprintf(path, sizeof path, "%s", info.dli_fname);
        char *slash = strrchr(path, '/');
        if (slash) slash[1] = 0; else path[0] = 0;
        strlcat(path, "libvulkan_kosmickrisp.dylib", sizeof path);
    } else {
        snprintf(path, sizeof path, "libvulkan_kosmickrisp.dylib");
    }
    void *icd = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!icd) {
        fprintf(stderr, "lp32gl: cannot load Vulkan driver %s: %s\n", path, dlerror());
        return;
    }
    negotiate_fn negotiate = (negotiate_fn)dlsym(icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (negotiate) {
        uint32_t version = 7;
        negotiate(&version);
    }
    icd_get_instance_proc_addr = (PFN_vkGetInstanceProcAddr)dlsym(icd, "vk_icdGetInstanceProcAddr");
    if (!icd_get_instance_proc_addr)
        fprintf(stderr, "lp32gl: %s has no vk_icdGetInstanceProcAddr\n", path);
}

/* Layers are a loader concept; an ICD has none to report. */
static VKAPI_ATTR VkResult VKAPI_CALL enumerate_instance_layers(uint32_t *count,
                                                                VkLayerProperties *properties)
{
    (void)properties;
    *count = 0;
    return VK_SUCCESS;
}

EXPORT PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    if (name && !strcmp(name, "vkEnumerateInstanceLayerProperties"))
        return (PFN_vkVoidFunction)enumerate_instance_layers;
    pthread_once(&once, load_icd);
    if (!icd_get_instance_proc_addr) return NULL;
    if (instance && !icd_get_device_proc_addr)
        icd_get_device_proc_addr =
            (PFN_vkGetDeviceProcAddr)icd_get_instance_proc_addr(instance, "vkGetDeviceProcAddr");
    return icd_get_instance_proc_addr(instance, name);
}

EXPORT PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice device, const char *name)
{
    /* Known once any instance-level lookup has happened, which a device
       requires. */
    return icd_get_device_proc_addr ? icd_get_device_proc_addr(device, name) : NULL;
}
