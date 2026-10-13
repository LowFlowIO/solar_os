#pragma once

#include "solar_os_storage.h"

#define SOLAR_OS_DISK_IMAGE_MAX 4U
#define SOLAR_OS_DISK_IMAGE_MAX_BYTES 0x7ffffe00U

/* Fixed-size, 512-byte-sector raw images. A claim belongs either to a host
 * FAT mount or to one foreground worker, including while it is suspended. */
esp_err_t solar_os_disk_image_open(const char *path, const void *owner,
                                  uint32_t *handle, uint32_t *size);
esp_err_t solar_os_disk_image_read(uint32_t handle, const void *owner,
                                  uint32_t offset, void *data, size_t length);
esp_err_t solar_os_disk_image_write(uint32_t handle, const void *owner,
                                   uint32_t offset, const void *data, size_t length);
esp_err_t solar_os_disk_image_sync(uint32_t handle, const void *owner);
esp_err_t solar_os_disk_image_close(uint32_t handle, const void *owner);
esp_err_t solar_os_disk_image_close_owner(const void *owner);

esp_err_t solar_os_disk_image_mount(const char *path, const char *mount_point,
                                   unsigned partition);
esp_err_t solar_os_disk_image_unmount(const char *target);
/* Creates a new MBR disk with one FAT partition; never overwrites a file. */
esp_err_t solar_os_disk_image_create(const char *path, uint32_t size_mib);
size_t solar_os_disk_image_mount_count(void);
bool solar_os_disk_image_get_mount(size_t index, solar_os_storage_mount_info_t *mount);
bool solar_os_disk_image_get_block(size_t index, solar_os_storage_block_t *block);
bool solar_os_disk_image_path_busy(const char *path);
bool solar_os_disk_image_backing_busy(const char *mount_point);
/* Serialize filesystem topology changes with image admission. Recursive so
 * storage controls can call image mount/unmount and metadata helpers. */
void solar_os_disk_image_registry_lock(void);
void solar_os_disk_image_registry_unlock(void);
