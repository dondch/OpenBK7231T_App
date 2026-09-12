#ifndef __OTA_H__
#define __OTA_H__

// NOTE: this offset was taken from BkDriverFlash.c
// search for BK_PARTITION_OTA

#if PLATFORM_BK7231T
#define START_ADR_OF_BK_PARTITION_OTA 0x132000
#elif PLATFORM_BK7231N
#define START_ADR_OF_BK_PARTITION_OTA 0x12A000
#elif PLATFORM_BK7252
// BK-W8 is a 4 MB part whose bootloader keeps its download partition at 0x210000,
// so BkDriverFlash.c puts BK_PARTITION_OTA there for the 4 MB table. The default
// below is 0x132000, which on a 4 MB layout is inside the running application: an
// OTA would erase the firmware performing it. Keep the two in step.
#define START_ADR_OF_BK_PARTITION_OTA 0x210000
#else
// TODO
#define START_ADR_OF_BK_PARTITION_OTA 0x132000
#endif

// Upper bound for an OTA write. The REST endpoints bound it with LFS_BLOCKS_END and
// a literal 0x200000, and on a 4 MB layout both sit below the download partition, so
// every upload is refused once the start address above is correct. Where this is
// defined it is used instead. 0x3C1000 is the end of the bootloader's download
// partition, 0x210000 + 0x1B1000.
#if PLATFORM_BK7252
#define END_ADR_OF_BK_PARTITION_OTA 0x3C1000
#endif

/// @brief Handle OTA request. Only used for Beken SDK.
/// @param urlin 
void otarequest(const char *urlin);


/***** SDK independent code from this point. ******/

/// @brief Indicates current OTA progress status. A non -ve value indicates active OTA.
/// @return 
int OTA_GetProgress();

/// @brief Reset OTA progress status to -1. This can be called from other SDKs.
/// @param value 
void OTA_ResetProgress();

/// @brief Increment OTA progress status. This can be called from other SDKs.
/// @param value 
void OTA_IncrementProgress(int value);

/// @brief Get total OTA size.
/// @return 
int OTA_GetTotalBytes();

/// @brief Set total OTA size. This can be called from other SDKs.
/// @param value 
void OTA_SetTotalBytes(int value);

int HAL_FlashRead(char*buffer, int readlen, int startaddr);

#endif /* __OTA_H__ */

