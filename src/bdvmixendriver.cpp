// Copyright (c) 2015 Bitdefender SRL, All rights reserved.
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 3.0 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library.

#include "bdvmi/xendriver.h"
#include "bdvmi/exception.h"
#include "bdvmi/loghelper.h"
#include "bdvmi/xeninlines.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sys/mman.h>
#include <sys/resource.h>
#include <cpuid.h>

extern "C" {
#include <xen/xen-compat.h>
#if ( __XEN_LATEST_INTERFACE_VERSION__ >= 0x00040300 )
#include <xenstore.h>
#else
#error unsupported Xen version
#endif
#define private rprivate /* private is a C++ keyword */
#if __XEN_LATEST_INTERFACE_VERSION__ == 0x00040600
#include <xen/vm_event.h>
#else
#include <xen/mem_event.h>
#endif
#undef private
}

//#define DISABLE_PAGE_CACHE

#define MTRR_PHYSMASK_VALID_BIT 11
#define MTRR_PHYSMASK_SHIFT 12
#define MTRR_PHYSBASE_TYPE_MASK 0xff /* lowest 8 bits */

#define X86_CR0_PE 0x00000001    /* Enable Protected Mode    (RW) */
#define X86_EFLAGS_VM 0x00020000 /* Virtual Mode */
#define _EFER_LMA 10             /* Long mode active (read-only) */
#define EFER_LMA ( 1 << _EFER_LMA )

#define CS_AR_BYTES_L ( 1 << 9 )
#define CS_AR_BYTES_D ( 1 << 10 )

#define TRAP_page_fault 14
#define X86_EVENTTYPE_HW_EXCEPTION 3 /* hardware exception */

#define paddr_to_pfn( pa ) ( ( unsigned long )( ( pa ) >> XC_PAGE_SHIFT ) )

namespace bdvmi {

#ifdef DISABLE_PAGE_CACHE
static bool check_page( void *addr )
{
	unsigned char vec[1] = {};

	// The page is not present or otherwise unavailable
	if ( mincore( addr, XC_PAGE_SIZE, vec ) < 0 || !( vec[0] & 0x01 ) )
		return false;

	return true;
}
#endif

XenDriver::XenDriver( domid_t domain, LogHelper *logHelper, bool hvmOnly )
    : xsh_( NULL ), domain_( domain ), pageCache_( logHelper ), guestWidth_( 8 ), logHelper_( logHelper )
{
	init( domain, hvmOnly );
}

XenDriver::XenDriver( const std::string &domainName, LogHelper *logHelper, bool hvmOnly )
    : xsh_( NULL ), pageCache_( logHelper ), guestWidth_( 8 ), logHelper_( logHelper )
{
	domain_ = getDomainId( domainName );
	init( domain_, hvmOnly );
}

XenDriver::~XenDriver()
{
	cleanup();
}

#define hvm_long_mode_enabled( regs ) ( regs.msr_efer & EFER_LMA )

int32_t XenDriver::guestX86Mode( const Registers &regs )
{
	if ( !( regs.cr0 & X86_CR0_PE ) )
		return 0;

	if ( regs.rflags & X86_EFLAGS_VM )
		return 1;

	if ( hvm_long_mode_enabled( regs ) && ( regs.cs_arbytes & CS_AR_BYTES_L ) )
		return 8;

	return ( ( regs.cs_arbytes & CS_AR_BYTES_D ) ? 4 : 2 );
}

bool XenDriver::cpuCount( unsigned int &count ) const throw()
{
	xc_dominfo_t info;

	if ( xc_domain_getinfo( xci_, domain_, 1, &info ) != 1 ) {

		if ( logHelper_ )
			logHelper_->error( std::string( "xc_domain_getinfo() failed: " ) + strerror( errno ) );

		return false;
	}

	count = info.max_vcpu_id + 1;
	// count = info.nr_online_vcpus;

	return true;
}

bool XenDriver::tscSpeed( unsigned long long &speed ) const throw()
{
	uint64_t elapsed_nsec;
	uint32_t tsc_mode, gtsc_khz, incarnation;

	if ( xc_domain_get_tsc_info( xci_, domain_, &tsc_mode, &elapsed_nsec, &gtsc_khz, &incarnation ) != 0 ) {
		if ( logHelper_ )
			logHelper_->error( std::string( "xc_domain_get_tsc_info() failed: " ) + strerror( errno ) );

		return false;
	}

	// Convert to Hz (ticks / second)
	speed = gtsc_khz * 1000;
	return true;
}

bool XenDriver::mtrrType( unsigned long long guestAddress, uint8_t &type ) const throw()
{
	const uint8_t MTRR_TYPE_UNCACHABLE = 0;
	const uint8_t MTRR_TYPE_WRTHROUGH = 4;

	int32_t seg, index;
	uint8_t overlap_mtrr = 0, overlap_mtrr_pos = 0;

	static bool hwMtrrInit = false;
	static struct hvm_hw_mtrr hwMtrr;

	if ( !hwMtrrInit ) {
		if ( xc_domain_hvm_getcontext_partial( xci_, domain_, HVM_SAVE_CODE( MTRR ), 0, &hwMtrr,
		                                       sizeof( hwMtrr ) ) != 0 ) {

			if ( logHelper_ )
				logHelper_->error( std::string( "xc_domain_hvm_getcontext_partial() failed: " ) +
				                   strerror( errno ) );

			return false;
		}
		else {
			hwMtrrInit = true;
		}
	}

	uint8_t def_type = hwMtrr.msr_mtrr_def_type & 0xff;
	uint8_t enabled = hwMtrr.msr_mtrr_def_type >> 10;
	uint8_t *u8_fixed = ( uint8_t * )hwMtrr.msr_mtrr_fixed;

	if ( !( enabled & 0x2 ) ) {
		type = MTRR_TYPE_UNCACHABLE;
		return true;
	}

	if ( ( guestAddress < 0x100000 ) && ( enabled & 1 ) ) {

		/* Fixed range MTRR takes effective */
		int32_t addr = ( uint32_t )guestAddress;

		if ( addr < 0x80000 ) {
			seg = ( addr >> 16 );
			return u8_fixed[seg];
		}
		else if ( addr < 0xc0000 ) {
			seg = ( addr - 0x80000 ) >> 14;
			index = ( seg >> 3 ) + 1;
			seg &= 7; /* select 0-7 segments */
			return u8_fixed[index * 8 + seg];
		}
		else {
			/* 0xC0000 --- 0x100000 */
			seg = ( addr - 0xc0000 ) >> 12;
			index = ( seg >> 3 ) + 3;
			seg &= 7; /* select 0-7 segments */
			return u8_fixed[index * 8 + seg];
		}
	}

	uint8_t num_var_ranges = hwMtrr.msr_mtrr_cap & 0xff;
	bool overlapped = isVarMtrrOverlapped( hwMtrr );

	for ( seg = 0; seg < num_var_ranges; ++seg ) {
		uint64_t phys_base = hwMtrr.msr_mtrr_var[seg * 2];
		uint64_t phys_mask = hwMtrr.msr_mtrr_var[seg * 2 + 1];

		if ( phys_mask & ( 1 << MTRR_PHYSMASK_VALID_BIT ) ) {
			if ( ( ( uint64_t )guestAddress & phys_mask ) >> MTRR_PHYSMASK_SHIFT ==
			     ( phys_base & phys_mask ) >> MTRR_PHYSMASK_SHIFT ) {

				if ( overlapped ) {
					overlap_mtrr |= 1 << ( phys_base & MTRR_PHYSBASE_TYPE_MASK );
					overlap_mtrr_pos = phys_base & MTRR_PHYSBASE_TYPE_MASK;
				}
				else {
					return phys_base & MTRR_PHYSBASE_TYPE_MASK;
				}
			}
		}
	}

	if ( overlap_mtrr == 0 ) {
		type = def_type;
		return true;
	}

	if ( !( overlap_mtrr & ~( ( ( uint8_t )1 ) << overlap_mtrr_pos ) ) ) {
		type = overlap_mtrr_pos;
		return true;
	}

	if ( overlap_mtrr & 0x1 ) {
		/* Two or more match, one is UC. */
		type = MTRR_TYPE_UNCACHABLE;
		return true;
	}

	if ( !( overlap_mtrr & 0xaf ) ) {
		/* Two or more match, WT and WB. */
		type = MTRR_TYPE_WRTHROUGH;
		return true;
	}

	/* Behaviour is undefined, but return the last overlapped type. */
	type = overlap_mtrr_pos;
	return true;
}

namespace {

#if ( __XEN_LATEST_INTERFACE_VERSION__ >= 0x00040500 )

typedef xenmem_access_t access_t;

const access_t access_n = XENMEM_access_n;
const access_t access_r = XENMEM_access_r;
const access_t access_w = XENMEM_access_w;
const access_t access_x = XENMEM_access_x;
const access_t access_rw = XENMEM_access_rw;
const access_t access_rx = XENMEM_access_rx;
const access_t access_wx = XENMEM_access_wx;
const access_t access_rwx = XENMEM_access_rwx;
const access_t access_rx2rw = XENMEM_access_rx2rw;

#define set_mem_access xc_set_mem_access
#define get_mem_access xc_get_mem_access

#else

typedef hvmmem_access_t access_t;

const access_t access_n = HVMMEM_access_n;
const access_t access_r = HVMMEM_access_r;
const access_t access_w = HVMMEM_access_w;
const access_t access_x = HVMMEM_access_x;
const access_t access_rw = HVMMEM_access_rw;
const access_t access_rx = HVMMEM_access_rx;
const access_t access_wx = HVMMEM_access_wx;
const access_t access_rwx = HVMMEM_access_rwx;
const access_t access_rx2rw = HVMMEM_access_rx2rw;

#define set_mem_access xc_hvm_set_mem_access
#define get_mem_access xc_hvm_get_mem_access

#endif
}

bool XenDriver::setPageProtection( unsigned long long guestAddress, bool read, bool write, bool execute ) throw()
{
	access_t memaccess = access_n;

	if ( read && !write && !execute )
		memaccess = access_r;

	else if ( !read && write && !execute )
		memaccess = access_w;

	else if ( !read && !write && execute )
		memaccess = access_x;

	else if ( read && write && !execute )
		memaccess = access_rw;

	else if ( read && !write && execute )
		memaccess = access_rx;

	else if ( !read && write && execute )
		memaccess = access_wx;

	else if ( read && write && execute )
		memaccess = access_rwx;

	unsigned long gfn = paddr_to_pfn( guestAddress );

	if ( set_mem_access( xci_, domain_, memaccess, gfn, 1 ) ) {

		if ( logHelper_ )
			logHelper_->error( std::string( "xc_hvm_set_mem_access() failed: " ) + strerror( errno ) );

		return false;
	}

	return true;
}

bool XenDriver::getPageProtection( unsigned long long guestAddress, bool &read, bool &write, bool &execute ) const
        throw()
{
	access_t memaccess;
	unsigned long gfn = paddr_to_pfn( guestAddress );

	if ( get_mem_access( xci_, domain_, gfn, &memaccess ) ) {

		if ( logHelper_ )
			logHelper_->error( std::string( "xc_hvm_get_mem_access() failed: " ) + strerror( errno ) );

		return false;
	}

	read = write = execute = false;

	switch ( memaccess ) {
		case access_r:
			read = true;
			break;

		case access_w:
			write = true;
			break;

		case access_rw:
			read = write = true;
			break;

		case access_x:
			execute = true;
			break;

		case access_rx:
			read = execute = true;
			break;

		case access_wx:
			write = execute = true;
			break;

		case access_rwx:
			read = write = execute = true;
			break;

		case access_rx2rw:             /* Page starts off as r-x, but automatically */
			read = execute = true; /* change to r-w on a write. */
			break;

		case access_n:
		default:
			break;
	}

	return true;
}

bool XenDriver::registers( unsigned short vcpu, Registers &regs ) const throw()
{
	struct hvm_hw_cpu hwCpu;

	if ( xc_domain_hvm_getcontext_partial( xci_, domain_, HVM_SAVE_CODE( CPU ), vcpu, &hwCpu, sizeof( hwCpu ) ) !=
	     0 ) {
		if ( logHelper_ )
			logHelper_->error( std::string( "xc_domain_hvm_getcontext_partial() failed: " ) +
			                   strerror( errno ) );

		return false;
	}

	regs.sysenter_cs = hwCpu.sysenter_cs;
	regs.sysenter_esp = hwCpu.sysenter_esp;
	regs.sysenter_eip = hwCpu.sysenter_eip;
	regs.msr_efer = hwCpu.msr_efer;
	regs.msr_star = hwCpu.msr_star;
	regs.msr_lstar = hwCpu.msr_lstar;
	regs.fs_base = hwCpu.fs_base;
	regs.gs_base = hwCpu.gs_base;
	regs.idtr_base = hwCpu.idtr_base;
	regs.idtr_limit = hwCpu.idtr_limit;
	regs.gdtr_base = hwCpu.gdtr_base;
	regs.gdtr_limit = hwCpu.gdtr_limit;
	regs.rflags = hwCpu.rflags;
	regs.rax = hwCpu.rax;
	regs.rcx = hwCpu.rcx;
	regs.rdx = hwCpu.rdx;
	regs.rbx = hwCpu.rbx;
	regs.rsp = hwCpu.rsp;
	regs.rbp = hwCpu.rbp;
	regs.rsi = hwCpu.rsi;
	regs.rdi = hwCpu.rdi;
	regs.r8 = hwCpu.r8;
	regs.r9 = hwCpu.r9;
	regs.r10 = hwCpu.r10;
	regs.r11 = hwCpu.r11;
	regs.r12 = hwCpu.r12;
	regs.r13 = hwCpu.r13;
	regs.r14 = hwCpu.r14;
	regs.r15 = hwCpu.r15;
	regs.rip = hwCpu.rip;
	regs.dr7 = hwCpu.dr7;
	regs.cr0 = hwCpu.cr0;
	regs.cr2 = hwCpu.cr2;
	regs.cr3 = hwCpu.cr3;
	regs.cr4 = hwCpu.cr4;
	regs.cr8 = 0; // can't get this with Xen / userspace
	regs.cs_arbytes = hwCpu.cs_arbytes;

#if ( __XEN_LATEST_INTERFACE_VERSION__ >= 0x00040400 )
	int32_t x86Mode = guestX86Mode( regs );
#else
	int32_t x86Mode = hwCpu.guest_x86_mode;
#endif

	switch ( x86Mode ) {
		case 2:
			regs.guest_x86_mode = Registers::CS_TYPE_16;
			break;
		case 4:
			regs.guest_x86_mode = Registers::CS_TYPE_32;
			break;
		case 8:
			regs.guest_x86_mode = Registers::CS_TYPE_64;
			break;
		default:
			regs.guest_x86_mode = Registers::ERROR;
			break;
	}

	return true;
}

bool XenDriver::mtrrs( unsigned short vcpu, Mtrrs &m ) const throw()
{
	struct hvm_hw_mtrr hwMtrr;

	if ( xc_domain_hvm_getcontext_partial( xci_, domain_, HVM_SAVE_CODE( MTRR ), vcpu, &hwMtrr,
	                                       sizeof( hwMtrr ) ) != 0 ) {
		if ( logHelper_ )
			logHelper_->error( std::string( "xc_domain_hvm_getcontext_partial() failed: " ) +
			                   strerror( errno ) );

		return false;
	}

	m.pat = hwMtrr.msr_pat_cr;
	m.cap = hwMtrr.msr_mtrr_cap;
	m.def_type = hwMtrr.msr_mtrr_def_type;

	return true;
}

bool XenDriver::setRegisters( unsigned short vcpu, const Registers &regs, bool setEip ) throw()
{
	vcpu_guest_context_any_t ctxt;

	if ( xc_vcpu_getcontext( xci_, domain_, vcpu, &ctxt ) != 0 ) {

		if ( logHelper_ )
			logHelper_->error( std::string( "xc_vcpu_getcontext() failed: " ) + strerror( errno ) );

		return false;
	}

	if ( guestWidth_ == 4 ) {

		ctxt.x32.user_regs.eax = regs.rax;
		ctxt.x32.user_regs.ecx = regs.rcx;
		ctxt.x32.user_regs.edx = regs.rdx;
		ctxt.x32.user_regs.ebx = regs.rbx;
		ctxt.x32.user_regs.esp = regs.rsp;
		ctxt.x32.user_regs.ebp = regs.rbp;
		ctxt.x32.user_regs.esi = regs.rsi;
		ctxt.x32.user_regs.edi = regs.rdi;

		if ( setEip )
			ctxt.x32.user_regs.eip = regs.rip;
	}
	else {

		ctxt.x64.user_regs.rax = regs.rax;
		ctxt.x64.user_regs.rcx = regs.rcx;
		ctxt.x64.user_regs.rdx = regs.rdx;
		ctxt.x64.user_regs.rbx = regs.rbx;
		ctxt.x64.user_regs.rsp = regs.rsp;
		ctxt.x64.user_regs.rbp = regs.rbp;
		ctxt.x64.user_regs.rsi = regs.rsi;
		ctxt.x64.user_regs.rdi = regs.rdi;
		ctxt.x64.user_regs.r8 = regs.r8;
		ctxt.x64.user_regs.r9 = regs.r9;
		ctxt.x64.user_regs.r10 = regs.r10;
		ctxt.x64.user_regs.r11 = regs.r11;
		ctxt.x64.user_regs.r12 = regs.r12;
		ctxt.x64.user_regs.r13 = regs.r13;
		ctxt.x64.user_regs.r14 = regs.r14;
		ctxt.x64.user_regs.r15 = regs.r15;

		if ( setEip )
			ctxt.x64.user_regs.eip = regs.rip;
	}

	if ( xc_vcpu_setcontext( xci_, domain_, vcpu, &ctxt ) == -1 ) {

		if ( logHelper_ )
			logHelper_->error( std::string( "xc_vcpu_setcontext() failed: " ) + strerror( errno ) );

		return false;
	}

	return true;
}

bool XenDriver::writeToPhysAddress( unsigned long long address, void *buffer, size_t /*length*/ ) throw()
{
	unsigned long gfn = paddr_to_pfn( address );
	unsigned long mfn = paddr_to_pfn( ( unsigned long )buffer );

	// Copy the whole page - can't find another way to do it with libxc.
	if ( xc_copy_to_domain_page( xci_, domain_, gfn, ( const char * )mfn ) ) {

		if ( logHelper_ )
			logHelper_->error( std::string( "xc_copy_to_domain_page() failed: " ) + strerror( errno ) );

		return false;
	}

	return true;
}

bool XenDriver::enableMsrExit( unsigned int msr, bool &oldValue ) throw()
{
	oldValue = false;

	try {

		if ( msrs_.find( msr ) != msrs_.end() ) {
			oldValue = true;
			return true;
		}

		msrs_.insert( msr );

	} catch ( ... ) {
		return false;
	}

	return true;
}

bool XenDriver::disableMsrExit( unsigned int msr, bool &oldValue ) throw()
{
	oldValue = false;

	try {
		std::set<unsigned int>::iterator it = msrs_.find( msr );

		if ( it != msrs_.end() ) {
			msrs_.erase( it );
			oldValue = true;
		}

	} catch ( ... ) {
		return false;
	}

	return true;
}

bool XenDriver::shutdown() throw()
{
	if ( xc_domain_shutdown( xci_, domain_, SHUTDOWN_poweroff ) ) {

		if ( logHelper_ )
			logHelper_->error( std::string( "xc_domain_shutdown() failed: " ) + strerror( errno ) );

		return false;
	}

	return true;
}

void XenDriver::init( domid_t domain, bool hvmOnly )
{
	xci_ = xc_interface_open( NULL, NULL, 0 );

	if ( !xci_ ) {
		cleanup();
		throw Exception( "xc_interface_init() failed" );
	}

	xc_dominfo_t info;

	if ( xc_domain_getinfo( xci_, domain, 1, &info ) != 1 ) {
		cleanup();
		throw Exception( "xc_domain_getinfo() failed" );
	}

	std::stringstream ss;

	if ( hvmOnly && !info.hvm ) {
		cleanup();
		ss << "Domain " << domain << " is not a HVM guest";
		throw Exception( ss.str(), Exception::NOT_HVM );
	}

	xen_capabilities_info_t caps;

	if ( xc_version( xci_, XENVER_capabilities, &caps ) != 0 ) {
		cleanup();
		throw Exception( "Could not get Xen capabilities" );
	}

	guestWidth_ = strstr( caps, "x86_64" ) ? 8 : 4;

	if ( !xsh_ ) {
		xsh_ = xs_open( 0 );

		if ( !xsh_ ) {
			cleanup();
			throw Exception( "xs_open() failed" );
		}
	}

	physAddr_ = 36;

	if ( cpuid_eax( 0x80000000 ) >= 0x80000008 )
		physAddr_ = ( uint8_t )cpuid_eax( 0x80000008 );

	pageCache_.init( xci_, domain );

	unsigned int size;

	ss.str( "" );
	ss << "/local/domain/" << domain_ << "/vm";

	char *path = static_cast<char *>( xs_read_timeout( xsh_, XBT_NULL, ss.str().c_str(), &size, 1 ) );

	if ( path && path[0] != '\0' ) {
		ss.str( "" );
		ss << path << "/uuid";

		free( path );
		size = 0;

		path = static_cast<char *>( xs_read_timeout( xsh_, XBT_NULL, ss.str().c_str(), &size, 1 ) );

		if ( path && path[0] != '\0' )
			uuid_ = path;
	}

	free( path );
}

void XenDriver::cleanup()
{
	if ( xci_ ) {
		xc_interface_close( xci_ );
		xci_ = NULL;
	}

	if ( xsh_ ) {
		xs_close( xsh_ );
		xsh_ = NULL;
	}
}

domid_t XenDriver::getDomainId( const std::string &domainName )
{
	domid_t domainId = 0;

	if ( !xsh_ ) {
		xsh_ = xs_open( 0 );

		if ( !xsh_ ) {
			cleanup();
			throw Exception( "xs_open() failed" );
		}
	}

	unsigned int size = 0;
	char **domains = xs_directory( xsh_, XBT_NULL, "/local/domain", &size );

	if ( size == 0 ) {
		cleanup();
		throw Exception( std::string( "Failed to retrieve domain ID by name [" ) + domainName + "]" );
	}

	for ( unsigned int i = 0; i < size; ++i ) {

		std::string tmp = std::string( "/local/domain/" ) + domains[i] + "/name";

		char *nameCandidate = static_cast<char *>( xs_read_timeout( xsh_, XBT_NULL, tmp.c_str(), NULL, 1 ) );

		if ( nameCandidate != NULL && domainName == nameCandidate )
			domainId = atoi( domains[i] );

		free( nameCandidate );
	}

	free( domains );

	return domainId;
}

MapReturnCode XenDriver::mapPhysMemToHost( unsigned long long address, size_t length, uint32_t /*flags*/,
                                           void *&pointer ) throw()
{
	// one-page limit
	if ( ( address & XC_PAGE_MASK ) != ( ( address + length - 1 ) & XC_PAGE_MASK ) )
		return MAP_INVALID_PARAMETER;

	pointer = NULL;
	unsigned long gfn = paddr_to_pfn( address );

	try {

		void *mapped = NULL;

#ifdef DISABLE_PAGE_CACHE
		mapped = xc_map_foreign_range( xci_, domain_, XC_PAGE_SIZE, PROT_READ | PROT_WRITE, gfn );

		/*
		if (!mapped && logHelper_)
		    logHelper_->error(std::string("xc_map_foreign_range() failed: ")
		        + strerror(errno));
		*/

		if ( mapped && !check_page( mapped ) ) {
			munmap( mapped, XC_PAGE_SIZE );
			return MAP_PAGE_NOT_PRESENT;
		}
#else
		MapReturnCode mrc = pageCache_.update( gfn, mapped );

		if ( mrc != MAP_SUCCESS )
			return mrc;
#endif

		if ( !mapped ) {

			if ( logHelper_ ) {
				std::stringstream ss;
				ss << "address: 0x" << std::setfill( '0' ) << std::setw( 16 ) << std::hex << address
				   << ", length: " << length;

				logHelper_->error( ss.str() );
			}

			return MAP_FAILED_GENERIC;
		}

		pointer = static_cast<char *>( mapped ) + ( address & ~XC_PAGE_MASK );

	} catch ( ... ) {
		return MAP_FAILED_GENERIC;
	}

	return MAP_SUCCESS;
}

bool XenDriver::unmapPhysMem( void *hostPtr ) throw()
{
	void *map = hostPtr;
	map = ( void * )( ( long int )map & XC_PAGE_MASK );

#ifdef DISABLE_PAGE_CACHE
	munmap( map, XC_PAGE_SIZE );
#else
	pageCache_.release( map );
#endif

	return true;
}

MapReturnCode XenDriver::mapVirtMemToHost( unsigned long long address, size_t length, uint32_t /* flags */,
                                           unsigned short vcpu, void *&pointer ) throw()
{
	// one-page limit
	if ( ( address & XC_PAGE_MASK ) != ( ( address + length - 1 ) & XC_PAGE_MASK ) )
		return MAP_INVALID_PARAMETER;

	unsigned long gfn;
	pointer = NULL;

	try {
		std::map<unsigned long long, unsigned long>::const_iterator ait = addressCache_.find( address );

		if ( ait != addressCache_.end() ) {
			gfn = ait->second;
		}
		else {
			gfn = xc_translate_foreign_address( xci_, domain_, vcpu, address );

			if ( gfn == 0 ) {

				if ( logHelper_ ) {
					std::stringstream ss;

					ss << "xc_translate_foreign_address(0x" << std::setfill( '0' )
					   << std::setw( 16 ) << std::hex << address << ") (vcpu = " << vcpu
					   << ") failed: " << strerror( errno );

					logHelper_->error( ss.str() );
				}

				return MAP_FAILED_GENERIC;
			}
		}

		void *mapped = NULL;

#ifdef DISABLE_PAGE_CACHE
		mapped = xc_map_foreign_range( xci_, domain_, XC_PAGE_SIZE, PROT_READ | PROT_WRITE, gfn );

		if ( mapped && !check_page( mapped ) ) {
			munmap( mapped, XC_PAGE_SIZE );
			return MAP_PAGE_NOT_PRESENT;
		}
#else
		MapReturnCode mrc = pageCache_.update( gfn, mapped );

		if ( mrc != MAP_SUCCESS )
			return mrc;
#endif

		if ( !mapped ) {

			if ( logHelper_ ) {
				std::stringstream ss;
				ss << "address: 0x" << std::setfill( '0' ) << std::setw( 16 ) << std::hex << address
				   << ", length: " << length;

				logHelper_->error( ss.str() );
			}

			return MAP_FAILED_GENERIC;
		}

		pointer = static_cast<char *>( mapped ) + ( address & ~XC_PAGE_MASK );

	} catch ( ... ) {
		return MAP_FAILED_GENERIC;
	}

	return MAP_SUCCESS;
}

bool XenDriver::unmapVirtMem( void *hostPtr ) throw()
{
	return unmapPhysMem( hostPtr );
}

bool XenDriver::cacheGuestVirtAddr( unsigned long long address ) throw()
{
	unsigned long gfn = xc_translate_foreign_address( xci_, domain_, 0, address );

	if ( gfn == 0 ) {
		if ( logHelper_ )
			logHelper_->error( std::string( "xc_translate_foreign_address() failed: " ) +
			                   strerror( errno ) );

		return false;
	}

	try {
		addressCache_[address] = gfn;

	} catch ( ... ) {
		return false;
	}

	return true;
}

#define PFEC_write_access ( 1U << 1 )

bool XenDriver::requestPageFault( int vcpu, uint64_t addressSpace, uint64_t virtualAddress,
                                  uint32_t writeAccess ) throw()
{
	// Get rid of unused parameters warnings.
	vcpu = vcpu;
	addressSpace = addressSpace;

#if ( __XEN_LATEST_INTERFACE_VERSION__ >= 0x00040400 )

	uint32_t error_code = ( writeAccess ? PFEC_write_access : 0 );

	// It is assumed that the guest is in user-mode and in the proper
	// address space for "vcpu" here - otherwise things will likely
	// explode. If something does explode here, check that those
	// conditions hold HV-side.
	if ( xc_hvm_inject_trap( xci_, domain_, vcpu, TRAP_page_fault, X86_EVENTTYPE_HW_EXCEPTION, error_code, 0,
	                         virtualAddress /*, addressSpace */ ) != 0 ) {
		if ( logHelper_ )
			logHelper_->error( std::string( "xc_hvm_inject_trap() failed: " ) + strerror( errno ) );

		return false;
	}

#else

	xen_domctl_set_pagefault_info_t info;

	info.address_space = addressSpace;
	info.virtual_address = virtualAddress;
	info.write_access = writeAccess;

	if ( xc_domain_set_pagefault_info( xci_, domain_, &info ) != 0 ) {
		if ( logHelper_ )
			logHelper_->error( std::string( "xc_domain_set_pagefault_info() failed: " ) +
			                   strerror( errno ) );

		return false;
	}

#endif

	return true;
}

bool XenDriver::disableRepOptimizations() throw()
{
#if __XEN_LATEST_INTERFACE_VERSION__ == 0x00040400
	if ( xc_domain_set_emulated_reps( xci_, domain_, 0 ) != 0 ) {
		if ( logHelper_ )
			logHelper_->error( std::string( "xc_domain_set_emulated_reps() failed: " ) + strerror( errno ) );

		return false;
	}

	return true;
#else
	return false;
#endif
}

bool XenDriver::pause() throw()
{
	if ( xc_domain_pause( xci_, domain_ ) != 0 ) {

		if ( logHelper_ )
			logHelper_->error( std::string( "xc_domain_pause() failed: " ) + strerror( errno ) );

		return false;
	}

	return true;
}

bool XenDriver::unpause() throw()
{
	if ( xc_domain_unpause( xci_, domain_ ) != 0 ) {

		if ( logHelper_ )
			logHelper_->error( std::string( "xc_domain_unpause() failed: " ) + strerror( errno ) );

		return false;
	}

	return true;
}

bool XenDriver::setPageCacheLimit( size_t limit ) throw()
{
	return pageCache_.setLimit( limit );
}

unsigned int XenDriver::cpuid_eax( unsigned int op ) const
{
	unsigned int eax = 0;
	unsigned int ebx = 0;
	unsigned int ecx = 0;
	unsigned int edx = 0;

	__get_cpuid( op, &eax, &ebx, &ecx, &edx );

	return eax;
}

void XenDriver::getMtrrRange( uint64_t base_msr, uint64_t mask_msr, uint64_t &base, uint64_t &end ) const
{
	uint32_t mask_lo = ( uint32_t )mask_msr;
	uint32_t mask_hi = ( uint32_t )( mask_msr >> 32 );
	uint32_t base_lo = ( uint32_t )base_msr;
	uint32_t base_hi = ( uint32_t )( base_msr >> 32 );

	if ( ( mask_lo & 0x800 ) == 0 ) {
		/* Invalid (i.e. free) range */
		base = 0;
		end = 0;
		return;
	}

	uint32_t size_or_mask = ~( ( 1 << ( physAddr_ - XC_PAGE_SHIFT ) ) - 1 );

	/* Work out the shifted address mask. */
	mask_lo = ( size_or_mask | ( mask_hi << ( 32 - XC_PAGE_SHIFT ) ) | ( mask_lo >> XC_PAGE_SHIFT ) );

	/* This works correctly if size is a power of two (a contiguous range). */
	uint32_t size = -mask_lo;
	base = base_hi << ( 32 - XC_PAGE_SHIFT ) | base_lo >> XC_PAGE_SHIFT;
	end = base + size - 1;
}

bool XenDriver::isVarMtrrOverlapped( const struct hvm_hw_mtrr &hwMtrr ) const
{
	uint64_t phys_base, phys_mask, base_pre, end_pre, base, end;
	uint8_t num_var_ranges = ( uint8_t )hwMtrr.msr_mtrr_cap;

	for ( int32_t i = 0; i < num_var_ranges; ++i ) {

		uint64_t phys_base_pre = ( ( uint64_t * )hwMtrr.msr_mtrr_var )[i * 2];
		uint64_t phys_mask_pre = ( ( uint64_t * )hwMtrr.msr_mtrr_var )[i * 2 + 1];

		getMtrrRange( phys_base_pre, phys_mask_pre, base_pre, end_pre );

		for ( int32_t seg = i + 1; seg < num_var_ranges; ++seg ) {

			phys_base = ( ( uint64_t * )hwMtrr.msr_mtrr_var )[seg * 2];
			phys_mask = ( ( uint64_t * )hwMtrr.msr_mtrr_var )[seg * 2 + 1];

			getMtrrRange( phys_base, phys_mask, base, end );

			if ( ( ( base_pre != end_pre ) && ( base != end ) ) ||
			     ( ( base >= base_pre ) && ( base <= end_pre ) ) ||
			     ( ( end >= base_pre ) && ( end <= end_pre ) ) ||
			     ( ( base_pre >= base ) && ( base_pre <= end ) ) ||
			     ( ( end_pre >= base ) && ( end_pre <= end ) ) ) {

				/* MTRR is overlapped. */
				return true;
			}
		}
	}

	return false;
}

} // namespace bdvmi

