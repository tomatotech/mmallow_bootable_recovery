#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/types.h>

#include "common.h"
#include "minzip/SysUtil.h"
#include "minzip/Zip.h"
#include "cutils/properties.h"
#include "ui.h"
#include "security.h"


/**
  *  --- judge platform whether match with zip image or not
  *
  *  @platformEncryptStatus: 0: platform unencrypted, 1: platform encrypted
  *  @imageEncryptStatus:    0: image unencrypted, 1: image encrypted
  *  @imageName:   image name
  *  @imageBuffer: image data address
  *  @imageSize:   image data size
  *
  *  return value:
  *  <0: failed
  *  =0: not match
  *  >0: match
  */
static int IsPlatformMachWithZipArchiveImage(
        const int platformEncryptStatus,
        const int imageEncryptStatus,
        const char *imageName,
        const unsigned char *imageBuffer,
        const int imageSize)
{
    int fd = -1, ret = -1;
    ssize_t result = -1;

    if (strcmp(imageName, BOOT_IMG) &&
        strcmp(imageName, RECOVERY_IMG) &&
        strcmp(imageName, BOOTLOADER_IMG)) {
        printf("can't support %s at present\n",
            imageName);
        return -1;
    }

    if (imageBuffer == NULL) {
        printf("havn't malloc space for %s\n",
            imageName);
        return -1;
    }

    if (imageSize <= 0) {
        printf("%s size is %d\n",
            imageName, imageSize);
        return -1;
    }

    switch (platformEncryptStatus) {
        case 0: {
            if (!imageEncryptStatus) {
                ret = 1;
            } else {
                ret = 0;
            }
            break;
        }

        case 1: {
            if (!imageEncryptStatus) {
                ret = 0;
            } else {
                fd = open(DEFEND_KEY, O_RDWR);
                if (fd <= 0) {
                    printf("open %s failed (%s)\n",
                        DEFEND_KEY, strerror(errno));
                    return -1;
                }
                result = write(fd, imageBuffer, imageSize);// check rsa
                printf("write %s datas to %s. [imgsize:%d, result:%d, %s]\n",
                    imageName, DEFEND_KEY, imageSize, result,
                    (result == 1) ? "match" :
                    (result == -2) ? "not match" : "failed or not support");
                if (result == 1) {
                    ret = 1;
                } else if(result == -2) {
                    ret = 0;
                } else {    // failed or not support
                    ret = -1;
                }
                close(fd);
                fd = -1;
            }
            break;
        }
    }

    return ret;
}

/**
  *  --- check bootloader.img whether encrypt or not
  *
  *  @imageName:   bootloader.img
  *  @imageBuffer: bootloader.img data address
  *
  *  return value:
  *  <0: failed
  *  =0: unencrypted
  *  >0: encrypted
  */
static int IsBootloaderImageEncrypted(
        const char *imageName,
        const unsigned char *imageBuffer)
{
    const unsigned char *pImageAddr = imageBuffer;
    const unsigned char *pEncryptedBootloaderInfoBufAddr = NULL;

    // Don't modify. unencrypt bootloader info
    const int bootloaderEncryptInfoOffset = 0x1b0;
    const unsigned char unencryptedBootloaderInfoBuf[] =
            { 0x4D, 0x33, 0x48, 0x48, 0x52, 0x45, 0x56, 0x30 };

    if (strcmp(imageName, BOOTLOADER_IMG)) {
        printf("this image must be %s,but it is %s\n",
            BOOTLOADER_IMG, imageName);
        return -1;
    }

    if (imageBuffer == NULL) {
        printf("havn't malloc space for %s\n",
            imageName);
        return -1;
    }

    pEncryptedBootloaderInfoBufAddr = pImageAddr + bootloaderEncryptInfoOffset;

    if (!memcmp(unencryptedBootloaderInfoBuf, pEncryptedBootloaderInfoBufAddr,
        ARRAY_SIZE(unencryptedBootloaderInfoBuf))) {
        return 0;   // unencrypted
    }

    return 1;       // encrypted
}

/**
  *  --- check zip archive image whether encrypt or not
  *   image is bootloader.img/boot.img/recovery.img
  *
  *  @imageName:   image name
  *  @imageBuffer: image data address
  *  @imageSize:   image data size
  *
  *  return value:
  *  <0: failed
  *  =0: unencrypted
  *  >0: encrypted
  */
static int IsZipArchiveImageEncrypted(
        const char *imageName,
        const unsigned char *imageBuffer,
        const int imageSize)
{
    int ret = -1;
    const unsigned char *pImageAddr = imageBuffer;

    if (strcmp(imageName, BOOT_IMG) &&
        strcmp(imageName, RECOVERY_IMG) &&
        strcmp(imageName, BOOTLOADER_IMG)) {
        printf("can't support %s at present\n",
            imageName);
        return -1;
    }

    if (imageBuffer == NULL) {
        printf("havn't malloc space for %s\n",
            imageName);
        return -1;
    }

    if (imageSize <= 0) {
        printf("%s size is %d\n",
            imageName, imageSize);
        return -1;
    }

    if (!strcmp(imageName, BOOTLOADER_IMG)) {
        return IsBootloaderImageEncrypted(imageName, imageBuffer);
    }

    const pT_SecureBootImgHdr encryptSecureBootImgHdr =
        (const pT_SecureBootImgHdr)pImageAddr;
    const pT_EncryptBootImgInfo encryptBootImgInfo =
        &encryptSecureBootImgHdr->encryptBootImgInfo;

    secureDbg("magic:%s, version:0x%04x, totalLenAfterEncrypted:0x%0x\n",
        encryptBootImgInfo->magic, encryptBootImgInfo->version,
        encryptBootImgInfo->totalLenAfterEncrypted);

    ret = memcmp(encryptBootImgInfo->magic, SECUREBOOT_MAGIC,
        strlen(SECUREBOOT_MAGIC));
    if (!ret && encryptBootImgInfo->version != 0x0) {
        return 1;   // encrypted
    }

    return 0;       // unencrypted
}

/**
  *  --- check platform whether encrypt or not
  *
  *  return value:
  *  <0: failed
  *  =0: unencrypted
  *  >0: encrypted
  */
static int IsPlatformEncrypted(void)
{
    int fd = -1, ret = -1;
    ssize_t count = 0;
    char rBuf[128] = {0};
    char platform[PROPERTY_VALUE_MAX+1] = {0};

    fd = open(SECURE_CHECK, O_RDONLY);
    if (fd <= 0) {
        printf("open %s failed (%s)\n",
            SECURE_CHECK, strerror(errno));
        return -1;
    }

    property_get("ro.build.product", platform, "unknow");
    count = read(fd, rBuf, sizeof(rBuf) - 1);
    if (count <= 0) {
        printf("read %s failed (count:%d)\n",
            SECURE_CHECK, count);
        return -1;
    }
    rBuf[count] = '\0';

    if (!strcmp(rBuf, s_pStatus[UNENCRYPT])) {
        printf("check platform(%s): unencrypted\n", platform);
        ret = 0;
    } else if (!strcmp(rBuf, s_pStatus[ENCRYPT])) {
        printf("check platform(%s): encrypted\n", platform);
        ret = 1;
    } else if (!strcmp(rBuf, s_pStatus[FAIL])) {
        printf("check platform(%s): failed\n", platform);
    } else {
        printf("check platform(%s): %s\n", platform, rBuf);
    }

    if (fd > 0) {
        close(fd);
        fd = -1;
    }

    return ret;
}

/**
  *  --- get upgrade package image data
  *
  *  @zipArchive: zip archive object
  *  @imageName:  upgrade package image's name
  *  @imageSize:  upgrade package image's size
  *
  *  return value:
  *  <0: failed
  *  =0: can't find image
  *  >0: get image data successful
  */
static unsigned char *s_pImageBuffer = NULL;
static int GetZipArchiveImage(
        const ZipArchive zipArchive,
        const char *imageName,
        int *imageSize)
{
    bool success = false;

    const ZipEntry *pZipEntry = mzFindZipEntry(&zipArchive, imageName);
    if (pZipEntry == NULL) {
        return 0;
    }

    *imageSize = mzGetZipEntryUncompLen(pZipEntry);
    if (*imageSize <= 0) {
        printf("can't get package entry uncomp len(%d) (%s)\n",
            *imageSize, strerror(errno));
        return -1;
    }

    if (s_pImageBuffer != NULL) {
        free(s_pImageBuffer);
        s_pImageBuffer = NULL;
    }

    s_pImageBuffer = (unsigned char *)calloc(*imageSize, sizeof(unsigned char));
    if (!s_pImageBuffer) {
        printf("can't malloc %d size space (%s)\n",
            *imageSize, strerror(errno));
        return -1;
    }

    success = mzExtractZipEntryToBuffer(&zipArchive, pZipEntry, s_pImageBuffer);
    if (!success) {
        printf("can't extract package entry to image buffer\n");
        goto FREE_IMAGE_MEM;
    }

    return 1;


FREE_IMAGE_MEM:
    if (s_pImageBuffer != NULL) {
        free(s_pImageBuffer);
        s_pImageBuffer = NULL;
    }

    return -1;
}

/**
  *  --- check platform and upgrade package whether
  *  encrypted,if all encrypted,rsa whether all the same
  *
  *  @zipPath: upgrade package path
  *
  *  return value:
  *  =-1: failed; not allow upgrade
  *  = 0: check not match; not allow upgrade
  *  = 1: check match; allow upgrade
  *  = 2: kernel not support secure check; allow upgrade
  */
int RecoverySecureCheck(const char *zipPath)
{
    int i = 0, ret = -1, err = -1;
    int imageSize = 0;
    int platformEncryptStatus = 0, imageEncryptStatus = 0;
    const char *pImageName[] = {
            BOOTLOADER_IMG,
            BOOT_IMG,
            RECOVERY_IMG };

    if (access(SECURE_CHECK, F_OK) || access(DEFEND_KEY, F_OK)) {
        secureDbg("kernel doesn't support secure check\n");
        return 2;   // kernel doesn't support
    }

    ui->Print("\n-- Secure Check...\n");
    platformEncryptStatus = IsPlatformEncrypted();
    if (platformEncryptStatus < 0) {
        return -1;
    }

    MemMapping map;
    if (sysMapFile(zipPath, &map) != 0) {
        printf("map file failed\n");
        return -1;
    }

    ZipArchive zipArchive;
    err = mzOpenZipArchive(map.addr, map.length, &zipArchive);
    if (err != 0) {
        printf("can't open %s (%s)\n",
            zipPath, err != -1 ? strerror(err) : "bad");
        goto ERR1;
    }

    for (i = 0; i < ARRAY_SIZE(pImageName); i++) {
        ret = GetZipArchiveImage(zipArchive, pImageName[i], &imageSize);
        if (ret < 0) {
            printf("get %s datas failed\n", pImageName[i]);
           goto ERR2;
        } else if (ret == 0) {
            printf("check %s: not find,skiping...\n", pImageName[i]);
            continue;
        } else if (ret > 0) {
            secureDbg("get %s datas(size:0x%0x, addr:0x%x) successful\n",
                pImageName[i], imageSize, (int)s_pImageBuffer);
            imageEncryptStatus = IsZipArchiveImageEncrypted(
                pImageName[i], s_pImageBuffer, imageSize);
            printf("check %s: %s\n",
                pImageName[i], (imageEncryptStatus < 0) ? "failed" :
                !imageEncryptStatus ? "unencrypted" : "encrypted");
            if (imageEncryptStatus < 0) {
                ret = -1;
                goto ERR3;
            }

            ret = IsPlatformMachWithZipArchiveImage(
                platformEncryptStatus, imageEncryptStatus, pImageName[i],
                s_pImageBuffer, imageSize);
            if (ret < 0) {
                printf("%s match platform failed\n", pImageName[i]);
                goto ERR3;
            } else if (ret == 0) { // if one of image doesn't match with platform,exit
                printf("%s doesn't match platform\n", pImageName[i]);
                goto ERR3;
            } else {
                secureDbg("%s match platform\n", pImageName[i]);
            }

            if (s_pImageBuffer != NULL) {
                free(s_pImageBuffer);
                s_pImageBuffer = NULL;
            }
        }
    }

    mzCloseZipArchive(&zipArchive);
    sysReleaseMap(&map);

    return 1;


ERR3:
    if (s_pImageBuffer != NULL) {
        free(s_pImageBuffer);
        s_pImageBuffer = NULL;
    }

ERR2:
    mzCloseZipArchive(&zipArchive);

ERR1:
    sysReleaseMap(&map);

    return ret;
}