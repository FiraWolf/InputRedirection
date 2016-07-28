#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <3ds.h>

#define HID_PID 0x10

// 11.0 HID
#define HID_PATCH1_LOC 0x101de0
#define HID_PATCH2_LOC 0x107acc
#define HID_PATCH3_LOC 0x106a74
#define HID_CAVE_LOC   0x1094B8

#define HID_DAT_LOC	   0x10df00
#define HID_TS_RD_LOC 0x10df04
#define HID_TS_WR_LOC 0x10df08

bool hadError = false;

u32 protect_remote_memory(Handle hProcess, void* addr, u32 size)
{
	return svcControlProcessMemory(hProcess, (u32)addr, (u32)addr, size, 6, 7);
}

u32 copy_remote_memory(Handle hDst, void* ptrDst, Handle hSrc, void* ptrSrc, u32 size)
{
	static u32 done_state = 0;

	u32 ret, i, state;
	u32 dmaConfig[20] = { 0 };
	Handle hDma;

	ret = svcFlushProcessDataCache(hSrc, ptrSrc, size);
	ret = svcFlushProcessDataCache(hDst, ptrDst, size);
	ret = svcStartInterProcessDma(&hDma, hDst, ptrDst, hSrc, ptrSrc, size, dmaConfig);
	state = 0;

	if (done_state == 0)
	{
		ret = svcGetDmaState(&state, hDma);
		svcSleepThread(1000000000);
		ret = svcGetDmaState(&state, hDma);
		done_state = state;
		printf("InterProcessDmaFinishState: %08lx\n", state);
	}

	for (i = 0; i < 10000; i++)
	{
		state = 0;
		ret = svcGetDmaState(&state, hDma);
		if (state == done_state)
		{
			break;
		}
		svcSleepThread(1000000);
	}

	if (i >= 10000)
	{
		printf("readRemoteMemory time out %08lx\n", state);
		hadError = true;
		return 1; // error
	}

	svcCloseHandle(hDma);
	ret = svcInvalidateProcessDataCache(hDst, ptrDst, size);
	if (ret != 0)
	{
		return ret;
	}
	return 0;

}

u32 open_current_process()
{
	u32 handle = 0;
	u32 ret;
	u32 hCurrentProcess;
	u32 currentPid;

	svcGetProcessId(&currentPid, 0xffff8001);
	ret = svcOpenProcess(&handle, currentPid);
	if (ret != 0)
	{
		return 0;
	}
	hCurrentProcess = handle;
	return hCurrentProcess;
}

u32 open_process(u32 pid)
{
	Handle hProcess;
	Result r = svcOpenProcess(&hProcess, pid);
	if (r != 0)
	{
		return 0;
	}
	return hProcess;
}

s32 killCache_k()
{
	__asm__ volatile("cpsid aif");
	__asm__ volatile("mcr p15, 0, r0, c7, c5, 0"); // icache
	__asm__ volatile("mcr p15, 0, r0, c7, c14, 0"); // dcache
	return 0;
}

void killCache()
{
	svcBackdoor(killCache_k);
}

u32 branch(u32 base, u32 target)
{
	s32 off = (s32)(target - base);
	off -= 8; // arm is 2 instructions ahead (8 bytes)
	off /= 4; // word offset vs byte offset

	u32 ins = 0xea000000; // branch without link
	ins |= *(u32*)&off;
	return ins;
}

Handle target;
Handle self;

void hook(u32 loc, u32 storage, u32 *hook_code, u32 hook_len)
{
	if (protect_remote_memory(target, (void*)(storage & (~0xfff)), 0x1000) != 0)
	{
		hadError = true;
		printf("patch 4 prot failed\n");
	}

	if (copy_remote_memory(target, (void*)storage, self, hook_code, hook_len) != 0)
	{
		hadError = true;
		printf("patch 4 copy failed\n");
	}

	u32 br = branch(loc, storage);

	if (copy_remote_memory(target, (void*)loc, self, &br, 4) != 0)
	{
		hadError = true;
		printf("patch 3 copy failed\n");
	}
}

void read_input();
extern u32 read_input_sz;

int main()
{
	gfxInitDefault();

	consoleInit(GFX_BOTTOM, NULL);

	printf("injecting into hid..\n");

	self = open_current_process();

	u32 new_loc = HID_DAT_LOC;

	target = open_process(HID_PID);

	u32 test = 0;

	Result r = copy_remote_memory(self, &test, target, (void*)new_loc, 4);
	if (r != 0)
	{
		hadError = true;
		printf("copy returned %08lx\n", r);
		exit(0);
	}

	if (test != 0)
	{
		hadError = true;
		printf("HID was already patched.\n");
	}
	else
	{
		u32 f = 0xfff;
		r = copy_remote_memory(target, (void*)new_loc, self, &f, 4);
		if (r != 0)
		{
			hadError = true;
			printf("init copy failed\n");
		}

		if (protect_remote_memory(target, (void*)(HID_PATCH1_LOC & (~0xfff)), 0x1000) != 0)
		{
			hadError = true;
			printf("patch 1 prot failed\n");
		}

		if (copy_remote_memory(target, (void*)HID_PATCH1_LOC, self, &new_loc, 4) != 0)
		{
			hadError = true;
			printf("patch 1 copy failed\n");
		}

		if (protect_remote_memory(target, (void*)(HID_PATCH2_LOC & (~0xfff)), 0x1000) != 0)
		{
			hadError = true;
			printf("patch 2 prot failed\n");
		}

		if (copy_remote_memory(target, (void*)HID_PATCH2_LOC, self, &new_loc, 4) != 0)
		{
			hadError = true;
			printf("patch 2 copy failed\n");
		}

		if (protect_remote_memory(target, (void*)(HID_PATCH3_LOC & (~0xfff)), 0x1000) != 0)
		{
			hadError = true;
			printf("patch 2 prot failed\n");
		}

		hook(HID_PATCH3_LOC, HID_CAVE_LOC, (u32*)&read_input, read_input_sz);

		killCache();

		svcCloseHandle(target);
		svcCloseHandle(self);
	}

	if (hadError)
	{
		// Main loop
		while (aptMainLoop())
		{
			gspWaitForVBlank();
			hidScanInput();

			u32 kDown = hidKeysDown();
			if (kDown & KEY_START)
				break; // break in order to return to hbmenu
					   // Flush and swap framebuffers
			gfxFlushBuffers();
			gfxSwapBuffers();
		}
	}

	// Exit services
	gfxExit();
	return 0;
}