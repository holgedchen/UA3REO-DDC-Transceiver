#include "usb_device.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_ua3reo.h"
#include "usbd_debug_if.h"
#include "usbd_cat_if.h"
#include "usbd_audio_if.h"
#include "usbd_iq_if.h"
#include "usbd_storage_if.h"
#include "functions.h"

USBD_HandleTypeDef hUsbDeviceFS = {0};

void MX_USB_DEVICE_Init(void)
{
  memset(&hUsbDeviceFS, 0x00, sizeof hUsbDeviceFS);
  /* Init Device Library, add supported class and start the library. */
  if (USBD_Init(&hUsbDeviceFS, &FS_Desc, DEVICE_FS) != USBD_OK)
  {
    Error_Handler();
  }
  if (USBD_RegisterClass(&hUsbDeviceFS, &USBD_UA3REO) != USBD_OK)
  {
    Error_Handler();
  }
	#if HRDW_HAS_USB_DEBUG
  if (USBD_DEBUG_RegisterInterface(&hUsbDeviceFS, &USBD_DEBUG_fops_FS) != USBD_OK)
  {
    Error_Handler();
  }
	#endif
	#if HRDW_HAS_USB_CAT
  if (USBD_CAT_RegisterInterface(&hUsbDeviceFS, &USBD_CAT_fops_FS) != USBD_OK)
  {
    Error_Handler();
  }
	#endif
  if (USBD_AUDIO_RegisterInterface(&hUsbDeviceFS, &USBD_AUDIO_fops_FS) != USBD_OK)
  {
    Error_Handler();
  }
	#if HRDW_HAS_USB_IQ
	if (USBD_IQ_RegisterInterface(&hUsbDeviceFS, &USBD_IQ_fops_FS) != USBD_OK)
  {
    Error_Handler();
  }
	#endif
	#if HRDW_HAS_SD
  if (USBD_MSC_RegisterStorage(&hUsbDeviceFS, &USBD_Storage_Interface_fops_FS) != USBD_OK)
  {
    Error_Handler();
  }
	#endif
  if (USBD_Start(&hUsbDeviceFS) != USBD_OK)
  {
    Error_Handler();
  }

	#ifdef STM32H743xx
  HAL_PWREx_EnableUSBVoltageDetector();
	#endif
}

void MX_USB_DevDisconnect(void)
{
  if (USBD_DeInit(&hUsbDeviceFS))
    HAL_PCD_DevDisconnect(&hpcd_USB_OTG_FS);
}
