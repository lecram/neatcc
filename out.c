#include <unistd.h>
#include <elf.h>
#include "tok.h"

#define MAXSECS		(1 << 7)
#define MAXSYMS		(1 << 10)

#define SECSIZE		(1 << 12)
#define MAXRELA		(1 << 10)

#define SEC_SYMS		1
#define SEC_SYMSTR		2
#define SEC_BEG			3

static Elf64_Ehdr ehdr;
static Elf64_Shdr shdr[MAXSECS];
static int nshdr = SEC_BEG;
static Elf64_Sym syms[MAXSYMS];
static int nsyms;
static char symstr[MAXSYMS * 8];
static int nsymstr = 1;

static struct sec {
	char buf[SECSIZE];
	int len;
	int shnum;
	Elf64_Rela rela[MAXRELA];
	int nrela;
	Elf64_Shdr *sec_shdr;
	Elf64_Shdr *rel_shdr;
} secs[MAXSECS];
static int nsecs;
static struct sec *sec;

#define MAXTEMP		(1 << 12)
#define TMP_CONST	1
#define TMP_ADDR	2

static char *cur;
static long sp;
static struct tmp {
	long addr;
	int type;
} tmp[MAXTEMP];
static int ntmp;

static char *putint(char *s, long n, int l)
{
	while (l--) {
		*s++ = n;
		n >>= 8;
	}
}

static char *putstr(char *s, char *r)
{
	while (*r)
		*s++ = *r++;
	*s++ = '\0';
	return s;
}

static Elf64_Sym *put_sym(char *name)
{
	Elf64_Sym *sym = &syms[nsyms++];
	sym->st_name = nsymstr;
	nsymstr = putstr(symstr + nsymstr, name) - symstr;
	sym->st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
	return sym;
}

static void os(char *s, int n)
{
	while (n--)
		*cur++ = *s++;
}

static void oi(long n, int l)
{
	while (l--) {
		*cur++ = n;
		n >>= 8;
	}
}

static int tmp_pop(void)
{
	os("\x48\x8b\x45", 3);	/* mov top(%rbp), %rax */
	oi(-tmp[--ntmp].addr, 1);
	return tmp[ntmp].type;
}

static void tmp_push(int type)
{
	tmp[ntmp].addr = sp;
	tmp[ntmp].type = type;
	sp += 8;
	os("\x48\x89\x45", 3);	/* mov %rax, top(%rbp) */
	oi(-tmp[ntmp++].addr, 1);
}


void o_func_beg(char *name)
{
	Elf64_Sym *sym = put_sym(name);
	sec = &secs[nsecs++];
	sym->st_shndx = nshdr;
	sec->sec_shdr = &shdr[nshdr++];
	sec->rel_shdr = &shdr[nshdr++];
	cur = sec->buf;
	os("\x55", 1);
	os("\x48\x89\xe5", 3);
	sp = 8;
	os("\x48\x83\xec", 3);
	oi(56, 1);
}

void o_num(int n)
{
	os("\xb8", 1);
	oi(n, 4);
	tmp_push(TMP_CONST);
}

static void deref(void)
{
	os("\x48\x8b\x00", 3);	/* mov (%rax), %rax */
}

void o_deref(void)
{
	if (tmp_pop() == TMP_ADDR)
		deref();
	tmp_push(TMP_ADDR);
}

void o_ret(int ret)
{
	if (ret) {
		if (tmp_pop() == TMP_ADDR)
			deref();
	} else {
		os("\x48\x31\xc0", 3);	/* xor %rax, %rax */
	}
	os("\xc9\xc3", 2);		/* leave; ret; */
}

void o_func_end(void)
{
	os("\xc9\xc3", 2);		/* leave; ret; */
	sec->len = cur - sec->buf;
}

void o_local(long addr)
{
	os("\x48\x89\xe8", 3);		/* mov %rbp, %rax */
	os("\x48\x83\xc0", 3);		/* add $addr, %rax */
	oi(-addr, 1);
	tmp_push(TMP_ADDR);
}

long o_mklocal(void)
{
	long addr = sp;
	sp += 8;
	return addr;
}

void o_rmlocal(long addr)
{
	sp = addr;
}

void o_assign(void)
{
	if (tmp_pop() == TMP_ADDR)
		deref();
	os("\x48\x89\xc3", 3);		/* mov %rax, %rbx */
	tmp_pop();
	os("\x48\x89\x18", 3);		/* mov %rbx, (%rax) */
}

static long codeaddr(void)
{
	return cur - sec->buf;
}

long o_mklabel(void)
{
	return codeaddr();
}

void o_jz(long addr)
{
	os("\x48\x85\xc0", 3);		/* test %rax, %rax */
	os("\x0f\x84", 2);		/* jz $addr */
	oi(codeaddr() - addr - 4, 4);
}

long o_stubjz(void)
{
	o_jz(codeaddr());
	return cur - sec->buf - 4;
}

void o_filljz(long addr)
{
	putint(sec->buf + addr, codeaddr() - addr - 4, 4);
}

void out_init(void)
{
}

void out_write(int fd)
{
	Elf64_Shdr *symstr_shdr = &shdr[SEC_SYMSTR];
	Elf64_Shdr *syms_shdr = &shdr[SEC_SYMS];
	unsigned long offset = sizeof(ehdr);
	int i;

	ehdr.e_ident[0] = 0x7f;
	ehdr.e_ident[1] = 'E';
	ehdr.e_ident[2] = 'L';
	ehdr.e_ident[3] = 'F';
	ehdr.e_ident[4] = ELFCLASS64;
	ehdr.e_ident[5] = ELFDATA2LSB;
	ehdr.e_ident[6] = EV_CURRENT;
	ehdr.e_type = ET_REL;
	ehdr.e_machine = EM_X86_64;
	ehdr.e_version = EV_CURRENT;
	ehdr.e_ehsize = sizeof(ehdr);
	ehdr.e_shentsize = sizeof(shdr[0]);
	ehdr.e_shoff = offset;
	ehdr.e_shnum = nshdr;
	ehdr.e_shstrndx = SEC_SYMSTR;
	offset += sizeof(shdr[0]) * nshdr;

	syms_shdr->sh_type = SHT_SYMTAB;
	syms_shdr->sh_offset = offset;
	syms_shdr->sh_size = nsyms * sizeof(syms[0]);
	syms_shdr->sh_entsize = sizeof(syms[0]);
	syms_shdr->sh_link = SEC_SYMSTR;
	offset += syms_shdr->sh_size;

	symstr_shdr->sh_type = SHT_STRTAB;
	symstr_shdr->sh_offset = offset;
	symstr_shdr->sh_size = nsymstr;
	symstr_shdr->sh_entsize = 1;
	offset += symstr_shdr->sh_size;

	for (i = 0; i < nsecs; i++) {
		struct sec *sec = &secs[i];

		sec->sec_shdr->sh_type = SHT_PROGBITS;
		sec->sec_shdr->sh_flags = SHF_EXECINSTR;
		sec->sec_shdr->sh_offset = offset;
		sec->sec_shdr->sh_size = sec->len;
		sec->sec_shdr->sh_entsize = 1;
		offset += sec->sec_shdr->sh_size;

		sec->rel_shdr->sh_type = SHT_RELA;
		sec->rel_shdr->sh_offset = offset;
		sec->rel_shdr->sh_size = sec->nrela * sizeof(sec->rela[0]);
		sec->rel_shdr->sh_entsize = sizeof(sec->rela[0]);
		sec->rel_shdr->sh_link = sec->sec_shdr - shdr;
		offset += sec->rel_shdr->sh_size;
	}

	write(fd, &ehdr, sizeof(ehdr));
	write(fd, shdr, sizeof(shdr[0]) * nshdr);
	write(fd, syms, sizeof(syms[0]) * nsyms);
	write(fd, symstr, nsymstr);
	for (i = 0; i < nsecs; i++) {
		struct sec *sec = &secs[i];
		write(fd, sec->buf, sec->len);
		write(fd, sec->rela, sec->nrela * sizeof(sec->rela[0]));
	}
}
