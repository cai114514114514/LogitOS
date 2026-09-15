/* SPDX-License-Identifier: MIT
 * Authoritative private-disk geometry. The Python runner reads these constants
 * from this header; a copied partition offset would test the wrong sectors. */
#define USB_TEST_MAGIC "LOGIT_USB_MSC_PRIVATE_TEST_v1"
#define USB_TEST_SECTORS 65536
#define USB_TEST_PART_START 2048
#define USB_TEST_PART_SECTORS 8192
#define USB_TEST_DATA_START 8
#define USB_TEST_DATA_SECTORS 130
#define USB_TEST_PHASE_OFFSET 64
#define USB_TEST_ID_OFFSET 65
#define USB_TEST_GUARD_PRE 167
#define USB_TEST_GUARD_POST 92
