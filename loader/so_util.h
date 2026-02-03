#ifndef __SO_UTIL_H__
#define __SO_UTIL_H__

#include "elf.h"

#include <kubridge.h>
#include <psp2/types.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))
#define MAX_DATA_SEG 4

typedef struct {
	uintptr_t addr;
	uintptr_t thumb_addr;
	uint32_t orig_instr[2];
	uint32_t patch_instr[2];
} so_hook;

typedef struct so_module {
	struct so_module *next;

	SceUID text_blockid, data_blockid;
	uintptr_t text_base, data_base;
	size_t text_size, data_size;

	Elf32_Ehdr *ehdr;
	Elf32_Phdr *phdr;
	Elf32_Shdr *shdr;

	Elf32_Dyn *dynamic;
	Elf32_Sym *dynsym;
	Elf32_Rel *reldyn;
	Elf32_Rel *relplt;

	int (** init_array)(void);
	uint32_t *hash;

	int num_dynamic;
	int num_dynsym;
	int num_reldyn;
	int num_relplt;
	int num_init_array;

	char *soname;
	char *shstr;
	char *dynstr;
} so_module;

typedef struct {
	char *symbol;
	uintptr_t func;
} so_default_dynlib;

so_hook hook_thumb(uintptr_t addr, uintptr_t dst);
so_hook hook_arm(uintptr_t addr, uintptr_t dst);
so_hook hook_addr(uintptr_t addr, uintptr_t dst);

void so_flush_caches(so_module *mod);
int so_load(so_module *mod, const char *filename, uintptr_t load_addr);
int so_relocate(so_module *mod);
int so_resolve(so_module *mod, so_default_dynlib *default_dynlib, int size_default_dynlib, int default_dynlib_only);
void so_initialize(so_module *mod);
uintptr_t so_symbol(so_module *mod, const char *symbol);

#define SO_CONTINUE(type, h, ...) ({ \
	sceClibMemcpy((void *)h.addr, h.orig_instr, sizeof(h.orig_instr)); \
	kuKernelFlushCaches((void *)h.addr, sizeof(h.orig_instr)); \
	type r = h.thumb_addr ? ((type(*)())h.thumb_addr)(__VA_ARGS__) : ((type(*)())h.addr)(__VA_ARGS__); \
	sceClibMemcpy((void *)h.addr, h.patch_instr, sizeof(h.patch_instr)); \
	kuKernelFlushCaches((void *)h.addr, sizeof(h.patch_instr)); \
	r; \
})

#endif
