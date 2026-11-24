/**
  The UFS host controller interface (JEDEC JESD223, UFSHCI 3.0) as zuma's controller has it: the standard registers at
  the start of ufs@13200000, the transfer request descriptor, and the UPIUs the driver sends and reads.

  zuma's controller takes the standard's units (offsets in double words, the PRDT's length in entries: as the
  bootloader's own descriptors show), but a PRDT entry holds at most 4 KiB: the vendor registers TXPRDT/RXPRDT_ENTRY_SIZE
  (vs_hci + 0x00/0x04) say 12, 2^12 bytes, as the bootloader leaves them.

  Copyright (c) 2026, AkIOS contributors.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef _UFSHCI_H_
#define _UFSHCI_H_

#include <Base.h>

//
// Registers
//
#define UFS_CAP       0x00    // bits 4:0: transfer request slots - 1
#define UFS_VER       0x08
#define UFS_IS        0x20    // interrupt status (write 1 to clear)
#define UFS_IE        0x24
#define UFS_HCS       0x30
#define UFS_HCE       0x34
#define UFS_UTRLBA    0x50
#define UFS_UTRLBAU   0x54
#define UFS_UTRLDBR   0x58    // doorbell
#define UFS_UTRLCLR   0x5C    // write 0 to a bit to clear that slot
#define UFS_UTRLRSR   0x60    // run-stop
#define UFS_UTRLCNR   0x64    // completion notification (write 1 to clear)
#define UFS_UTMRLRSR  0x80
#define UFS_UICCMD    0x90
#define UFS_UICARG1   0x94
#define UFS_UICARG2   0x98    // bits 7:0: the result (0 = success)
#define UFS_UICARG3   0x9C

#define UIC_DME_GET          0x01
#define UIC_DME_SET          0x02
#define UIC_DME_LINKSTARTUP  0x16
#define IS_UCCS              (1 << 10)  // UIC command completed
#define VS_GPIO_OUT          0x70       // vendor: the device's reset line (1 = released)

//
// Samsung's vendor HCI registers (the phone's tree: the second range of ufs@13200000, at 0x1100): a request slot
// must be marked as carrying a SCSI command (its "nexus type") before its doorbell, or the controller does not take it
//
#define UFS_VS_HCI           0x1100
#define VS_UTRL_NEXUS_TYPE   0x40
#define VS_UTMRL_NEXUS_TYPE  0x44

#define HCS_DP        (1 << 0)
#define HCS_UTRLRDY   (1 << 1)
#define HCS_UTMRLRDY  (1 << 2)
#define HCS_UCRDY     (1 << 3)

#define IS_UTRCS      (1 << 0)    // a transfer request completed
#define IS_ERRORS     0x0000FC1Cu // UE, DFES, UTPES, HCFES, SBFES, CEFES and the like: anything that is not a completion

#pragma pack(1)

//
// The UTP transfer request descriptor (32 bytes, one per slot)
//
#define UTRD_CT_UFS        (1u << 28)
#define UTRD_DD_NONE       (0u << 25)
#define UTRD_DD_TO_DEVICE  (1u << 25)
#define UTRD_DD_FROM_DEVICE (2u << 25)
#define UTRD_INTERRUPT     (1u << 24)
#define OCS_SUCCESS        0x00
#define OCS_INVALID        0x0F

typedef struct {
  UINT32  Dw0;            // command type, data direction, interrupt
  UINT32  Dw1;
  UINT32  Dw2;            // bits 7:0: overall command status
  UINT32  Dw3;
  UINT32  UcdBase;        // the command descriptor, 128-byte aligned
  UINT32  UcdBaseHigh;
  UINT16  RespLength;     // double words
  UINT16  RespOffset;     // double words
  UINT16  PrdtLength;     // entries
  UINT16  PrdtOffset;     // double words
} UTRD;

//
// The command descriptor: the command UPIU, the response UPIU, then the PRDT
//
#define UPIU_NOP_OUT       0x00
#define UPIU_NOP_IN        0x20
#define UPIU_QUERY         0x16
#define UPIU_QUERY_RESP    0x36
#define QUERY_FN_READ      0x01
#define QUERY_FN_WRITE     0x81
#define QUERY_OP_READ_FLAG 0x05
#define QUERY_OP_SET_FLAG  0x06
#define FLAG_DEVICE_INIT   0x01
#define UPIU_COMMAND       0x01
#define UPIU_RESPONSE      0x21
#define UPIU_FLAG_READ     0x40
#define UPIU_FLAG_WRITE    0x20

typedef struct {
  UINT8   Type;           // UPIU_COMMAND
  UINT8   Flags;
  UINT8   Lun;
  UINT8   TaskTag;
  UINT8   CommandSet;     // 0: SCSI
  UINT8   Reserved1[3];
  UINT8   EhsLength;
  UINT8   Reserved2;
  UINT16  DataSegmentLength;    // big endian
  UINT32  ExpectedLength;       // big endian
  UINT8   Cdb[16];
} COMMAND_UPIU;

typedef struct {
  UINT8   Type;           // UPIU_RESPONSE
  UINT8   Flags;
  UINT8   Lun;
  UINT8   TaskTag;
  UINT8   CommandSet;
  UINT8   Reserved1;
  UINT8   Response;       // 0: the target did it
  UINT8   Status;         // the SCSI status
  UINT8   EhsLength;
  UINT8   DeviceInfo;
  UINT16  DataSegmentLength;    // big endian: the sense data's length + 2
  UINT32  ResidualCount;        // big endian
  UINT8   Reserved2[16];
  UINT16  SenseLength;          // big endian
  UINT8   Sense[18];
} RESPONSE_UPIU;

typedef struct {
  UINT32  Address;
  UINT32  AddressHigh;
  UINT32  Reserved;
  UINT32  ByteCount;      // bits 17:0: bytes - 1
} PRDT_ENTRY;

#pragma pack()

#define PRDT_MAX_BYTES     SIZE_4KB                 // vs_hci TXPRDT/RXPRDT_ENTRY_SIZE = 12
#define PRDT_ENTRIES       4096                     // up to 16 MiB a command, as the bootloader reads
#define UCD_COMMAND_OFFSET 0x000
#define UCD_RESPONSE_OFFSET 0x020
#define UCD_PRDT_OFFSET    0x080
#define UCD_SIZE           (UCD_PRDT_OFFSET + PRDT_ENTRIES * sizeof (PRDT_ENTRY))

STATIC_ASSERT (sizeof (UTRD) == 32, "UTRD");
STATIC_ASSERT (sizeof (COMMAND_UPIU) == 32, "command UPIU");
STATIC_ASSERT (OFFSET_OF (RESPONSE_UPIU, SenseLength) == 32, "response UPIU");
STATIC_ASSERT (sizeof (PRDT_ENTRY) == 16, "PRDT entry");

#endif
