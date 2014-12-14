/*
 * Copyright (c) 2010, Stefan Lankes, RWTH Aachen University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the University nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <eduos/stddef.h>
#include <eduos/stdio.h>
#include <eduos/stdlib.h>
#include <eduos/string.h>
#include <eduos/errno.h>
#include <eduos/processor.h>
#include <eduos/time.h>
#include <eduos/spinlock.h>
#include <asm/irq.h>
#include <asm/idt.h>
#include <asm/irqflags.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/apic.h>
#include <asm/multiboot.h>

// IO APIC MMIO structure: write reg, then read or write data.
typedef struct {
	uint32_t reg;
	uint32_t pad[3];
	uint32_t data;
} ioapic_t;

#ifndef MAX_CORES
#define MAX_CORES	1
#endif

static const apic_processor_entry_t* apic_processors[MAX_CORES] = {[0 ... MAX_CORES-1] = NULL};
static uint32_t boot_processor = MAX_CORES;
apic_mp_t* apic_mp  __attribute__ ((section (".data"))) = NULL;
static apic_config_table_t* apic_config = NULL;
static size_t lapic = 0;
static volatile ioapic_t* ioapic = NULL;
static uint32_t icr = 0;
static uint32_t ncores = 1;
static uint8_t irq_redirect[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF};
static uint8_t initialized = 0;
spinlock_t bootlock = SPINLOCK_INIT;

// forward declaration
static int lapic_reset(void);

static uint32_t lapic_read_default(uint32_t addr)
{
	return *((const volatile uint32_t*) (lapic+addr));
}

static uint32_t lapic_read_msr(uint32_t addr)
{
	return rdmsr(0x800 + (addr >> 4));
}

typedef uint32_t (*lapic_read_func)(uint32_t addr);

static lapic_read_func lapic_read = lapic_read_default;

static void lapic_write_default(uint32_t addr, uint32_t value)
{
#if 1
	/* 
	 * to avoid a pentium bug, we have to read a apic register 
	 * before we write a value to this register
	 */
	asm volatile ("movl (%%eax), %%edx; movl %%ebx, (%%eax)" :: "a"(lapic+addr), "b"(value) : "%edx");
#else
	*((volatile uint32_t*) (lapic+addr)) = value;
#endif
}

static void lapic_write_msr(uint32_t addr, uint32_t value)
{
	wrmsr(0x800 + (addr >> 4), value);
}

typedef void (*lapic_write_func)(uint32_t addr, uint32_t value);

static lapic_write_func lapic_write = lapic_write_default;

static inline uint32_t ioapic_read(uint32_t reg)
{
	ioapic->reg = reg;

	return ioapic->data;
}

static inline void ioapic_write(uint32_t reg, uint32_t value)
{
	ioapic->reg = reg;
	ioapic->data = value;
}

static inline uint32_t ioapic_version(void)
{
	if (ioapic)
		return ioapic_read(IOAPIC_REG_VER) & 0xFF;

	return 0;
}

static inline uint32_t ioapic_max_redirection_entry(void)
{
	if (ioapic)
		 return (ioapic_read(IOAPIC_REG_VER) >> 16) & 0xFF;

	return 0;
}

/*
 * Send a 'End of Interrupt' command to the APIC
 */
void apic_eoi(void)
{
	if (BUILTIN_EXPECT(lapic, 1))
		lapic_write(APIC_EOI, 0);
}

uint32_t apic_cpu_id(void)
{
	if (lapic && initialized)
		return ((lapic_read(APIC_ID)) >> 24);
	return 0;
}

static inline void apic_set_cpu_id(uint32_t id)
{
	if (lapic && initialized)
		lapic_write(APIC_ID, id << 24);
}

static inline uint32_t apic_version(void)
{
	if (lapic)
		return lapic_read(APIC_VERSION) & 0xFF;

	return 0;
}

static inline uint32_t apic_lvt_entries(void)
{
	if (lapic)
		return (lapic_read(APIC_VERSION) >> 16) & 0xFF;

	return 0;
}

int apic_is_enabled(void)
{
	return (lapic && initialized);
}

int apic_disable_timer(void)
{
	if (BUILTIN_EXPECT(!apic_is_enabled(), 0))
		return -EINVAL;

	lapic_write(APIC_LVT_T, 0x10000);	// disable timer interrupt

	return 0;
}

int apic_enable_timer(void)
{
	if (BUILTIN_EXPECT(apic_is_enabled() && icr, 1)) {
		lapic_write(APIC_DCR, 0xB);		// set it to 1 clock increments
		lapic_write(APIC_LVT_T, 0x2007B);	// connects the timer to 123 and enables it
		lapic_write(APIC_ICR, icr);

		return 0;
	}

	return -EINVAL;
}

static apic_mp_t* search_apic(size_t base, size_t limit) {
	size_t ptr, ptr_old = 0;
	apic_mp_t* tmp;

	for (ptr=base; ptr<=limit-sizeof(uint32_t); ptr++) {
		tmp = (apic_mp_t*) ptr;

		if (!(ptr & PAGE_MASK)) {
			if (ptr_old)
				page_unmap(ptr_old, 1);
			ptr_old = ptr;
			page_map(ptr, ptr, 1, PG_GLOBAL | PG_RW | PG_PCD);
		}

		if (tmp->signature == MP_FLT_SIGNATURE) {
			if (!((tmp->version > 4) || tmp->features[0]))
				return tmp;
		}
	}

	if (ptr_old)
		page_unmap(ptr_old, 1);

	return NULL;
}

static int lapic_reset(void)
{
	uint32_t max_lvt;

	if (!lapic)
		return -ENXIO;

	max_lvt = apic_lvt_entries();

	lapic_write(APIC_SVR, 0x17F);	// enable the apic and connect to the idt entry 127
	lapic_write(APIC_TPR, 0x00);	// allow all interrupts
	if (icr) {
		lapic_write(APIC_DCR, 0xB);		// set it to 1 clock increments
		lapic_write(APIC_LVT_T, 0x2007B);	// connects the timer to 123 and enables it
		lapic_write(APIC_ICR, icr);
	} else 
		lapic_write(APIC_LVT_T, 0x10000);	// disable timer interrupt
	if (max_lvt >= 4)
		lapic_write(APIC_LVT_TSR, 0x10000);	// disable thermal sensor interrupt
	if (max_lvt >= 5)
		lapic_write(APIC_LVT_PMC, 0x10000);	// disable performance counter interrupt
	lapic_write(APIC_LINT0, 0x7C);	// connect LINT0 to idt entry 124
	lapic_write(APIC_LINT1, 0x7D);	// connect LINT1 to idt entry 125
	lapic_write(APIC_LVT_ER, 0x7E);	// connect error to idt entry 126

	return 0;
}

/* 
 * detects the timer frequency of the APIC and restart
 * the APIC timer with the correct period
 */
int apic_calibration(void)
{
	uint32_t i;
	uint32_t flags;
	uint64_t ticks, old;

	if (!lapic)
		return -ENXIO;

	old = get_clock_tick();

	/* wait for the next time slice */
	while ((ticks = get_clock_tick()) - old == 0)
		HALT;

	flags = irq_nested_disable();
	lapic_write(APIC_DCR, 0xB);			// set it to 1 clock increments
	lapic_write(APIC_LVT_T, 0x2007B); 	// connects the timer to 123 and enables it
	lapic_write(APIC_ICR, 0xFFFFFFFFUL);
	irq_nested_enable(flags);

	/* wait 3 time slices to determine a ICR */
	while (get_clock_tick() - ticks < 3)
		HALT;

	icr = (0xFFFFFFFFUL - lapic_read(APIC_CCR)) / 3;

	flags = irq_nested_disable();
	lapic_reset();
	irq_nested_enable(flags);

	// Now, eduOS is able to use the APIC => Therefore, we disable the PIC
	outportb(0xA1, 0xFF);
	outportb(0x21, 0xFF);

	kprintf("APIC calibration determines an ICR of 0x%x\n", icr);

	flags = irq_nested_disable();

	if (ioapic) {
		uint32_t max_entry = ioapic_max_redirection_entry();

		// now lets turn everything else on
		for(i=0; i<=max_entry; i++)
			if (i != 2) 
				ioapic_inton(i, apic_processors[boot_processor]->id);
		// now, we don't longer need the IOAPIC timer and turn it off
		ioapic_intoff(2, apic_processors[boot_processor]->id);
	}
	initialized = 1;
	irq_nested_enable(flags);

	return 0;
}

static int apic_probe(void)
{
	size_t addr;
	uint32_t i, count;
	int isa_bus = -1;

	apic_mp = search_apic(0xF0000, 0x100000);
	if (apic_mp)
		goto found_mp;
	apic_mp = search_apic(0x9F000, 0xA0000);
	if (apic_mp)
		goto found_mp;

found_mp:
	if (!apic_mp) 
		goto no_mp;

	kprintf("Found MP config table at 0x%x\n", apic_mp);
	kprintf("System uses Multiprocessing Specification 1.%u\n", apic_mp->version);	
	kprintf("MP features 1: %u\n", apic_mp->features[0]);

	if (apic_mp->features[0]) {
		kputs("Currently, eduOS supports only multiprocessing via the MP config tables!\n");
		goto no_mp;
	}

	apic_config = (apic_config_table_t*) ((size_t) apic_mp->mp_config);
	if (!apic_config || strncmp((void*) &apic_config->signature, "PCMP", 4) !=0) {
		kputs("Invalid MP config table\n");
		goto no_mp;
	}

	addr = (size_t) apic_config;
	addr += sizeof(apic_config_table_t);
	if (addr % 4)
		addr += 4 - addr % 4;

	// search the ISA bus => required to redirect the IRQs
	for(i=0; i<apic_config->entry_count; i++) {
		switch(*((uint8_t*) addr)) {
		case 0: 
			addr += 20;
			break;
		case 1: {
				apic_bus_entry_t* mp_bus;

				mp_bus = (apic_bus_entry_t*) addr;
				if (mp_bus->name[0] == 'I' && mp_bus->name[1] == 'S' &&
				    mp_bus->name[2] == 'A')
					isa_bus = i;
			}
			break;
		default:
			addr += 8;
		}
	}

	addr = (size_t) apic_config;
	addr += sizeof(apic_config_table_t);
	if (addr % 4)
		addr += 4 - addr % 4;

	for(i=0, count=0; i<apic_config->entry_count; i++) {
		if (*((uint8_t*) addr) == 0) { // cpu entry
			if (i < MAX_CORES) {
				apic_processors[i] = (apic_processor_entry_t*) addr;
				//TODO: remove dirty hack
				page_map((size_t)apic_processors[i] & PAGE_MASK, (size_t)apic_processors[i] & PAGE_MASK, 1, PG_GLOBAL | PG_RW | PG_PCD);
				if (!(apic_processors[i]->cpu_flags & 0x01)) // is the processor usable?
					apic_processors[i] = NULL;
				else if (apic_processors[i]->cpu_flags & 0x02)
					boot_processor = i;
			}
			count++;
			addr += 20;
		} else if (*((uint8_t*) addr) == 2) { // IO_APIC
			apic_io_entry_t* io_entry = (apic_io_entry_t*) addr;
			ioapic = (ioapic_t*) ((size_t) io_entry->addr);
			kprintf("Found IOAPIC at 0x%x\n", ioapic);
			//TODO: remove dirty hack
			page_map(0x91000, (size_t)ioapic & PAGE_MASK, 1, PG_GLOBAL | PG_RW | PG_PCD);
			ioapic = (ioapic_t*) 0x91000;
			addr += 8;
			kprintf("Map IOAPIC to 0x%x\n", ioapic);
		} else if (*((uint8_t*) addr) == 3) { // IO_INT
			/* TODO: page_map is missing */
			apic_ioirq_entry_t* extint = (apic_ioirq_entry_t*) addr;
			if (extint->src_bus == isa_bus) {
				irq_redirect[extint->src_irq] = extint->dest_intin;
				kprintf("Redirect irq %u -> %u\n", extint->src_irq,  extint->dest_intin);
			}
			addr += 8;
		} else addr += 8;
	}
	kprintf("Found %u cores\n", count);

	if (count > MAX_CORES) {
		kputs("Found too many cores! Increase the macro MAX_CORES!\n");
		goto no_mp;
	}
	ncores = count;

check_lapic:
	if (apic_config)
		lapic = apic_config->lapic;
	else if (has_apic()) 
		lapic = 0xFEE00000;

	if (!lapic)
		goto out;
	kprintf("Found APIC at 0x%x\n", lapic);
	//TODO: remove dirty hack
	page_map(0x90000, (size_t)lapic & PAGE_MASK, 1, PG_GLOBAL | PG_RW | PG_PCD);
	lapic = 0x90000;

	if (has_x2apic()) {
		kprintf("Enable X2APIC support!\n");
		wrmsr(0x1B, lapic | 0xD00);
		lapic_read = lapic_read_msr;
		lapic_write = lapic_write_msr;
	}

	kprintf("Map APIC to 0x%x\n", lapic);
	kprintf("Maximum LVT Entry: 0x%x\n", apic_lvt_entries());
	kprintf("APIC Version: 0x%x\n", apic_version());

	if (!((apic_version() >> 4))) {
		kprintf("Currently, eduOS didn't supports extern APICs!\n");
		goto out;
	}

	if (apic_lvt_entries() < 3) {
		kprintf("LVT is too small\n");
		goto out;
	}

	return 0;

out:
	apic_mp = NULL;
	apic_config = NULL;
	lapic = 0;
	ncores = 1;
	return -ENXIO;

no_mp:
	apic_mp = NULL;
	apic_config = NULL;
	ncores = 1;
	goto check_lapic;
}

static void apic_err_handler(struct state *s)
{
	kprintf("Got APIC error 0x%x\n", lapic_read(APIC_ESR));
}

int apic_init(void)
{ 
	int ret;

	ret = apic_probe();
	if (ret)
		return ret;

	// set APIC error handler
	irq_install_handler(126, apic_err_handler);
	kprintf("Boot processor %u (ID %u)\n", boot_processor, apic_processors[boot_processor]->id);

	return 0;
}

int ioapic_inton(uint8_t irq, uint8_t apicid)
{
	ioapic_route_t route;
	uint32_t off;

	if (BUILTIN_EXPECT(irq > 24, 0)){
		kprintf("IOAPIC: trying to turn on irq %i which is too high\n", irq);
		return -EINVAL;
	}

	if (irq < 16)
		off = irq_redirect[irq]*2;
	else
		off = irq*2;
#if 0
	route.lower.whole = ioapic_read(IOAPIC_REG_TABLE+1+off);
	route.dest.upper = ioapic_read(IOAPIC_REG_TABLE+off);
	route.lower.bitfield.mask = 0; // turn it on (stop masking)
#else
	route.lower.bitfield.dest_mode = 0;
	route.lower.bitfield.mask = 0;
	route.dest.physical.physical_dest = apicid; // send to the boot processor
	route.lower.bitfield.delivery_mode = 0;
	route.lower.bitfield.polarity = 0;
	route.lower.bitfield.trigger = 0;
	route.lower.bitfield.vector = 0x20+irq;
	route.lower.bitfield.mask = 0; // turn it on (stop masking)
#endif

	ioapic_write(IOAPIC_REG_TABLE+off, route.lower.whole);
	ioapic_write(IOAPIC_REG_TABLE+1+off, route.dest.upper);

	route.dest.upper = ioapic_read(IOAPIC_REG_TABLE+1+off);
        route.lower.whole = ioapic_read(IOAPIC_REG_TABLE+off);

	return 0;
}

int ioapic_intoff(uint8_t irq, uint8_t apicid)
{
	ioapic_route_t route;
	uint32_t off;

	if (BUILTIN_EXPECT(irq > 24, 0)){
		kprintf("IOAPIC: trying to turn on irq %i which is too high\n", irq);
		return -EINVAL;
	}

	if (irq < 16) 
		off = irq_redirect[irq]*2;
	else
		off = irq*2;

#if 0
	route.lower.whole = ioapic_read(IOAPIC_REG_TABLE+1+off);
	route.dest.upper = ioapic_read(IOAPIC_REG_TABLE+off);
	route.lower.bitfield.mask = 1; // turn it off (start masking)
#else
	route.lower.bitfield.dest_mode = 0;
	route.lower.bitfield.mask = 0;
	route.dest.physical.physical_dest = apicid;
	route.lower.bitfield.delivery_mode = 0;
	route.lower.bitfield.polarity = 0;
	route.lower.bitfield.trigger = 0;
	route.lower.bitfield.vector = 0x20+irq;
	route.lower.bitfield.mask = 1; // turn it off (start masking)
#endif

	ioapic_write(IOAPIC_REG_TABLE+off, route.lower.whole);
	ioapic_write(IOAPIC_REG_TABLE+1+off, route.dest.upper);

	return 0;
}
