// JetpackFallBrake - F4SE plugin for Fallout 4 (1.11.x).
//
// Vanilla behaviour: the power armor jetpack integrates its own vertical speed every frame as
//   vz += (thrust - 100) * 6.8693 * dt
// (thrust 100 exactly cancels gravity). The strong "initial" thrust only happens once per takeoff, and the
// sustained thrust is 85 in vanilla and 100 with some mods, so firing the jetpack while falling never stops
// the fall.
//
// Starting speed: when the jetpack is fired in mid-air, the game copies the character's real vertical speed
// into the jetpack only if it is falling faster than 700 units/s. Otherwise the jetpack starts from 0 (first
// firing after landing) or from whatever speed it had when it was last released, so firing during a slower
// fall jumped straight up. That one check is replaced so that any downward speed is copied (bRealFallSpeed).
//
// Recovery: when the jetpack is fired while falling, the thrust follows a smooth curve towards the normal
// climb speed V:
//   wanted acceleration = fBrakeAccel * sat((V - vz) / fEaseSpeed),   sat(x) = x / (1 + x^4)^(1/4)
// so a fast fall is met with the full deceleration, which eases off smoothly as the fall slows, carries on
// through vz = 0 without a pause and fades out as vz nears V. The applied acceleration follows the wanted
// one with a short spool-up (fThrustRampTime) when it rises, and at once when it drops.
// V defaults to the speed the takeoff burst gives at 60 fps (about 361 units/s with Flying Jetpack). An initial
// thrust that lasts longer than 1 s counts only for its first second. With an initial thrust at or near 100 (a
// jetpack that only holds altitude) V is about 0, so the recovery stops the fall and the jetpack hovers. Once
// vz is within a few units/s of V, the rest is closed in one frame and the game's own thrust takes over,
// exactly as after a ground takeoff. Climbs that do not start from a fall (takeoffs, firing again while still
// rising) are never touched, so the game's initial and sustained thrust keep their meaning. During a recovery
// the thrust is never lower than the game's own value, except for the faded burst below.
// If this is the first firing after landing, the game's takeoff burst is still pending. The part of it above
// the thrust the game is left with after the burst is faded out by the fall speed at firing: at a fall speed
// near 0 it works in full (so firing just before or just after the top of a jump feels the same), and by a
// fall speed of V it is gone and only the curve acts, so a fast fall is not met by a sudden kick. During the
// burst the larger of the two accelerations is used.
//
// Fall damage: the game takes it from the drop between the recorded fall start height and the landing
// height, not from the landing speed, and the jetpack never lowers that record when it slows a fall. A second
// hook keeps the record at "current height + the height a free fall needs to reach the current speed" while
// the jetpack fires downwards (bFallDamageFromSpeed), so a soft landing after braking does no damage.
//
// The code is found by byte pattern (no hardcoded addresses). If the pattern is not found exactly once,
// nothing is patched and the reason is logged.

#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <share.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
	struct F4SEInterface
	{
		std::uint32_t f4seVersion;
		std::uint32_t runtimeVersion;
		std::uint32_t editorVersion;
		std::uint32_t isEditor;
	};

	FILE* g_log = nullptr;

	struct Config
	{
		float climbSpeed = -1.0f;   // target climb speed (units/s); <= 0: speed of a ground takeoff
		float brakeAccel = 3000.0f; // strongest acceleration (units/s^2; gravity is about 687)
		float easeSpeed = 1500.0f;  // speed range (units/s) over which the acceleration fades; larger = gentler
		float rampTime = 0.1f;      // spool-up time constant (s) when the acceleration rises
	} g_cfg;

	// assist state: whether a recovery from a fall is running, the acceleration applied last frame, and when
	// that was (to tell a new activation)
	bool g_recovering = false;
	float g_burstWeight = 1.0f;  // share of the pending takeoff burst kept during a recovery (1 = full)
	float g_assistAccel = 0.0f;
	LARGE_INTEGER g_lastCall{};
	double g_qpcPerSecond = 0.0;

	// game settings and the thrust-to-acceleration factor, resolved from the code at install time
	const float* g_timeToSustained = nullptr;
	const float* g_thrustInitial = nullptr;
	const float* g_thrustSustained = nullptr;
	float g_accelPerThrust = 6.869333744f;
	bool g_loggedFirstUse = false;

	constexpr float kHoverThrust = 100.0f;  // thrust that exactly cancels gravity
	constexpr double kNewActivationGap = 0.25;  // seconds without a call = the jetpack was released
	constexpr float kRecoveryEndBand = 5.0f;    // a recovery ends once vz is this close to the climb speed
	constexpr float kMaxBurstTime = 1.0f;       // a longer initial thrust counts as the takeoff burst only this long

	void Log(const char* fmt, ...)
	{
		if (!g_log) {
			return;
		}
		va_list args;
		va_start(args, fmt);
		std::vfprintf(g_log, fmt, args);
		va_end(args);
		std::fputc('\n', g_log);
		std::fflush(g_log);
	}

	void OpenLog()
	{
		PWSTR docs = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
			std::wstring path = std::wstring(docs) + L"\\My Games\\Fallout4\\F4SE";
			CreateDirectoryW(path.c_str(), nullptr);
			path += L"\\JetpackFallBrake.log";
			g_log = _wfsopen(path.c_str(), L"w", _SH_DENYWR);  // readable while the game runs
		}
		CoTaskMemFree(docs);
	}

	std::wstring IniPath()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&IniPath), &self);
		wchar_t buf[MAX_PATH]{};
		GetModuleFileNameW(self, buf, MAX_PATH);
		std::wstring p(buf);
		const auto slash = p.find_last_of(L"\\/");
		return p.substr(0, slash + 1) + L"JetpackFallBrake.ini";
	}

	// pattern: -1 = wildcard
	std::vector<std::uintptr_t> FindAll(std::uintptr_t begin, std::size_t size, const std::vector<int>& pat)
	{
		std::vector<std::uintptr_t> hits;
		const auto* p = reinterpret_cast<const std::uint8_t*>(begin);
		for (std::size_t i = 0; i + pat.size() <= size; ++i) {
			bool ok = true;
			for (std::size_t j = 0; j < pat.size(); ++j) {
				if (pat[j] >= 0 && p[i + j] != static_cast<std::uint8_t>(pat[j])) {
					ok = false;
					break;
				}
			}
			if (ok) {
				hits.push_back(begin + i);
			}
		}
		return hits;
	}

	bool Section(const char* name, std::uintptr_t& begin, std::size_t& size)
	{
		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto* sec = IMAGE_FIRST_SECTION(nt);
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
			if (std::strncmp(reinterpret_cast<const char*>(sec->Name), name, 8) == 0) {
				begin = base + sec->VirtualAddress;
				size = sec->Misc.VirtualSize;
				return true;
			}
		}
		return false;
	}

	void* AllocNear(std::uintptr_t target)
	{
		SYSTEM_INFO si{};
		GetSystemInfo(&si);
		const std::uintptr_t gran = si.dwAllocationGranularity;
		const std::uintptr_t lo = target > 0x7FF00000 ? target - 0x7FF00000 : gran;
		const std::uintptr_t hi = target + 0x7FF00000;
		for (int dir = 0; dir < 2; ++dir) {
			std::uintptr_t a = (target & ~(gran - 1));
			while (dir == 0 ? a > lo : a < hi) {
				a = dir == 0 ? a - gran : a + gran;
				MEMORY_BASIC_INFORMATION mbi{};
				if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) {
					continue;
				}
				if (mbi.State == MEM_FREE) {
					if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
						return p;
					}
				}
			}
		}
		return nullptr;
	}

	struct Emitter
	{
		std::uint8_t* p;
		void bytes(std::initializer_list<std::uint8_t> b) { for (auto x : b) *p++ = x; }
		void u32(std::uint32_t v) { std::memcpy(p, &v, 4); p += 4; }
		void u64(std::uint64_t v) { std::memcpy(p, &v, 8); p += 8; }
		void jmpAbs(std::uintptr_t to) { bytes({ 0xFF, 0x25, 0, 0, 0, 0 }); u64(to); }

		// Save the volatile registers and leave rsp aligned for a call. frame = 0x88 when entered with rsp
		// 16-byte aligned (inside a function body), 0x80 when entered at a function's first instruction.
		void pushVolatile(std::uint32_t frame = 0x88)
		{
			bytes({ 0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53 });  // push rax rcx rdx r8-r11
			bytes({ 0x48, 0x81, 0xEC }); u32(frame);                                   // sub rsp, frame
			for (std::uint8_t r = 0; r < 6; ++r) {                                     // movups [rsp+0x20+16r], xmmr
				bytes({ 0x0F, 0x11, static_cast<std::uint8_t>(0x44 | (r << 3)), 0x24, static_cast<std::uint8_t>(0x20 + 16 * r) });
			}
		}

		void popVolatile(std::uint32_t frame = 0x88)
		{
			for (std::uint8_t r = 0; r < 6; ++r) {                                     // movups xmmr, [rsp+0x20+16r]
				bytes({ 0x0F, 0x10, static_cast<std::uint8_t>(0x44 | (r << 3)), 0x24, static_cast<std::uint8_t>(0x20 + 16 * r) });
			}
			bytes({ 0x48, 0x81, 0xC4 }); u32(frame);                                   // add rsp, frame
			bytes({ 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58 });  // pop r11-r8 rdx rcx rax
		}

		void callAbs(const void* fn)
		{
			bytes({ 0x48, 0xB8 }); u64(reinterpret_cast<std::uint64_t>(fn));            // mov rax, fn
			bytes({ 0xFF, 0xD0 });                                                     // call rax
		}
	};

	void WriteJump(std::uintptr_t site, std::size_t length, const void* to)
	{
		DWORD old = 0;
		VirtualProtect(reinterpret_cast<void*>(site), length, PAGE_EXECUTE_READWRITE, &old);
		auto* h = reinterpret_cast<std::uint8_t*>(site);
		h[0] = 0xE9;
		const auto rel = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(to) - static_cast<std::intptr_t>(site + 5));
		std::memcpy(h + 1, &rel, 4);
		std::memset(h + 5, 0x90, length - 5);
		VirtualProtect(reinterpret_cast<void*>(site), length, old, &old);
		FlushInstructionCache(GetCurrentProcess(), h, length);
	}

	// The game setting "fInAirFallingCharGravityMult:Havok" (falling characters get this much more gravity).
	const float* FindFallingGravityMult()
	{
		static const char kName[] = "fInAirFallingCharGravityMult:Havok";
		std::uintptr_t rdata = 0, data = 0;
		std::size_t rdataSize = 0, dataSize = 0;
		if (!Section(".rdata", rdata, rdataSize) || !Section(".data", data, dataSize)) {
			return nullptr;
		}
		const auto* r = reinterpret_cast<const char*>(rdata);
		for (std::size_t i = 0; i + sizeof(kName) <= rdataSize; ++i) {
			if (std::memcmp(r + i, kName, sizeof(kName)) != 0) {
				continue;
			}
			const auto str = reinterpret_cast<std::uintptr_t>(r + i);
			for (std::size_t j = 0x10; j + 8 <= dataSize; j += 8) {
				if (*reinterpret_cast<const std::uintptr_t*>(data + j) == str) {
					return reinterpret_cast<const float*>(data + j - 0x10 + 8);  // Setting: value at +8, name at +0x10
				}
			}
		}
		return nullptr;
	}

	// Smooth saturation: linear near 0, levels off at 1.
	float Sat(float x)
	{
		const float x2 = x * x;
		return x / std::sqrt(std::sqrt(1.0f + x2 * x2));
	}

	// 0 -> 1 with zero slope at both ends.
	float SmoothStep(float x)
	{
		x = std::clamp(x, 0.0f, 1.0f);
		return x * x * (3.0f - 2.0f * x);
	}

	bool NewActivation()
	{
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		const bool gap = g_qpcPerSecond <= 0.0 ||
			static_cast<double>(now.QuadPart - g_lastCall.QuadPart) / g_qpcPerSecond > kNewActivationGap;
		g_lastCall = now;
		return gap;
	}

	// Vertical speed a ground takeoff gains from the burst at 60 fps: the frames of the first kMaxBurstTime in
	// which the initial/sustained blend pushes upwards (thrust above 100). The game samples the blend at the
	// start of each frame, so for a short burst this is a little more than the continuous T * ((I + S) / 2 - 100).
	// About 0 when the initial thrust is at or near 100 (a jetpack that only holds altitude).
	float TakeoffClimbSpeed(float timeToSustained, float initial, float sustained)
	{
		constexpr float kFrame = 1.0f / 60.0f;
		if (!(timeToSustained > 0.0f) || !(initial > kHoverThrust)) {
			return 0.0f;
		}
		float n = std::max(1.0f, std::ceil(std::min(timeToSustained, kMaxBurstTime) / kFrame - 1e-3f));
		if (initial > sustained) {
			// the blend falls to 100 after this many frames
			n = std::min(n, std::ceil((initial - kHoverThrust) * timeToSustained / ((initial - sustained) * kFrame)));
		}
		return std::max(0.0f, g_accelPerThrust * kFrame *
			(n * (initial - kHoverThrust) - (initial - sustained) * (kFrame / timeToSustained) * n * (n - 1.0f) * 0.5f));
	}

	// Called from the hook every frame the jetpack fires, before "elapsed += dt" (the hook reloads elapsed
	// afterwards, so this may change it). Returns the thrust to use for this frame.
	float JetpackThrust(std::uint8_t* jetpack, float thrust, float elapsed, float dt)
	{
		const bool newActivation = NewActivation();
		if (newActivation) {
			g_assistAccel = 0.0f;      // the engine spools up again
			g_recovering = false;      // decided below from the speed at this firing
		}
		const float vz = *reinterpret_cast<const float*>(jetpack + 0xD0);
		const float timeToSustained = *g_timeToSustained;
		const float initial = *g_thrustInitial;
		const float sustained = *g_thrustSustained;
		const float climbSpeed = g_cfg.climbSpeed > 0.0f
			? g_cfg.climbSpeed
			: TakeoffClimbSpeed(timeToSustained, initial, sustained);
		// A recovery starts only when the jetpack is fired while falling. Everything else (a takeoff, firing
		// again while still rising, the climb after a recovery) is the game's own.
		if (newActivation) {
			g_recovering = std::isfinite(vz) && vz < 0.0f;
			g_burstWeight = climbSpeed > 0.0f ? 1.0f - SmoothStep(-vz / climbSpeed) : 0.0f;
		}
		if (!(dt > 0.0f)) {
			return thrust;
		}
		// The first firing after landing runs the game's takeoff burst while elapsed < time to sustained. A
		// longer initial thrust counts as the burst only for its first kMaxBurstTime.
		const float burstTime = std::min(timeToSustained, kMaxBurstTime);
		const bool inBurst = elapsed < burstTime;
		if (g_recovering && !inBurst && (!std::isfinite(vz) || !(vz < climbSpeed - kRecoveryEndBand))) {
			// reached the climb speed: close the last few units/s in this frame and hand over to the game
			g_recovering = false;
			g_assistAccel = 0.0f;
			return std::isfinite(vz) && vz < climbSpeed
				? std::max(thrust, kHoverThrust + (climbSpeed - vz) / dt / g_accelPerThrust)
				: thrust;
		}
		if (!g_recovering) {
			g_assistAccel = 0.0f;
			return thrust;
		}

		const float wanted = g_cfg.brakeAccel * Sat((climbSpeed - vz) / g_cfg.easeSpeed);
		float accel = wanted <= g_assistAccel
			? wanted                                                                    // throttle down at once
			: g_assistAccel + (wanted - g_assistAccel) * (1.0f - std::exp(-dt / g_cfg.rampTime));  // spool up
		accel = std::max(0.0f, std::min(accel, (climbSpeed - vz) / dt));  // never overshoot the climb speed
		g_assistAccel = accel;

		if (!g_loggedFirstUse) {
			g_loggedFirstUse = true;
			Log("first recovery: vz %.0f, climb speed %.0f (initial %.1f, sustained %.1f, time %g)%s",
				vz, climbSpeed, initial, sustained, timeToSustained,
				inBurst ? ", takeoff burst faded by the fall speed" : "");
		}
		if (inBurst) {
			// The game's burst or the curve, whichever pushes harder. The part of the burst above the thrust the
			// game is left with after it (100 at least) is faded by the fall speed at firing; the rest is kept.
			const float settled = initial + (sustained - initial) * (burstTime / timeToSustained);
			const float kept = std::max(kHoverThrust, std::min(thrust, settled));
			const float burstAccel =
				((kept - kHoverThrust) + g_burstWeight * std::max(0.0f, thrust - kept)) * g_accelPerThrust;
			return kHoverThrust + std::max(burstAccel, accel) / g_accelPerThrust;
		}
		return std::max(thrust, kHoverThrust + accel / g_accelPerThrust);
	}

	// Starting speed of a mid-air firing. The game's check (Fallout4.exe+C5BE80) is "character in the air and
	// falling faster than 700 units/s"; only then does the jetpack start from the real vertical speed. This copy
	// of it, used only by the jetpack start, accepts any downward speed.
	using GetCharControllerFn = void* (*)(void* process);
	using GetCharStateFn = std::int32_t (*)(void* stateContext);
	GetCharControllerFn g_getCharController = nullptr;
	GetCharStateFn g_getCharState = nullptr;
	std::uint32_t g_velocityZMask[4]{};  // selects the z lane of the controller's velocity
	const float* g_unitsPerHavok = nullptr;
	const float* g_gameFallThreshold = nullptr;
	bool g_loggedSlowFallStart = false;

	bool JetpackStartsFromFall(std::uint8_t* actor)
	{
		auto* process = actor ? *reinterpret_cast<void**>(actor + 0x300) : nullptr;
		auto* controller = process ? static_cast<std::uint8_t*>(g_getCharController(process)) : nullptr;
		if (!controller || g_getCharState(controller + 0x2A0) != 2) {  // 2 = in the air
			return false;
		}
		std::uint32_t lanes[4];
		std::memcpy(lanes, controller + 0x160, sizeof(lanes));
		std::uint32_t bits = 0;
		for (int i = 0; i < 4; ++i) {
			bits |= lanes[i] & g_velocityZMask[i];
		}
		float vz = 0.0f;
		std::memcpy(&vz, &bits, 4);
		vz *= *g_unitsPerHavok;
		if (!(vz < 0.0f)) {
			return false;
		}
		if (!g_loggedSlowFallStart && vz > -*g_gameFallThreshold) {
			g_loggedSlowFallStart = true;
			Log("first jetpack start from a slow fall: vz %.0f (the game copies it only below -%.0f)", vz,
				*g_gameFallThreshold);
		}
		return true;
	}

	// Fall damage is worked out from [controller+0x328], the height where the fall started, minus the height
	// at landing. The game never lowers that height when the jetpack slows a fall, so a soft landing after
	// braking still counts the whole drop. While the jetpack fires downwards, this sets it to the height a free
	// fall would need to reach the current speed, so the damage follows the real landing speed. While rising it
	// is left alone, as in the game (1.3.0 also set it while rising, which the game does not expect).
	using GetPositionFn = void (*)(void* controller, float* out);
	GetPositionFn g_getPosition = nullptr;
	const float* g_fallingGravityMult = nullptr;
	bool g_fallFromSpeed = true;

	void FallStartHook(std::uint8_t* controller, const std::uint8_t* jetpack)
	{
		float pos[3]{};
		g_getPosition(controller, pos);
		const float vz = *reinterpret_cast<const float*>(jetpack + 0xD0);
		float& fallStart = *reinterpret_cast<float*>(controller + 0x328);
		// Rising: leave the record alone, as the game does (the jetpack stop sets it to the release height).
		if (!(vz < 0.0f) || !std::isfinite(vz) || !std::isfinite(pos[2])) {
			return;
		}
		if (!g_fallFromSpeed) {
			// the game's own rule: start a fall record when descending above the current record
			if (fallStart == 0.0f || fallStart <= pos[2]) {
				fallStart = pos[2];
			}
			return;
		}
		// Descending: the height a free fall (with the stronger falling gravity) needs to reach this speed.
		const float gravity = kHoverThrust * g_accelPerThrust;  // thrust 100 cancels it exactly
		const float fallMult = g_fallingGravityMult ? std::clamp(*g_fallingGravityMult, 0.1f, 10.0f) : 1.85f;
		float start = pos[2] + vz * vz / (2.0f * gravity * fallMult);
		if (start == 0.0f) {
			start = 0.001f;  // 0 means "not falling" to the game
		}
		fallStart = start;
	}

	float IniFloat(const std::wstring& ini, const wchar_t* key, float def)
	{
		wchar_t buf[64]{};
		GetPrivateProfileStringW(L"General", key, L"", buf, 64, ini.c_str());
		if (!buf[0]) {
			return def;
		}
		const float v = static_cast<float>(_wtof(buf));
		return std::isfinite(v) ? v : def;
	}

	const float* RipTarget(std::uintptr_t instr, std::size_t dispOffset, std::size_t length)
	{
		std::int32_t disp = 0;
		std::memcpy(&disp, reinterpret_cast<const void*>(instr + dispOffset), 4);
		return reinterpret_cast<const float*>(instr + length + disp);
	}

	void Install()
	{
		const std::wstring ini = IniPath();
		const bool enabled = GetPrivateProfileIntW(L"General", L"bEnabled", 1, ini.c_str()) != 0;
		g_cfg.climbSpeed = IniFloat(ini, L"fClimbSpeed", g_cfg.climbSpeed);
		g_cfg.brakeAccel = std::max(1.0f, IniFloat(ini, L"fBrakeAccel", g_cfg.brakeAccel));
		g_cfg.easeSpeed = std::max(1.0f, IniFloat(ini, L"fEaseSpeed", g_cfg.easeSpeed));
		g_cfg.rampTime = std::max(0.001f, IniFloat(ini, L"fThrustRampTime", g_cfg.rampTime));
		g_fallFromSpeed = GetPrivateProfileIntW(L"General", L"bFallDamageFromSpeed", 1, ini.c_str()) != 0;
		LARGE_INTEGER freq{};
		QueryPerformanceFrequency(&freq);
		g_qpcPerSecond = static_cast<double>(freq.QuadPart);
		if (!enabled) {
			Log("bEnabled=0: nothing installed");
			return;
		}

		std::uintptr_t text = 0;
		std::size_t textSize = 0;
		if (!Section(".text", text, textSize)) {
			Log("no .text section");
			return;
		}

		// jetpack update: thrust selection (initial/sustained blend) followed by "elapsed += dt; store elapsed"
		const std::vector<int> pat = {
			0xF3, 0x0F, 0x10, 0x05, -1, -1, -1, -1,          // movss xmm0, [fJetpackTimeToSustained]
			0x0F, 0x2F, 0xC8,                                // comiss xmm1, xmm0
			0x73, 0x2C,                                      // jae sustained
			0x0F, 0x28, 0xD0,                                // movaps xmm2, xmm0
			0x0F, 0x28, 0xF7,                                // movaps xmm6, xmm7
			0xF3, 0x0F, 0x5C, 0xD1,                          // subss xmm2, xmm1
			0xF3, 0x0F, 0x5E, 0xD0,                          // divss xmm2, xmm0
			0xF3, 0x0F, 0x10, 0x05, -1, -1, -1, -1,          // movss xmm0, [fJetpackThrustInitial]   (+0x1B)
			0xF3, 0x0F, 0x5C, 0xF2,                          // subss xmm6, xmm2
			0xF3, 0x0F, 0x59, 0xC2,                          // mulss xmm0, xmm2
			0xF3, 0x0F, 0x59, 0x35, -1, -1, -1, -1,          // mulss xmm6, [fJetpackThrustSustained]
			0xF3, 0x0F, 0x58, 0xF0,                          // addss xmm6, xmm0
			0xEB, 0x08,                                      // jmp hook
			0xF3, 0x0F, 0x10, 0x35, -1, -1, -1, -1,          // movss xmm6, [fJetpackThrustSustained]
			0xF3, 0x0F, 0x58, 0x4D, 0x00,                    // hook (+0x41): addss xmm1, [rbp]   (elapsed += dt)
			0xF3, 0x0F, 0x11, 0x8F, 0xD4, 0x00, 0x00, 0x00,  //            movss [rdi+0xD4], xmm1
		};
		const auto hits = FindAll(text, textSize, pat);
		if (hits.size() != 1) {
			Log("jetpack thrust code: expected 1 match, found %zu. Nothing patched.", hits.size());
			return;
		}
		const std::uintptr_t start = hits[0];
		const std::uintptr_t hook = start + 0x41;
		const std::uintptr_t resume = hook + 13;

		// the same function must integrate the jetpack's vertical speed at [rdi+0xD0]:
		//   vz += thrust_minus_100 * accelPerThrust * dt
		const std::vector<int> integ = {
			0xF3, 0x0F, 0x59, 0x35, -1, -1, -1, -1,          // mulss xmm6, [accelPerThrust]
			0xF3, 0x0F, 0x59, 0x75, 0x00,                    // mulss xmm6, [rbp]          (dt)
			0xF3, 0x0F, 0x58, 0xB7, 0xD0, 0x00, 0x00, 0x00,  // addss xmm6, [rdi+0xD0]
			0xF3, 0x0F, 0x11, 0xB7, 0xD0, 0x00, 0x00, 0x00,  // movss [rdi+0xD0], xmm6
		};
		const auto integHits = FindAll(hook, 0x300, integ);
		if (integHits.size() != 1) {
			Log("vertical speed field [+0xD0] not confirmed near the hook. Nothing patched.");
			return;
		}

		g_timeToSustained = RipTarget(start, 4, 8);
		g_thrustInitial = RipTarget(start + 0x1B, 4, 8);
		g_thrustSustained = RipTarget(start + 0x39, 4, 8);
		if (RipTarget(start + 0x2B, 4, 8) != g_thrustSustained) {
			Log("sustained thrust setting not confirmed. Nothing patched.");
			return;
		}
		const float accelPerThrust = *RipTarget(integHits[0], 4, 8);
		if (!(accelPerThrust > 1.0f && accelPerThrust < 50.0f)) {
			Log("unexpected thrust-to-acceleration factor %f. Nothing patched.", accelPerThrust);
			return;
		}
		g_accelPerThrust = accelPerThrust;

		auto* stub = static_cast<std::uint8_t*>(AllocNear(hook));
		if (!stub) {
			Log("could not allocate trampoline memory. Nothing patched.");
			return;
		}

		// At the hook: rdi = jetpack, rbp -> dt, xmm1 = elapsed (before += dt), xmm6 = thrust.
		// xmm1 and xmm2 are still used after the hook; rsp is 16-byte aligned here.
		Emitter e{ stub };
		e.pushVolatile();
		e.bytes({ 0x48, 0x89, 0xF9 });                                // mov rcx, rdi        (jetpack)
		e.bytes({ 0x0F, 0x28, 0xD1 });                                // movaps xmm2, xmm1   (elapsed)
		e.bytes({ 0x0F, 0x28, 0xCE });                                // movaps xmm1, xmm6   (thrust)
		e.bytes({ 0xF3, 0x0F, 0x10, 0x5D, 0x00 });                    // movss xmm3, [rbp]   (dt)
		e.callAbs(reinterpret_cast<const void*>(&JetpackThrust));
		e.bytes({ 0x0F, 0x28, 0xF0 });                                // movaps xmm6, xmm0   (new thrust)
		e.popVolatile();
		e.bytes({ 0xF3, 0x0F, 0x10, 0x8F, 0xD4, 0x00, 0x00, 0x00 });  // movss xmm1, [rdi+0xD4] (elapsed, may have changed)
		e.bytes({ 0xF3, 0x0F, 0x58, 0x4D, 0x00 });                    // addss xmm1, [rbp]   (original)
		e.bytes({ 0xF3, 0x0F, 0x11, 0x8F, 0xD4, 0x00, 0x00, 0x00 });  // movss [rdi+0xD4], xmm1
		e.jmpAbs(resume);
		FlushInstructionCache(GetCurrentProcess(), stub, 0x200);
		WriteJump(hook, 13, stub);

		const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		Log("patched jetpack update at Fallout4.exe+%llX (stub at %p)",
			static_cast<unsigned long long>(hook - base), stub);

		// Second hook, later in the same function: the block that records where a fall starts
		//   if (vz < 0 && GetFallDistance(controller) <= 0) controller->fallStartZ = GetPosition(controller).z;
		// is replaced by FallStartHook (see above).
		const std::vector<int> fallPat = {
			0x44, 0x0F, 0x2F, 0x97, 0xD0, 0x00, 0x00, 0x00,  // comiss xmm10, [rdi+0xD0]        (0 vs vz)
			0x76, 0x37,                                      // jbe done
			0x48, 0x8B, 0xCB,                                // mov rcx, rbx                   (controller)
			0xE8, -1, -1, -1, -1,                            // call GetFallDistance
			0x41, 0x0F, 0x2F, 0xC2,                          // comiss xmm0, xmm10
			0x77, 0x29,                                      // ja done
			0xF3, 0x0F, 0x10, 0x05, -1, -1, -1, -1,          // movss xmm0, [0.0]
			0x48, 0x8D, 0x54, 0x24, 0x20,                    // lea rdx, [rsp+0x20]
			0x48, 0x8B, 0xCB,                                // mov rcx, rbx
			0xF3, 0x0F, 0x11, 0x44, 0x24, 0x28,              // movss [rsp+0x28], xmm0
			0xE8, -1, -1, -1, -1,                            // call GetPosition                (+0x2E)
			0xF3, 0x0F, 0x10, 0x44, 0x24, 0x28,              // movss xmm0, [rsp+0x28]         (z)
			0xF3, 0x0F, 0x11, 0x83, 0x28, 0x03, 0x00, 0x00,  // movss [rbx+0x328], xmm0        (fall start)
		};                                                   // done: (+0x41)
		const auto fallHits = FindAll(hook, 0x400, fallPat);
		if (fallHits.size() != 1) {
			Log("fall start code: expected 1 match, found %zu. Fall damage left to the game.", fallHits.size());
		} else {
			const std::uintptr_t fallSite = fallHits[0];
			std::int32_t rel = 0;
			std::memcpy(&rel, reinterpret_cast<const void*>(fallSite + 0x2F), 4);
			g_getPosition = reinterpret_cast<GetPositionFn>(fallSite + 0x33 + rel);
			g_fallingGravityMult = FindFallingGravityMult();

			// At the site: rbx = character controller, rdi = jetpack, rsp 16-byte aligned.
			Emitter f{ stub + 0x200 };
			f.pushVolatile();
			f.bytes({ 0x48, 0x8B, 0xCB });  // mov rcx, rbx (controller)
			f.bytes({ 0x48, 0x8B, 0xD7 });  // mov rdx, rdi (jetpack)
			f.callAbs(reinterpret_cast<const void*>(&FallStartHook));
			f.popVolatile();
			f.jmpAbs(fallSite + 0x41);
			FlushInstructionCache(GetCurrentProcess(), stub + 0x200, 0x100);
			WriteJump(fallSite, 10, stub + 0x200);
			Log("patched fall start at Fallout4.exe+%llX; falling gravity mult %s %.2f; fall damage %s",
				static_cast<unsigned long long>(fallSite - base),
				g_fallingGravityMult ? "(game setting)" : "(not found, default)",
				g_fallingGravityMult ? *g_fallingGravityMult : 1.85f,
				g_fallFromSpeed ? "from landing speed" : "game rule");
		}

		// Jetpack start: "call IsFallingFast(actor)" decides whether the jetpack starts from the character's real
		// vertical speed. Retarget that one call (the check itself has other callers that stay unchanged).
		const std::vector<int> startPat = {
			0x48, 0x8B, 0xCF,                                // mov rcx, rdi              (actor)
			0x4C, 0x89, 0xBC, 0x24, 0xA0, 0x00, 0x00, 0x00,  // mov [rsp+0xA0], r15
			0x0F, 0x29, 0xB4, 0x24, 0x90, 0x00, 0x00, 0x00,  // movaps [rsp+0x90], xmm6
			0xE8, -1, -1, -1, -1,                            // call IsFallingFast        (+0x13)
			0x45, 0x33, 0xFF,                                // xor r15d, r15d
			0x0F, 0x57, 0xF6,                                // xorps xmm6, xmm6
			0x84, 0xC0,                                      // test al, al
			0x74, -1,                                        // je no_copy
			0x0F, 0x28, 0x05, -1, -1, -1, -1,                // movaps xmm0, [z mask]
			0x0F, 0x54, 0x83, 0x60, 0x01, 0x00, 0x00,        // andps xmm0, [rbx+0x160]  (controller velocity)
		};
		const std::vector<int> checkPat = {
			0x48, 0x83, 0xEC, 0x28,                          // sub rsp, 0x28
			0x48, 0x8B, 0x89, 0x00, 0x03, 0x00, 0x00,        // mov rcx, [rcx+0x300]      (process)
			0x48, 0x85, 0xC9, 0x74, -1,                      // test rcx, rcx; je
			0x48, 0x89, 0x5C, 0x24, 0x20,                    // mov [rsp+0x20], rbx
			0xE8, -1, -1, -1, -1,                            // call GetCharController    (+0x15)
			0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x74, -1,    // mov rbx, rax; test; je
			0x48, 0x8D, 0x88, 0xA0, 0x02, 0x00, 0x00,        // lea rcx, [rax+0x2A0]
			0xE8, -1, -1, -1, -1,                            // call GetState             (+0x29)
			0x83, 0xF8, 0x02, 0x75, -1,                      // cmp eax, 2; jne
			0x0F, 0x28, 0x05, -1, -1, -1, -1,                // movaps xmm0, [z mask]     (+0x33)
			0x0F, 0x54, 0x83, 0x60, 0x01, 0x00, 0x00,        // andps xmm0, [rbx+0x160]
			0x48, 0x8B, 0x5C, 0x24, 0x20,                    // mov rbx, [rsp+0x20]
			0x0F, 0x28, 0xC8, 0x0F, 0xC6, 0xC8, 0x4E, 0x0F, 0x56, 0xC8,
			0xF3, 0x0F, 0x10, 0x05, -1, -1, -1, -1,          // movss xmm0, [700.0]       (+0x50)
			0x0F, 0x57, 0x05, -1, -1, -1, -1,                // xorps xmm0, [sign]        (-700)
			0x0F, 0x28, 0xD1, 0x0F, 0xC6, 0xD1, 0xB1, 0x0F, 0x56, 0xD1,
			0xF3, 0x0F, 0x59, 0x15, -1, -1, -1, -1,          // mulss xmm2, [units per havok unit] (+0x69)
			0x0F, 0x2F, 0xC2, 0x0F, 0x97, 0xC0,              // comiss xmm0, xmm2; seta al
		};
		const bool realFallSpeed = GetPrivateProfileIntW(L"General", L"bRealFallSpeed", 1, ini.c_str()) != 0;
		const auto startHits = FindAll(text, textSize, startPat);
		if (!realFallSpeed) {
			Log("bRealFallSpeed=0: jetpack start speed left to the game");
		} else if (startHits.size() != 1) {
			Log("jetpack start code: expected 1 match, found %zu. Start speed left to the game.", startHits.size());
		} else {
			const std::uintptr_t site = startHits[0] + 0x13;
			std::int32_t rel = 0;
			std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), 4);
			const std::uintptr_t check = site + 5 + rel;
			if (FindAll(check, checkPat.size(), checkPat).size() != 1) {
				Log("jetpack start check at Fallout4.exe+%llX not recognised. Start speed left to the game.",
					static_cast<unsigned long long>(check - base));
			} else {
				std::memcpy(&rel, reinterpret_cast<const void*>(check + 0x16), 4);
				g_getCharController = reinterpret_cast<GetCharControllerFn>(check + 0x1A + rel);
				std::memcpy(&rel, reinterpret_cast<const void*>(check + 0x2A), 4);
				g_getCharState = reinterpret_cast<GetCharStateFn>(check + 0x2E + rel);
				std::memcpy(g_velocityZMask, RipTarget(check + 0x33, 3, 7), sizeof(g_velocityZMask));
				g_gameFallThreshold = RipTarget(check + 0x50, 4, 8);
				g_unitsPerHavok = RipTarget(check + 0x69, 4, 8);

				Emitter s{ stub + 0x600 };
				s.bytes({ 0x48, 0x83, 0xEC, 0x28 });  // sub rsp, 0x28   (entered by a call)
				s.callAbs(reinterpret_cast<const void*>(&JetpackStartsFromFall));  // rcx = actor, unchanged
				s.bytes({ 0x48, 0x83, 0xC4, 0x28 });  // add rsp, 0x28
				s.bytes({ 0xC3 });                    // ret             (al = start from the real speed)
				FlushInstructionCache(GetCurrentProcess(), stub + 0x600, 0x40);

				DWORD old = 0;
				VirtualProtect(reinterpret_cast<void*>(site), 5, PAGE_EXECUTE_READWRITE, &old);
				const auto callRel = static_cast<std::int32_t>(reinterpret_cast<std::intptr_t>(stub + 0x600) - static_cast<std::intptr_t>(site + 5));
				std::memcpy(reinterpret_cast<void*>(site + 1), &callRel, 4);  // keep the E8, retarget the call
				VirtualProtect(reinterpret_cast<void*>(site), 5, old, &old);
				FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 5);
				Log("patched jetpack start at Fallout4.exe+%llX: any fall speed is kept (the game: faster than %.0f only, units per havok unit %.2f)",
					static_cast<unsigned long long>(site - base), *g_gameFallThreshold, *g_unitsPerHavok);
			}
		}

		Log("settings: time Fallout4.exe+%llX, initial +%llX, sustained +%llX, accel per thrust %.4f",
			static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_timeToSustained) - base),
			static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_thrustInitial) - base),
			static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_thrustSustained) - base),
			g_accelPerThrust);
		if (g_cfg.climbSpeed > 0.0f) {
			Log("climb speed %.0f (from INI)", g_cfg.climbSpeed);
		} else {
			Log("climb speed: same as a ground takeoff (from the game settings)");
		}
		Log("max accel %.0f, ease speed %.0f, thrust ramp %.2fs", g_cfg.brakeAccel, g_cfg.easeSpeed, g_cfg.rampTime);
	}
}

extern "C"
{
	struct F4SEPluginVersionData
	{
		std::uint32_t dataVersion;
		std::uint32_t pluginVersion;
		char name[256];
		char author[256];
		std::uint32_t addressIndependence;
		std::uint32_t structureIndependence;
		std::uint32_t compatibleVersions[16];
		std::uint32_t seVersionRequired;
		std::uint32_t reservedNonBreaking;
		std::uint32_t reservedBreaking;
		std::uint8_t reserved[512];
	};

	__declspec(dllexport) F4SEPluginVersionData F4SEPlugin_Version = {
		1,                    // dataVersion
		0x01000000,           // 1.0.0 (first public release)
		"JetpackFallBrake",
		"",
		1u << 0,              // kAddressIndependence_Signatures
		1u << 0,              // kStructureIndependence_NoStructs
		{ 0 },
		0, 0, 0, { 0 }
	};

	__declspec(dllexport) bool F4SEPlugin_Load(const F4SEInterface* f4se)
	{
		OpenLog();
		Log("JetpackFallBrake 1.0.0 (F4SE %08X, runtime %08X)", f4se ? f4se->f4seVersion : 0, f4se ? f4se->runtimeVersion : 0);
		if (f4se && f4se->isEditor) {
			return false;
		}
		Install();
		return true;
	}
}
