// Registers the ELF's C++ unwind tables with libgcc's unwinder.
//
// PS2 EE programs link with -nostartfiles, so gcc's crtbegin.o (which normally calls __register_frame_info) is not
// present, and nothing provides PT_GNU_EH_FRAME lookup on bare metal. Without registration every C++ throw calls
// abort() from inside _Unwind_RaiseException. ps2/link/ee-linkfile keeps .eh_frame and defines __EH_FRAME_BEGIN__.

extern "C"
{
	extern char __EH_FRAME_BEGIN__[];
	void __register_frame_info(const void *begin, void *object);
}

namespace
{
	// libgcc's `struct object` (unwind-dw2-fde.h) is 6 pointers on 32-bit targets; keep generous, aligned storage.
	alignas(16) unsigned char g_ehObject[64];

	// Runs before ordinary static constructors (priority 101 is the first user priority).
	__attribute__((constructor(101))) void RegisterEhFrames()
	{
		__register_frame_info(__EH_FRAME_BEGIN__, g_ehObject);
	}
}
