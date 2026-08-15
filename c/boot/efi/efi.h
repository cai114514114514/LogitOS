/* boot/efi/efi.h -- the slice of UEFI this loader actually calls, written out
 * by hand.
 *
 * WHY BY HAND. The point of this milestone is that LogitOS's bottom layer is
 * its own: gnu-efi and edk2 are the two things a UEFI loader normally starts
 * from, and both are third-party trees an order of magnitude larger than the
 * loader. Neither is installed here either. So the structures below are
 * transcribed from the UEFI 2.10 specification, and ONLY the members this
 * loader reaches -- everything before a member it calls has to be present
 * (offsets are the ABI), everything after it does not.
 *
 * WHY THE _Static_asserts. A UEFI protocol is a struct of function pointers
 * with no names on the wire: an extra or missing member does not fail to
 * compile, it shifts every later member by 8 bytes and the first call through
 * the wrong slot jumps into whatever the firmware happened to put there. That
 * failure has no diagnostic -- the machine resets or hangs with the screen
 * still showing the firmware logo. So every member offset this loader depends
 * on is asserted against the number in the spec table, at compile time. A
 * mistranscription becomes a build error instead of a black screen.
 *
 * CALLING CONVENTION: UEFI on x86_64 is Microsoft x64 (UEFI 2.10 sec 2.3.4).
 * The build already targets x86_64-unknown-windows so that is the default, but
 * EFIAPI is spelled out anyway -- a reader should not have to know the target
 * triple to know how these are called.
 */
#ifndef LOGIT_EFI_H
#define LOGIT_EFI_H

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef signed   long long INT64;
typedef unsigned short     CHAR16;      /* UEFI strings are UCS-2 (sec 2.3.1) */
typedef UINT64             UINTN;       /* 64-bit on x86_64 */
typedef UINT64             EFI_STATUS;
typedef void              *EFI_HANDLE;
typedef UINT64             EFI_PHYSICAL_ADDRESS;
typedef UINT64             EFI_VIRTUAL_ADDRESS;

#define EFIAPI __attribute__((ms_abi))
#define OFF(t, f) __builtin_offsetof(t, f)

/* --- status codes (UEFI 2.10 appendix D) ---------------------------------
 * The high bit marks an error; that is why a bare `if (st)` is a correct test
 * for "did not succeed" but says nothing about severity. */
#define EFI_SUCCESS             0ULL
#define EFI_ERR(n)              (0x8000000000000000ULL | (n))
#define EFI_LOAD_ERROR          EFI_ERR(1)
#define EFI_INVALID_PARAMETER   EFI_ERR(2)
#define EFI_UNSUPPORTED         EFI_ERR(3)
#define EFI_BUFFER_TOO_SMALL    EFI_ERR(5)
#define EFI_NOT_FOUND           EFI_ERR(14)

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

/* --- EFI_TABLE_HEADER (sec 4.2) ----------------------------------------- */
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;
_Static_assert(sizeof(EFI_TABLE_HEADER) == 24, "EFI_TABLE_HEADER is 24 bytes");

/* Table signatures (sec 4.3, 4.4). A _Static_assert only checks that this file
 * agrees with itself about what was transcribed; these are the RUNTIME check
 * that the transcription describes the object the firmware actually handed us.
 * They cost two comparisons and turn "systab->BootServices is at the wrong
 * offset" from a jump into garbage into a printed refusal.
 *   'IBI SYST' little-endian  = 0x5453595320494249
 *   'BOOTSERV' little-endian  = 0x56524553544f4f42 */
#define EFI_SYSTEM_TABLE_SIGNATURE  0x5453595320494249ULL
#define EFI_BOOT_SERVICES_SIGNATURE 0x56524553544f4f42ULL

/* --- Simple Text Output Protocol (sec 12.4) ------------------------------
 * Used only until ExitBootServices; after that ConOut is a dangling pointer
 * into freed firmware code and calling it is undefined. Serial takes over. */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                      CHAR16 *String);
    void *TestString;
    void *QueryMode;
    void *SetMode;
    void *SetAttribute;
    EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    /* SetCursorPosition, EnableCursor, Mode follow; unused. */
};
_Static_assert(OFF(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL, OutputString) == 8, "sec 12.4");
_Static_assert(OFF(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL, ClearScreen) == 48, "sec 12.4");

/* --- memory services (sec 7.2) ------------------------------------------ */
typedef enum {
    AllocateAnyPages   = 0,
    AllocateMaxAddress = 1,
    AllocateAddress    = 2,
} EFI_ALLOCATE_TYPE;

typedef enum {
    EfiReservedMemoryType      = 0,
    EfiLoaderCode              = 1,
    EfiLoaderData              = 2,
    EfiBootServicesCode        = 3,
    EfiBootServicesData        = 4,
    EfiRuntimeServicesCode     = 5,
    EfiRuntimeServicesData     = 6,
    EfiConventionalMemory      = 7,
    EfiUnusableMemory          = 8,
    EfiACPIReclaimMemory       = 9,
    EfiACPIMemoryNVS           = 10,
    EfiMemoryMappedIO          = 11,
    EfiMemoryMappedIOPortSpace = 12,
    EfiPalCode                 = 13,
    EfiPersistentMemory        = 14,
} EFI_MEMORY_TYPE;

/* EFI_MEMORY_DESCRIPTOR (sec 7.2, GetMemoryMap). NOTE the Pad: PhysicalStart
 * is 8-byte aligned, so a UINT32 Type is followed by 4 bytes of padding that
 * the spec names explicitly. NOTE ALSO that this struct's size is NOT how you
 * step through the map -- GetMemoryMap returns a DescriptorSize which the
 * firmware is allowed to make larger than this (the spec says so in as many
 * words, precisely so it can be extended). Every walk below uses that value. */
typedef struct {
    UINT32               Type;
    UINT32               Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64               NumberOfPages;
    UINT64               Attribute;
} EFI_MEMORY_DESCRIPTOR;
_Static_assert(OFF(EFI_MEMORY_DESCRIPTOR, PhysicalStart) == 8,  "sec 7.2");
_Static_assert(OFF(EFI_MEMORY_DESCRIPTOR, NumberOfPages) == 24, "sec 7.2");
_Static_assert(sizeof(EFI_MEMORY_DESCRIPTOR) == 40, "sec 7.2");

/* --- EFI_BOOT_SERVICES (sec 4.4) ----------------------------------------
 * The whole table is spelled out to the last member this loader uses, because
 * the offsets ARE the ABI. Members between the ones we call are void* on
 * purpose: naming a signature we never invoke would be an untested claim. */
typedef struct {
    EFI_TABLE_HEADER Hdr;                                   /*   0 */
    void *RaiseTPL;                                         /*  24 */
    void *RestoreTPL;                                       /*  32 */
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE Type,
                                       EFI_MEMORY_TYPE MemoryType,
                                       UINTN Pages,
                                       EFI_PHYSICAL_ADDRESS *Memory); /* 40 */
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory,
                                   UINTN Pages);            /*  48 */
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize,
                                      EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                      UINTN *MapKey,
                                      UINTN *DescriptorSize,
                                      UINT32 *DescriptorVersion); /* 56 */
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size,
                                      void **Buffer);       /*  64 */
    EFI_STATUS (EFIAPI *FreePool)(void *Buffer);            /*  72 */
    /* Event & Timer Services. SignalEvent sits between WaitForEvent and
     * CloseEvent and is easy to leave out -- this table did, in an earlier
     * draft of this file, and the result is the exact failure the header
     * comment above describes: every member from HandleProtocol onwards was
     * 8 bytes early, HandleProtocol() called UninstallProtocolInterface(),
     * and the _Static_asserts agreed with the mistake because they had been
     * copied from the same wrong count instead of from the spec. The offsets
     * in the comments below are cumulative from Hdr and were re-derived by
     * counting the spec's member list, not by trusting the previous line. */
    void *CreateEvent;                                      /*  80 */
    void *SetTimer;                                         /*  88 */
    void *WaitForEvent;                                     /*  96 */
    void *SignalEvent;                                      /* 104 */
    void *CloseEvent;                                       /* 112 */
    void *CheckEvent;                                       /* 120 */
    void *InstallProtocolInterface;                         /* 128 */
    void *ReinstallProtocolInterface;                       /* 136 */
    void *UninstallProtocolInterface;                       /* 144 */
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                                        void **Interface);  /* 152 */
    void *Reserved;                                         /* 160 */
    void *RegisterProtocolNotify;                           /* 168 */
    void *LocateHandle;                                     /* 176 */
    void *LocateDevicePath;                                 /* 184 */
    void *InstallConfigurationTable;                        /* 192 */
    void *LoadImage;                                        /* 200 */
    void *StartImage;                                       /* 208 */
    void *Exit;                                             /* 216 */
    void *UnloadImage;                                      /* 224 */
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle,
                                          UINTN MapKey);    /* 232 */
    void *GetNextMonotonicCount;                            /* 240 */
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);         /* 248 */
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode,
                                          UINTN DataSize, CHAR16 *WatchdogData);
                                                            /* 256 */
    void *ConnectController;                                /* 264 */
    void *DisconnectController;                             /* 272 */
    void *OpenProtocol;                                     /* 280 */
    void *CloseProtocol;                                    /* 288 */
    void *OpenProtocolInformation;                          /* 296 */
    void *ProtocolsPerHandle;                               /* 304 */
    void *LocateHandleBuffer;                               /* 312 */
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration,
                                        void **Interface);  /* 320 */
} EFI_BOOT_SERVICES;
_Static_assert(OFF(EFI_BOOT_SERVICES, AllocatePages)    ==  40, "sec 4.4");
_Static_assert(OFF(EFI_BOOT_SERVICES, FreePages)        ==  48, "sec 4.4");
_Static_assert(OFF(EFI_BOOT_SERVICES, GetMemoryMap)     ==  56, "sec 4.4");
_Static_assert(OFF(EFI_BOOT_SERVICES, AllocatePool)     ==  64, "sec 4.4");
_Static_assert(OFF(EFI_BOOT_SERVICES, FreePool)         ==  72, "sec 4.4");
_Static_assert(OFF(EFI_BOOT_SERVICES, HandleProtocol)   == 152, "sec 4.4");
_Static_assert(OFF(EFI_BOOT_SERVICES, ExitBootServices) == 232, "sec 4.4");
_Static_assert(OFF(EFI_BOOT_SERVICES, Stall)            == 248, "sec 4.4");
_Static_assert(OFF(EFI_BOOT_SERVICES, SetWatchdogTimer) == 256, "sec 4.4");
_Static_assert(OFF(EFI_BOOT_SERVICES, LocateProtocol)   == 320, "sec 4.4");

/* --- EFI_CONFIGURATION_TABLE (sec 4.6) ---------------------------------- */
typedef struct {
    EFI_GUID VendorGuid;
    void    *VendorTable;
} EFI_CONFIGURATION_TABLE;
_Static_assert(sizeof(EFI_CONFIGURATION_TABLE) == 24, "sec 4.6");

/* --- EFI_SYSTEM_TABLE (sec 4.3) ----------------------------------------- */
typedef struct {
    EFI_TABLE_HEADER Hdr;                          /*   0 */
    CHAR16 *FirmwareVendor;                        /*  24 */
    UINT32  FirmwareRevision;                      /*  32 */
    UINT32  Pad0;                                  /*  36 (alignment) */
    EFI_HANDLE ConsoleInHandle;                    /*  40 */
    void   *ConIn;                                 /*  48 */
    EFI_HANDLE ConsoleOutHandle;                   /*  56 */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;       /*  64 */
    EFI_HANDLE StandardErrorHandle;                /*  72 */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;       /*  80 */
    void   *RuntimeServices;                       /*  88 */
    EFI_BOOT_SERVICES *BootServices;               /*  96 */
    UINTN   NumberOfTableEntries;                  /* 104 */
    EFI_CONFIGURATION_TABLE *ConfigurationTable;   /* 112 */
} EFI_SYSTEM_TABLE;
_Static_assert(OFF(EFI_SYSTEM_TABLE, ConOut)                  ==  64, "sec 4.3");
_Static_assert(OFF(EFI_SYSTEM_TABLE, BootServices)            ==  96, "sec 4.3");
_Static_assert(OFF(EFI_SYSTEM_TABLE, NumberOfTableEntries)    == 104, "sec 4.3");
_Static_assert(OFF(EFI_SYSTEM_TABLE, ConfigurationTable)      == 112, "sec 4.3");

/* --- EFI_LOADED_IMAGE_PROTOCOL (sec 9.1) --------------------------------
 * The road from "I am running" to "here is the volume I was loaded from":
 * DeviceHandle is the handle of that volume, and SimpleFileSystem hangs off
 * it. That is how the loader finds the kernel without knowing anything about
 * the machine's disks. */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
typedef struct {
    UINT32      Revision;        /*  0 */
    EFI_HANDLE  ParentHandle;    /*  8 */
    void       *SystemTable;     /* 16 */
    EFI_HANDLE  DeviceHandle;    /* 24 */
    void       *FilePath;        /* 32 */
    void       *Reserved;        /* 40 */
    UINT32      LoadOptionsSize; /* 48 */
    void       *LoadOptions;     /* 56 */
    void       *ImageBase;       /* 64 */
    UINT64      ImageSize;       /* 72 */
} EFI_LOADED_IMAGE_PROTOCOL;
_Static_assert(OFF(EFI_LOADED_IMAGE_PROTOCOL, DeviceHandle) == 24, "sec 9.1");
_Static_assert(OFF(EFI_LOADED_IMAGE_PROTOCOL, ImageBase)    == 64, "sec 9.1");

/* --- Simple File System / File protocols (sec 13.4, 13.5) ---------------- */
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
    UINT64 Revision;                                          /*  0 */
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *This,
                              EFI_FILE_PROTOCOL **NewHandle,
                              CHAR16 *FileName,
                              UINT64 OpenMode, UINT64 Attributes); /* 8 */
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *This);      /* 16 */
    void *Delete;                                             /* 24 */
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize,
                              void *Buffer);                  /* 32 */
    void *Write;                                              /* 40 */
    void *GetPosition;                                        /* 48 */
    EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *This,
                                     UINT64 Position);        /* 56 */
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
                                 UINTN *BufferSize, void *Buffer); /* 64 */
    void *SetInfo;                                            /* 72 */
    void *Flush;                                              /* 80 */
};
_Static_assert(OFF(EFI_FILE_PROTOCOL, Open)        ==  8, "sec 13.5");
_Static_assert(OFF(EFI_FILE_PROTOCOL, Read)        == 32, "sec 13.5");
_Static_assert(OFF(EFI_FILE_PROTOCOL, SetPosition) == 56, "sec 13.5");
_Static_assert(OFF(EFI_FILE_PROTOCOL, GetInfo)     == 64, "sec 13.5");

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                    EFI_FILE_PROTOCOL **Root);
};
_Static_assert(OFF(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL, OpenVolume) == 8, "sec 13.4");

/* EFI_FILE_INFO (sec 13.5.16). Only FileSize is read -- the kernel image size
 * decides how many pages to demand at 1 MiB, and demanding them in one call is
 * what makes a refusal a refusal instead of a half-loaded kernel. */
#define EFI_FILE_INFO_ID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
typedef struct {
    UINT64 Size;           /*  0 */
    UINT64 FileSize;       /*  8 */
    UINT64 PhysicalSize;   /* 16 */
    UINT8  CreateTime[16];       /* EFI_TIME, sec 8.3 -- not read */
    UINT8  LastAccessTime[16];
    UINT8  ModificationTime[16];
    UINT64 Attribute;      /* 72 */
    CHAR16 FileName[1];    /* 80, variable length */
} EFI_FILE_INFO;
_Static_assert(OFF(EFI_FILE_INFO, FileSize) ==  8, "sec 13.5.16");
_Static_assert(OFF(EFI_FILE_INFO, FileName) == 80, "sec 13.5.16");

/* --- Graphics Output Protocol (sec 12.9) --------------------------------- */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor = 1,
    PixelBitMask                          = 2,
    PixelBltOnly                          = 3,
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32 Version;                    /*  0 */
    UINT32 HorizontalResolution;       /*  4 */
    UINT32 VerticalResolution;         /*  8 */
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat; /* 12 */
    EFI_PIXEL_BITMASK PixelInformation;    /* 16 */
    UINT32 PixelsPerScanLine;          /* 32 */
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;
_Static_assert(OFF(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION, PixelFormat)       == 12, "sec 12.9");
_Static_assert(OFF(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION, PixelsPerScanLine) == 32, "sec 12.9");

typedef struct {
    UINT32 MaxMode;                                /*  0 */
    UINT32 Mode;                                   /*  4 */
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;    /*  8 */
    UINTN  SizeOfInfo;                             /* 16 */
    EFI_PHYSICAL_ADDRESS FrameBufferBase;          /* 24 */
    UINTN  FrameBufferSize;                        /* 32 */
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;
_Static_assert(OFF(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE, Info)            ==  8, "sec 12.9");
_Static_assert(OFF(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE, FrameBufferBase) == 24, "sec 12.9");

typedef struct {
    void *QueryMode;                               /*  0 */
    void *SetMode;                                 /*  8 */
    void *Blt;                                     /* 16 */
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;       /* 24 */
} EFI_GRAPHICS_OUTPUT_PROTOCOL;
_Static_assert(OFF(EFI_GRAPHICS_OUTPUT_PROTOCOL, Mode) == 24, "sec 12.9");

/* --- ACPI table GUIDs (sec 4.6.1.1) --------------------------------------
 * ACPI 2.0+ first: its RSDP carries the XSDT (64-bit table pointers), which is
 * what any machine made this century actually populates. The 1.0 GUID is the
 * fallback and yields a 20-byte RSDP with only an RSDT. */
#define EFI_ACPI_20_TABLE_GUID \
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }
#define EFI_ACPI_10_TABLE_GUID \
    { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

#endif /* LOGIT_EFI_H */
