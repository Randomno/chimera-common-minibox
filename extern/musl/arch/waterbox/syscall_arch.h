#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

// "Regular call, plus an arg in rax" turned out to be more syntactially awful than I expected
// But it's not slow or anything.

// The interop address is materialized INSIDE the asm and r10 is listed
// as clobbered: r10 does not survive the call (the dispatcher uses it),
// so a compiler-visible C variable holding it must not be reused across
// two syscalls (fdopen's double fcntl jumped through the clobbered value
// straight to address zero). Every expansion below is self-sufficient.

static __inline long __syscall0(long n)
{
	register long _rax __asm__("rax") = n;
	__asm__ __volatile__ ("movabs $0x35f00000080, %%r10; call *%%r10"
		:"+r"(_rax)
		:
		:"r10", "rdi", "rsi", "rdx", "rcx", "r8", "r9", "r11", "r12", "r13", "r14", "r15", "rbx",
			"zmm0", "zmm1", "zmm2", "zmm3", "zmm4", "zmm5", "zmm6", "zmm7", "zmm8", "zmm9", "zmm10", "zmm11", "zmm12", "zmm13", "zmm14", "zmm15", "memory");
	return _rax;
}

static __inline long __syscall1(long n, long a1)
{
	register long _rax __asm__("rax") = n;
	register long _rdi __asm__("rdi") = a1;
	__asm__ __volatile__ ("movabs $0x35f00000080, %%r10; call *%%r10"
		:"+r"(_rax), "+r"(_rdi)
		:
		:"r10", "rsi", "rdx", "rcx", "r8", "r9", "r11", "r12", "r13", "r14", "r15", "rbx",
			"zmm0", "zmm1", "zmm2", "zmm3", "zmm4", "zmm5", "zmm6", "zmm7", "zmm8", "zmm9", "zmm10", "zmm11", "zmm12", "zmm13", "zmm14", "zmm15", "memory");
	return _rax;
}

static __inline long __syscall2(long n, long a1, long a2)
{
	register long _rax __asm__("rax") = n;
	register long _rdi __asm__("rdi") = a1;
	register long _rsi __asm__("rsi") = a2;	
	__asm__ __volatile__ ("movabs $0x35f00000080, %%r10; call *%%r10"
		:"+r"(_rax), "+r"(_rdi), "+r"(_rsi)
		:
		:"r10", "rdx", "rcx", "r8", "r9", "r11", "r12", "r13", "r14", "r15", "rbx",
			"zmm0", "zmm1", "zmm2", "zmm3", "zmm4", "zmm5", "zmm6", "zmm7", "zmm8", "zmm9", "zmm10", "zmm11", "zmm12", "zmm13", "zmm14", "zmm15", "memory");
	return _rax;
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
	register long _rax __asm__("rax") = n;
	register long _rdi __asm__("rdi") = a1;
	register long _rsi __asm__("rsi") = a2;
	register long _rdx __asm__("rdx") = a3;	
	__asm__ __volatile__ ("movabs $0x35f00000080, %%r10; call *%%r10"
		:"+r"(_rax), "+r"(_rdi), "+r"(_rsi), "+r"(_rdx)
		:
		:"r10", "rcx", "r8", "r9", "r11", "r12", "r13", "r14", "r15", "rbx",
			"zmm0", "zmm1", "zmm2", "zmm3", "zmm4", "zmm5", "zmm6", "zmm7", "zmm8", "zmm9", "zmm10", "zmm11", "zmm12", "zmm13", "zmm14", "zmm15", "memory");
	return _rax;
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	register long _rax __asm__("rax") = n;
	register long _rdi __asm__("rdi") = a1;
	register long _rsi __asm__("rsi") = a2;
	register long _rdx __asm__("rdx") = a3;
	register long _rcx __asm__("rcx") = a4;	
	__asm__ __volatile__ ("movabs $0x35f00000080, %%r10; call *%%r10"
		:"+r"(_rax), "+r"(_rdi), "+r"(_rsi), "+r"(_rdx), "+r"(_rcx)
		:
		:"r10", "r8", "r9", "r11", "r12", "r13", "r14", "r15", "rbx",
			"zmm0", "zmm1", "zmm2", "zmm3", "zmm4", "zmm5", "zmm6", "zmm7", "zmm8", "zmm9", "zmm10", "zmm11", "zmm12", "zmm13", "zmm14", "zmm15", "memory");
	return _rax;
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	register long _rax __asm__("rax") = n;
	register long _rdi __asm__("rdi") = a1;
	register long _rsi __asm__("rsi") = a2;
	register long _rdx __asm__("rdx") = a3;
	register long _rcx __asm__("rcx") = a4;
	register long _r8 __asm__("r8") = a5;	
	__asm__ __volatile__ ("movabs $0x35f00000080, %%r10; call *%%r10"
		:"+r"(_rax), "+r"(_rdi), "+r"(_rsi), "+r"(_rdx), "+r"(_rcx), "+r"(_r8)
		:
		:"r10", "r9", "r11", "r12", "r13", "r14", "r15", "rbx",
			"zmm0", "zmm1", "zmm2", "zmm3", "zmm4", "zmm5", "zmm6", "zmm7", "zmm8", "zmm9", "zmm10", "zmm11", "zmm12", "zmm13", "zmm14", "zmm15", "memory");
	return _rax;
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	register long _rax __asm__("rax") = n;
	register long _rdi __asm__("rdi") = a1;
	register long _rsi __asm__("rsi") = a2;
	register long _rdx __asm__("rdx") = a3;
	register long _rcx __asm__("rcx") = a4;
	register long _r8 __asm__("r8") = a5;
	register long _r9 __asm__("r9") = a6;	
	__asm__ __volatile__ ("movabs $0x35f00000080, %%r10; call *%%r10"
		:"+r"(_rax), "+r"(_rdi), "+r"(_rsi), "+r"(_rdx), "+r"(_rcx), "+r"(_r8), "+r"(_r9)
		:
		:"r10", "r11", "r12", "r13", "r14", "r15", "rbx",
			"zmm0", "zmm1", "zmm2", "zmm3", "zmm4", "zmm5", "zmm6", "zmm7", "zmm8", "zmm9", "zmm10", "zmm11", "zmm12", "zmm13", "zmm14", "zmm15", "memory");
	return _rax;
}

#define VDSO_USEFUL
#define VDSO_CGT_SYM "__vdso_clock_gettime"
#define VDSO_CGT_VER "LINUX_2.6"
#define VDSO_GETCPU_SYM "__vdso_getcpu"
#define VDSO_GETCPU_VER "LINUX_2.6"

#define IPC_64 0
