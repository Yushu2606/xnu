/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */


#ifndef _IOKIT_KERNELINTERNAL_H
#define _IOKIT_KERNELINTERNAL_H

#include <sys/cdefs.h>

__BEGIN_DECLS

#include <vm/vm_pageout.h>
#include <mach/memory_object_types.h>
#include <device/device_port.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOKitServer.h>
#include <kern/socd_client.h>

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

typedef kern_return_t (*IOIteratePageableMapsCallback)(vm_map_t map, void * ref);

void IOLibInit(void);
kern_return_t IOIteratePageableMaps(vm_size_t size,
    IOIteratePageableMapsCallback callback, void * ref);
vm_map_t IOPageableMapForAddress(uintptr_t address);

struct IOMemoryDescriptorMapAllocRef {
	vm_map_t          map;
	mach_vm_address_t mapped;
	mach_vm_size_t    size;
	vm_prot_t         prot;
	vm_tag_t          tag;
	IOOptionBits      options;
};

kern_return_t
IOMemoryDescriptorMapAlloc(vm_map_t map, void * ref);


mach_vm_address_t
IOKernelAllocateWithPhysicalRestrict(
	kalloc_heap_t       kheap,
	mach_vm_size_t      size,
	mach_vm_address_t   maxPhys,
	mach_vm_size_t      alignment,
	bool                contiguous);
void
IOKernelFreePhysical(
	kalloc_heap_t       kheap,
	mach_vm_address_t   address,
	mach_vm_size_t      size);

#if IOTRACKING
IOReturn
IOMemoryMapTracking(IOTrackingUser * tracking, task_t * task,
    mach_vm_address_t * address, mach_vm_size_t * size);
#endif /* IOTRACKING */

extern vm_size_t debug_iomallocpageable_size;

extern ppnum_t gIOLastPage;

extern IOSimpleLock * gIOPageAllocLock;
extern queue_head_t   gIOPageAllocList;

/* Physical to physical copy (ints must be disabled) */
extern void bcopy_phys(addr64_t from, addr64_t to, vm_size_t size);
#if defined (__arm64__)
extern void bcopy_phys_with_options(addr64_t from, addr64_t to, vm_size_t nbytes, int options);
#endif /* __arm64__ */

__END_DECLS

#define __IODEQUALIFY(type, expr)                               \
   ({ typeof(expr) expr_ = (type)(uintptr_t)(expr);             \
       (type)(uintptr_t)(expr_); })

struct IODMACommandMapSegment {
	uint64_t fDMAOffset;       // The offset of this segment in DMA
	uint64_t fMapOffset;       // Offset of segment in mapping
	uint64_t fPageOffset;      // Offset within first page of segment
};

struct IODMACommandInternal {
	IOMDDMAWalkSegmentState      fState;
	IOMDDMACharacteristics       fMDSummary;

	UInt64 fPreparedOffset;
	UInt64 fPreparedLength;

	UInt32 fSourceAlignMask;

	UInt8  fCursor;
	UInt8  fCheckAddressing;
	UInt8  fIterateOnly;
	UInt8  fMisaligned;
	UInt8  fPrepared;
	UInt8  fDoubleBuffer;
	UInt8  fNewMD;
	UInt8  fLocalMapperAllocValid;
	UInt8  fIOVMAddrValid;
	UInt8  fForceDoubleBuffer;
	UInt8  fSetActiveNoMapper;

	vm_page_t fCopyPageAlloc;
	vm_page_t fCopyNext;
	vm_page_t fNextRemapPage;

	ppnum_t  fCopyPageCount;

	uint64_t  fLocalMapperAlloc;
	uint64_t  fLocalMapperAllocLength;

	OSPtr<IOBufferMemoryDescriptor> fCopyMD;

	IOService * fDevice;
	IOLock * fDextLock;

	// IODMAEventSource use
	IOReturn fStatus;
	UInt64   fActualByteCount;
	AbsoluteTime    fTimeStamp;

	// Multisegment vars
	IODMACommandMapSegment * fMapSegments;
	uint32_t                 fMapSegmentsCount;
	uint64_t fLocalMapperAllocBase;
	uint64_t fOffset2Index;
	uint64_t fNextOffset;
	uint64_t fIndex;
};

struct IOMemoryDescriptorDevicePager {
	void *                       devicePager;
	unsigned int             pagerContig:1;
	unsigned int             unused:31;
	IOMemoryDescriptor * memory;
};

struct IOMemoryDescriptorReserved {
	IOMemoryDescriptorDevicePager dp;
	uint64_t                      descriptorID;
	uint64_t                      preparationID;
	// for kernel IOMD subclasses... they have no expansion
	uint64_t                      kernReserved[4];
	vm_tag_t                      kernelTag;
	vm_tag_t                      userTag;
	task_t                        creator;
	OSObject                    * contextObject;
};

#if defined(__x86_64__)
struct iopa_t {
	IOLock       * lock;
	queue_head_t   list;
	vm_size_t      pagecount;
	vm_size_t      bytecount;
};

struct iopa_page_t {
	queue_chain_t link;
	uint64_t      avail;
	uint32_t      signature;
};
typedef struct iopa_page_t iopa_page_t;

typedef uintptr_t (*iopa_proc_t)(kalloc_heap_t kheap, iopa_t * a);

enum{
	kIOPageAllocSignature  = 'iopa'
};

extern "C" void      iopa_init(iopa_t * a);
extern "C" uintptr_t iopa_alloc(iopa_t * a, iopa_proc_t alloc, kalloc_heap_t kheap,
    vm_size_t bytes, vm_size_t balign);
extern "C" uintptr_t iopa_free(iopa_t * a, uintptr_t addr, vm_size_t bytes);
extern "C" uint32_t  gIOPageAllocChunkBytes;

extern "C" iopa_t    gIOBMDPageAllocator;
#endif /* defined(__x86_64__) */


extern "C" struct timeval gIOLastSleepTime;
extern "C" struct timeval gIOLastWakeTime;

extern clock_sec_t gIOConsoleLockTime;

extern bool gCPUsRunning;

extern OSSet * gIORemoveOnReadProperties;

extern uint32_t gHaltTimeMaxLog;
extern uint32_t gHaltTimeMaxPanic;

extern "C" void IOKitInitializeTime( void );
extern void IOMachPortInitialize(void);

extern "C" OSString * IOCopyLogNameForPID(int pid);

extern "C" void IOKitKernelLogBuffer(const char * title, const void * buffer, size_t size,
    void (*output)(const char *format, ...));

#if defined(__i386__) || defined(__x86_64__)
#ifndef __cplusplus
#error xx
#endif

extern const OSSymbol * gIOCreateEFIDevicePathSymbol;
extern "C" void IOSetKeyStoreData(LIBKERN_CONSUMED IOMemoryDescriptor * data);
extern "C" void IOSetAPFSKeyStoreData(LIBKERN_CONSUMED IOMemoryDescriptor* data);
#endif
extern const  OSSymbol * gAKSGetKey;

void IOScreenLockTimeUpdate(clock_sec_t secs);

void     IOCPUInitialize(void);
IOReturn IOInstallServicePlatformActions(IOService * service);
IOReturn IOInstallServiceSleepPlatformActions(IOService * service);
IOReturn IORemoveServicePlatformActions(IOService * service);
void     IOCPUSleepKernel(void);
void     IOPlatformActionsInitialize(void);

class IOSystemStateNotification : public IOService
{
	OSDeclareDefaultStructors(IOSystemStateNotification);
public:
	static IOService * initialize(void);
	virtual IOReturn setProperties( OSObject * properties) APPLE_KEXT_OVERRIDE;
	virtual bool serializeProperties(OSSerialize * serialize) const APPLE_KEXT_OVERRIDE;
};

#endif /* ! _IOKIT_KERNELINTERNAL_H */
