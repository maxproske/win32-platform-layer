// WIN32 Platform Layer TODO
// --------------------
// * This is not a final win32 platform layer we would ship.
// * Sound buffer up that we can play into
// * Graphics buffer we can write directly into
// * Rudementery input from a controller
// * Timing (sleep/timeBeginPeriod to not melt processor)
// * File I/O (saved game locations, asset path)
// * Get a handle to our own exe
// * Threading
#include "handmade.h"

// #include "handmade.cpp" is the "Unity" build (no, not the game engine)
// Instead of chaining on the cli (cl ... win32_handmade.cpp handmade.cpp), everything in one translation unit
#include "handmade.cpp"

// Put as much above windows.h as possible, so #defines do not conflict
#include <windows.h>
#include <xinput.h>
#include <dsound.h>
#include <stdio.h>
#include <malloc.h>

struct win32_offscreen_buffer
{
	BITMAPINFO Info;
	void *Memory;
	int Width;
	int Height;
	int Pitch;
};

global_variable bool32 GlobalRunning;
global_variable win32_offscreen_buffer GlobalBackbuffer;
global_variable LPDIRECTSOUNDBUFFER GlobalSecondaryBuffer;

struct win32_window_dimension
{
	int Width;
	int Height;
};

// Hoist out Windows XInput, so we don't rely on a dll being on someone's machine.
// There is an external function called this in case you didn't know
// So bind to this during the linker stage, and do not throw a LNK error
// -----
// Use macros to define function pointers
// Macros are a very efficient way of defining a function prototype once
#define X_INPUT_GET_STATE(name) DWORD WINAPI name(DWORD dwUserIndex, XINPUT_STATE *pState)
// Defining a type of that, so you can use it as a pointer
typedef X_INPUT_GET_STATE(x_input_get_state);
// And creating a stub. (If the function doesn't exist, just return 0. No harm done)
X_INPUT_GET_STATE(XInputGetStateStub)
{
	return(ERROR_DEVICE_NOT_CONNECTED);
}
// So our function pointer will never be 0x000000
global_variable x_input_get_state *XInputGetState_ = XInputGetStateStub;
#define XInputGetState XInputGetState_

#define X_INPUT_SET_STATE(name) DWORD WINAPI name(DWORD dwUserIndex, XINPUT_VIBRATION *pVibration)
typedef X_INPUT_SET_STATE(x_input_set_state);
X_INPUT_SET_STATE(XInputSetStateStub)
{
	return(ERROR_DEVICE_NOT_CONNECTED);
}
global_variable x_input_set_state *XInputSetState_ = XInputSetStateStub;
#define XInputSetState XInputSetState_

#define DIRECT_SOUND_CREATE(name) HRESULT WINAPI name(LPCGUID pcGuidDevice, LPDIRECTSOUND *ppDS, LPUNKNOWN pUnkOuter)
typedef DIRECT_SOUND_CREATE(direct_sound_create);

// Implements Win32 file loading
void* PlatformLoadFile(char *FileName)
{
	return 0;
}

// Take loading Windows DLL into our own hands
internal void Win32LoadXInput() {
	// TODO(max): Test this on Windows 8 which only has 1.4
	HMODULE XInputLibrary = LoadLibraryA("xinput1_4.dll");
	if (!XInputLibrary) {
		HMODULE XInputLibrary = LoadLibraryA("xinput9_1_0.dll");
	}
	if (!XInputLibrary) {
		HMODULE XInputLibrary = LoadLibraryA("xinput1_3.dll"); // Looks in a number of places (exe folder first, then Windows)
	}
	if (XInputLibrary)
	{
		// Get address of procedures from .dll
		XInputGetState = (x_input_get_state *)GetProcAddress(XInputLibrary, "XInputGetState");
		XInputSetState = (x_input_set_state *)GetProcAddress(XInputLibrary, "XInputSetState");
	}
}

win32_window_dimension Win32GetWindowDimension(HWND Window)
{
	win32_window_dimension Result;

	RECT ClientRect;
	GetClientRect(Window, &ClientRect);
	Result.Width = ClientRect.right - ClientRect.left;
	Result.Height = ClientRect.bottom - ClientRect.top;

	return(Result);
}

struct win32_sound_output
{
	int SamplesPerSecond;
	int ToneHz;
	int16 ToneVolume;
	uint32 RunningSampleIndex;
	int WavePeriod;
	int BytesPerSample;
	int SecondaryBufferSize;
	real32 tSine;
	int LatencySampleCount;
};

internal void Win32InitDSound(HWND Window, int32 SamplesPerSecond, int32 BufferSize)
{
	// Load the library
	HMODULE DSoundLibrary = LoadLibraryA("dsound.dll");

	if (DSoundLibrary)
	{
		direct_sound_create *DirectSoundCreate = (direct_sound_create *)GetProcAddress(DSoundLibrary, "DirectSoundCreate");
		
		LPDIRECTSOUND DirectSound;
		if (DirectSoundCreate && SUCCEEDED(DirectSoundCreate(0, &DirectSound, 0)))
		{
			// Set format
			WAVEFORMATEX WaveFormat = {};
			WaveFormat.wFormatTag = WAVE_FORMAT_PCM;
			WaveFormat.nChannels = 2; // Stereo
			WaveFormat.nSamplesPerSec = SamplesPerSecond;
			WaveFormat.wBitsPerSample = 16; // All audio is 16-bit
			WaveFormat.nBlockAlign = (WaveFormat.nChannels * WaveFormat.wBitsPerSample) / 8; // redundant
			WaveFormat.nAvgBytesPerSec = WaveFormat.nSamplesPerSec * WaveFormat.nBlockAlign; // redundant, ffs windows
			WaveFormat.cbSize = 0;
			// Set cooperative level
			HRESULT Error = DirectSound->SetCooperativeLevel(Window, DSSCL_PRIORITY);
			if (SUCCEEDED(Error))
			{
				// Create Primary buffer (NOT a buffer, just a handle to the sound card)
				DSBUFFERDESC BufferDescription = {}; // Zero before using
				BufferDescription.dwSize = sizeof(BufferDescription);
				BufferDescription.dwFlags = DSBCAPS_PRIMARYBUFFER; // Handle
				LPDIRECTSOUNDBUFFER PrimaryBuffer;
				if (SUCCEEDED(DirectSound->CreateSoundBuffer(&BufferDescription, &PrimaryBuffer, 0)))
				{
					// We will never send this buffer data. Just set the waveformat (48K, 44.1K, etc.)
					if (SUCCEEDED(PrimaryBuffer->SetFormat(&WaveFormat)))
					{
						// We have finally set the format of the primary format
						OutputDebugStringA("Primary format was set.\n");
					}
					else 
					{
						// Diagnostic
					}
				}
				else 
				{
					// Diagnostic
				}
			}
			else
			{
				// Diagnostic
			}

			// Create Secondary buffer (NOT double bnuffering, this is the actual buffer sound is output to)
			DSBUFFERDESC BufferDescription = {};
			BufferDescription.dwSize = sizeof(BufferDescription);
			BufferDescription.dwFlags = 0;
			BufferDescription.dwBufferBytes = BufferSize;
			BufferDescription.lpwfxFormat = &WaveFormat;
			Error = DirectSound->CreateSoundBuffer(&BufferDescription, &GlobalSecondaryBuffer, 0);
			if (SUCCEEDED(Error))
			{
				// Start playing
				OutputDebugStringA("Secondary buffer created successfully.\n");
			}
		}
		else
		{
			// Diagnostic
		}
	}
	else
	{
		// Diagnostic
	}
}

// Allocate back buffer
internal void Win32ResizeDIBSection(win32_offscreen_buffer *Buffer, int Width, int Height)
{
	// Free first
	if (Buffer->Memory)
	{
		VirtualFree(Buffer->Memory, 0, MEM_RELEASE); // 0 remembers how it was allocated
	}

	// Fill out bitmap info header
	Buffer->Width = Width;
	Buffer->Height = Height;
	int BytesPerPixel = 4;

	Buffer->Info.bmiHeader.biSize = sizeof(Buffer->Info.bmiHeader);
	Buffer->Info.bmiHeader.biWidth = Buffer->Width;
	Buffer->Info.bmiHeader.biHeight = -Buffer->Height; // Start from top left. Performance penalty?
	Buffer->Info.bmiHeader.biPlanes = 1;
	Buffer->Info.bmiHeader.biBitCount = 32; // RGB + 8 bit padding so we fall on 32-bit boundaries
	Buffer->Info.bmiHeader.biCompression = BI_RGB; // No compression

	// Allocate memory ourselves using low-level, Windows API
	int BitmapMemorySize = (Buffer->Width*Buffer->Height)*BytesPerPixel;
	Buffer->Memory = VirtualAlloc(0, BitmapMemorySize, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
	Buffer->Pitch = Buffer->Width*BytesPerPixel;
}

// Blit to whole window
internal void Win32DisplayBufferInWindow(
	win32_offscreen_buffer *Buffer,
	HDC DeviceContext,
	int WindowWidth, int WindowHeight,
	int X, int Y, int Width, int Height)
{
	// TODO(max): Correct aspect ratio on resize
	StretchDIBits(DeviceContext,
		0, 0, WindowWidth, WindowHeight, // dst
		0, 0, Buffer->Width, Buffer->Height, // src
		Buffer->Memory, &Buffer->Info,
		DIB_RGB_COLORS, // RGB, not using a palette town
		SRCCOPY); // Direct copy bits, no bitwise ops necessary
}

// Handle Windows events
LRESULT CALLBACK Win32MainWindowCallback(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	LRESULT Result = 0; // 64 bits so WindowProc can return any type

	switch (Message)
	{
	case WM_SIZE:
	{
	} break;
	case WM_DESTROY:
	{
		GlobalRunning = false;
	} break;
	case WM_CLOSE:
	{
		GlobalRunning = false;
	} break;
	case WM_ACTIVATEAPP:
	{
	} break;
	case WM_SETCURSOR:
	{
	} break;
	// Handle SYS keys ourselves instead of DefWindowProc
	case WM_SYSKEYDOWN:
	case WM_SYSKEYUP:
	case WM_KEYDOWN:
	case WM_KEYUP:
	{
		uint32 VKCode = WParam; // Virtual-key code for non-ANSI keys 
		// LParam gives you even additional info (LParam & (1 << 30); for up before the message was sent or after)
		#define KeyMessageWasDownBit (1 << 30)
		#define KeyMessageIsDownBit (1 << 31)
		bool32 WasDown = ((LParam & KeyMessageWasDownBit) != 0);
		bool32 IsDown = ((LParam & KeyMessageIsDownBit) == 0);

		if (WasDown != IsDown)
		{
			if (VKCode == 'W')
			{
				OutputDebugStringA("W\n");
			}
			else if (VKCode == 'A')
			{
				OutputDebugStringA("A\n");
			}
			else if (VKCode == 'S')
			{
				OutputDebugStringA("S\n");
			}
			else if (VKCode == 'D')
			{
				OutputDebugStringA("D\n");
			}
			else if (VKCode == 'Q')
			{
				OutputDebugStringA("Q\n");
			}
			else if (VKCode == 'E')
			{
				OutputDebugStringA("E\n");
			}
			else if (VKCode == VK_UP)
			{
				OutputDebugStringA("VK_UP\n");
			}
			else if (VKCode == VK_LEFT)
			{
				OutputDebugStringA("VK_LEFT\n");
			}
			else if (VKCode == VK_RIGHT)
			{
				OutputDebugStringA("VK_RIGHT\n");
			}
			else if (VKCode == VK_DOWN)
			{
				OutputDebugStringA("VK_DOWN\n");
			}
			else if (VKCode == VK_ESCAPE)
			{
				if (IsDown) {
					OutputDebugStringA("VK_ESCAPE: is down.\n");
				}
				if (WasDown) {
					OutputDebugStringA("VK_ESCAPE: was down.\n");
				}
			}
			else if (VKCode == VK_SPACE)
			{
				OutputDebugStringA("VK_SPACE\n");
			}
		}

		bool32 AltKeyWasDown = ((LParam & (1 << 29)) != 0);
		if ((VKCode == VK_F4) && AltKeyWasDown) {
			GlobalRunning = false;
		}
	} break;
	case WM_PAINT:
	{
		// Manually repaint obscured/dirty areas (offscreen/during resizing)
		PAINTSTRUCT Paint;
		HDC DeviceContext = BeginPaint(Window, &Paint); // For WM_PAINT
		int X = Paint.rcPaint.left;
		int Y = Paint.rcPaint.top;
		int Width = Paint.rcPaint.right - Paint.rcPaint.left;
		int Height = Paint.rcPaint.bottom - Paint.rcPaint.top;

		// Note(max): Do we need to uncomment this?
		//Win32InitDSound(Window, 48000, 48000*sizeof(int16)*2); // L+R channels (LRLRLR...)

		// Draw to rect
		win32_window_dimension Dimension = Win32GetWindowDimension(Window);
		Win32DisplayBufferInWindow(&GlobalBackbuffer, DeviceContext, Dimension.Width, Dimension.Height, X, Y, Width, Height);

		EndPaint(Window, &Paint); // For WM_PAINT
	} break;
	default:
		Result = DefWindowProcA(Window, Message, WParam, LParam); // Let Windows do the default behaviour
		break;
	}
	return Result;
}

internal void Win32ClearBuffer(win32_sound_output *SoundOutput)
{
	VOID *Region1;
	DWORD Region1Size; // Byte sizes
	VOID *Region2;
	DWORD Region2Size;
	// Did the sound card get yanked
	if (SUCCEEDED(GlobalSecondaryBuffer->Lock(
		0,
		SoundOutput->SecondaryBufferSize, // Clear absolutely everything in the win32 sound buffer
		&Region1, &Region1Size,
		&Region2, &Region2Size,
		0))) {
		
		// Work in bytes now, not samples
		uint8 *DestSample = (uint8 *)Region1;
		for (DWORD ByteIndex = 0; ByteIndex < Region1Size; ++ByteIndex)
		{
			*DestSample++ = 0; // Clear manually instead of using memset (to call as few library functions as possible)
		}

		DestSample = (uint8 *)Region2;
		for (DWORD ByteIndex = 0; ByteIndex < Region2Size; ++ByteIndex)
		{
			*DestSample++ = 0;
		}

		GlobalSecondaryBuffer->Unlock(Region1, Region1Size, Region2, Region2Size);
	}
}

internal void Win32FillSoundBuffer(win32_sound_output *SoundOutput, DWORD ByteToLock, DWORD BytesToWrite, game_sound_output_buffer *SourceBuffer)
{
	VOID *Region1;
	DWORD Region1Size; // Byte sizes
	VOID *Region2;
	DWORD Region2Size;

	// Did the sound card get yanked
	if (SUCCEEDED(GlobalSecondaryBuffer->Lock(
		ByteToLock,
		BytesToWrite, // How far we would go to get to the play cursor
		&Region1, &Region1Size,
		&Region2, &Region2Size,
		0))) {

		// Assert that both region sizes are valid (even)

		DWORD Region1SampleCount = Region1Size / SoundOutput->BytesPerSample;
		int16 *DestSample = (int16 *)Region1;
		int16 *SourceSample = SourceBuffer->Samples; // Pull from the source buffer instead of sine wave

		for (DWORD SampleIndex = 0; SampleIndex < Region1SampleCount; ++SampleIndex)
		{
			// Copy from dest buffer to source buffer
			*DestSample++ = *SourceSample++;
			*DestSample++ = *SourceSample++;
			++SoundOutput->RunningSampleIndex;
		}

		// TODO(max): Collapse these loops
		DWORD Region2SampleCount = Region2Size / SoundOutput->BytesPerSample;
		DestSample = (int16 *)Region2;
		for (DWORD SampleIndex = 0; SampleIndex < Region2SampleCount; ++SampleIndex)
		{
			*DestSample++ = *SourceSample++;
			*DestSample++ = *SourceSample++;
			++SoundOutput->RunningSampleIndex;
		}

		// Unlock to prevent clicking when buffer loops
		GlobalSecondaryBuffer->Unlock(Region1, Region1Size, Region2, Region2Size);
	}
}

// Entry point for Windows
int CALLBACK WinMain(HINSTANCE Instance, HINSTANCE PrevInstance, LPSTR CommandLine, int ShowCode)
{
	LARGE_INTEGER PerfCountFrequencyResult;
	QueryPerformanceFrequency(&PerfCountFrequencyResult); // Fixed at system boot time, we only need to ask once
	int64 PerfCountFrequency = PerfCountFrequencyResult.QuadPart;

	// Load XInput from DLL manually
	Win32LoadXInput();

	WNDCLASSA WindowClass = {}; // Clear window initialization to 0

	// Resize our bitmap here instead of in WM_SIZE
	Win32ResizeDIBSection(&GlobalBackbuffer, 1280, 720);

	WindowClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC; // Redraw on horizontal or vertical scale. Get one device context and use it forever
	WindowClass.lpfnWndProc = Win32MainWindowCallback; // Pointer to our Windows event handler
	WindowClass.hInstance = Instance; // Handle to ourselves
	WindowClass.lpszClassName = "HandmadeHeroWindowClass";

	// Register window class for subsequent use
	if (RegisterClassA(&WindowClass))
	{
		HWND Window = CreateWindowExA(
			0, WindowClass.lpszClassName, "Handmade Hero",
			WS_OVERLAPPEDWINDOW | WS_VISIBLE, // Overlap of several parameters
			CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
			0, 0,
			Instance,
			0);
		if (Window)
		{
			HDC DeviceContext = GetDC(Window);

			// Graphics test
			int XOffset = 0;
			int YOffset = 0;

			win32_sound_output SoundOutput = {};
			SoundOutput.SamplesPerSecond = 48000;
			SoundOutput.ToneHz = 256; // 261Hz is middle C
			SoundOutput.ToneVolume = 3000;
			SoundOutput.WavePeriod = SoundOutput.SamplesPerSecond / SoundOutput.ToneHz; // How many samples do we need to fill to get 256Hz
			SoundOutput.BytesPerSample = sizeof(int16) * 2;
			SoundOutput.SecondaryBufferSize = SoundOutput.SamplesPerSecond * SoundOutput.BytesPerSample;
			SoundOutput.LatencySampleCount = SoundOutput.SamplesPerSecond / 15; // For low latency. If playing at 15fps or higher, don't get any dropouts
			// Fill sound buffer at startup
			Win32InitDSound(Window, SoundOutput.SamplesPerSecond, SoundOutput.SecondaryBufferSize);
			Win32ClearBuffer(&SoundOutput); // Flush it with zeros
			//Win32FillSoundBuffer(&SoundOutput, 0, SoundOutput.LatencySampleCount * SoundOutput.BytesPerSample);
			GlobalSecondaryBuffer->Play(0, 0, DSBPLAY_LOOPING); // We don't care about reserved or priority

			GlobalRunning = true;

			
			// TODO(max): Pool with bitmap VirtualAlloc
			// Allocate a backing store where we can copy sounds out to the ring buffer
			int16 *Samples = (int16 *)VirtualAlloc(0, SoundOutput.SecondaryBufferSize, 
													MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

			LARGE_INTEGER LastCounter; // Uses a union (multiple structs overlay some same space in memory) .QuadPart to access as 64bit, .LowPart+.HighPart to access as 32-bit
			QueryPerformanceCounter(&LastCounter);
			uint64 LastCycleCount = __rdtsc(); // Snap the RDTSC counter from the processor. An "intrinsic" for RDTSC

			// Pull messages off our queue
			while (GlobalRunning)
			{
				// GetMessage blocks graphics API, so use PeekMessage
				MSG Message;
				while (PeekMessage(&Message, 0, 0, 0, PM_REMOVE))
				{
					// Quit anytime we get a quit message
					if (Message.message == WM_QUIT) GlobalRunning = false;

					// Flush out our queue
					TranslateMessage(&Message); // Turn messages into proper keyboard messages
					DispatchMessageA(&Message); // Dispatch message to Windows to have them
				}

				// TODO(max): Should we poll this more frequently?
				for (DWORD ControllerIndex = 0; ControllerIndex < XUSER_MAX_COUNT; ++ControllerIndex)
				{
					XINPUT_STATE ControllerState;
					if (XInputGetState(ControllerIndex, &ControllerState) == ERROR_SUCCESS)
					{
						// This controller is plugged in
						XINPUT_GAMEPAD *Pad = &ControllerState.Gamepad;
						bool32 Up = (Pad->wButtons & XINPUT_GAMEPAD_DPAD_UP);
						bool32 Down = (Pad->wButtons & XINPUT_GAMEPAD_DPAD_DOWN);
						bool32 Left = (Pad->wButtons & XINPUT_GAMEPAD_DPAD_LEFT);
						bool32 Right = (Pad->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT);
						bool32 Start = (Pad->wButtons & XINPUT_GAMEPAD_START);
						bool32 LeftShoulder = (Pad->wButtons & XINPUT_GAMEPAD_BACK);
						bool32 RightShoulder = (Pad->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER);
						bool32 AButton = (Pad->wButtons & XINPUT_GAMEPAD_A);
						bool32 BButton = (Pad->wButtons & XINPUT_GAMEPAD_B);
						bool32 XButton = (Pad->wButtons & XINPUT_GAMEPAD_X);
						bool32 YButton = (Pad->wButtons & XINPUT_GAMEPAD_Y);
						int16 StickX = Pad->sThumbLX;
						int16 StickY = Pad->sThumbLY;

						XOffset += StickX / 4096;
						YOffset += StickY / 4096;
					}
					else
					{
						// The controller is not available
					}
				}

				// Lock DirectSound buffer at the write cursor
				// int16 int16  int16 int16 ...
				// [LEFT RIGHT] LEFT  RIGHT ...
				DWORD ByteToLock;
				DWORD TargetCursor;
				DWORD BytesToWrite; // Output number of samples we actually want for this frame
				DWORD PlayCursor;
				DWORD WriteCursor;
				bool32 SoundIsValid = false;
				if (SUCCEEDED(GlobalSecondaryBuffer->GetCurrentPosition(&PlayCursor, &WriteCursor))) {
					// Ignore the write cursor, because it may be garbage by the time we play it
					// Calcualte the byte lock and byte write instead
					ByteToLock = (SoundOutput.RunningSampleIndex * SoundOutput.BytesPerSample) % SoundOutput.SecondaryBufferSize; // % if it loops, return to the beginning
					TargetCursor = (PlayCursor + (SoundOutput.LatencySampleCount * SoundOutput.BytesPerSample)) % SoundOutput.SecondaryBufferSize; // Be 1/15th ahead of play cursor tat all times
					// If byte to lock was after the play cursor
					if (ByteToLock > TargetCursor)
					{
						// Case for region 1 (wrapping)
						BytesToWrite = (SoundOutput.SecondaryBufferSize - ByteToLock);
						BytesToWrite += TargetCursor;
					}
					else
					{
						// Case for region 2 (no wrapping)
						BytesToWrite = TargetCursor - ByteToLock;
					}
					
					SoundIsValid = true;
				}

				game_sound_output_buffer SoundBuffer = {};
				SoundBuffer.SamplesPerSecond = SoundOutput.SamplesPerSecond;
				SoundBuffer.SampleCount = BytesToWrite / SoundOutput.BytesPerSample;
				SoundBuffer.Samples = Samples;

				//RenderWeirdGradient(&GlobalBackbuffer, XOffset, YOffset);
				game_offscreen_buffer Buffer = {};
				Buffer.Memory = GlobalBackbuffer.Memory;
				Buffer.Width = GlobalBackbuffer.Width;
				Buffer.Height = GlobalBackbuffer.Height;
				Buffer.Pitch = GlobalBackbuffer.Pitch;
				GameUpdateAndRender(&Buffer, XOffset, YOffset, &SoundBuffer, SoundOutput.ToneHz);

				// DirectSound picks a point in the 2s buffer to write to
				// Region 1 is the actual location of the write cursor offset, to where it should end in the next 2s buffer
				// Region 2 is the start of the first 2s buffer, to where it should go.
				// 22220000001|111100000

				if (SoundIsValid)
				{
					// Fill sound buffer
					Win32FillSoundBuffer(&SoundOutput, ByteToLock, BytesToWrite, &SoundBuffer);
				}

				// Blit to screen
				win32_window_dimension Dimension = Win32GetWindowDimension(Window);
				Win32DisplayBufferInWindow(&GlobalBackbuffer, DeviceContext, Dimension.Width, Dimension.Height, 0, 0, Dimension.Width, Dimension.Height);
				
				// Increment offset
				++XOffset;

				uint64 EndCycleCount = __rdtsc();

				// Look at the clock
				LARGE_INTEGER EndCounter;
				QueryPerformanceCounter(&EndCounter);

				uint64 CyclesElapsed = EndCycleCount - LastCycleCount;
				int64 CounterElapsed = EndCounter.QuadPart - LastCounter.QuadPart; // Compute difference
				
				// real64s are free here, because sprintf_s upconverts real32s
				real64 MSPerFrame = (((1000.0f*(real64)CounterElapsed) / (real64)PerfCountFrequency)); // How many wall clock seconds actually elapsed
				real64 FPS = (real64)PerfCountFrequency / (real64)CounterElapsed;
				real64 MCPF = (real64)(CyclesElapsed / (1000.0f * 1000.0f)); // Printing out a 64-bit integer is relatively new

#if 0
				char Buffer[256];
				sprintf_s(Buffer, "%.02fms/f, %.02f FPS, %.02fM instructions/frame\n", MSPerFrame, FPS, MCPF);
				OutputDebugStringA(Buffer);
#endif
				LastCounter = EndCounter; // With QueryPerformanceCounter
				LastCycleCount = EndCycleCount; // With RDTSC
			}
		}
		else
		{
			// TODO(max): Logging
		}
	}
	else
	{
		// TODO(max): Logging
	}
	return 0;
}
