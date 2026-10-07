#ifndef FTP_TEST_LWIPOPTS_H
#define FTP_TEST_LWIPOPTS_H
#include "../../examples/ethernet-ftp/lwipopts.h"
/* Native pointers require 8-byte alignment; firmware is fixed at 4. All
 * capacities/protocol options are otherwise the actual target configuration. */
#undef MEM_ALIGNMENT
#define MEM_ALIGNMENT 8
#endif
