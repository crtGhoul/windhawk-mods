// ==WindhawkMod==
// @id              per-app-env-vars
// @name            Per-App Env Vars
// @description     Per-application environment variables without batch wrappers, via a custom environment block in a CreateProcessW hook
// @version         1.0
// @author          crtGhoul
// @github          https://github.com/crtGhoul
// @license         MIT
// @include         *
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Per-App Env Vars

Sets environment variables for specific applications without wrapping
them in batch files. Example: give one Qt app `QT_SCALE_FACTOR=1.5`
without affecting anything else on the system.

How it works: hooks `CreateProcessW` in every process. When a launch
matches one of your rules (by target exe basename, case-insensitive),
the mod builds a new Unicode environment block — starting from the
caller's block, or the current process environment when the caller
passed none — applies your overrides (replacing existing entries or
appending new ones), and launches the child with that block. Launches
that match no rule pass through completely untouched.

Rule format (one per line; semicolons also work as rule separators):

    exename.exe:VAR1=value1;VAR2=value2

Example:

    myapp.exe:QT_SCALE_FACTOR=1.5;FOO=bar
    game.exe:__GL_SYNC_TO_VBLANK=0

## Settings

- **Enabled** — master switch.
- **Rules** — one rule per line: `exename.exe:VAR=value;VAR2=value2`.

## Honest limitations

- Only the Unicode (`CreateProcessW`) path is handled. A caller that
  passes its own ANSI environment block (non-NULL `lpEnvironment`
  without `CREATE_UNICODE_ENVIRONMENT`) is passed through untouched —
  rules are not applied to it. In practice almost everything passes
  NULL and inherits, so this rarely matters.
- Matching is by exe basename only; two different apps with the same
  exe name share the rule.
- A variable value cannot contain a semicolon (it is the separator).
- Verify on a real PC with Process Explorer's Environment tab that the
  child got exactly the intended block.

## Testing status

Type-checked only with x86_64-w64-mingw32-g++ -fsyntax-only against the
real windhawk_api.h (Windhawk v1.7.3); NOT compiled with Windhawk's real
compiler; NOT tested on a real Windows machine.
*/
// ==WindhawkModSettings==
/*
- enabled: true
  $name: Enabled
  $description: Master switch. When off, all launches pass through untouched.
- rules: ""
  $name: Rules
  $description: One per line: exename.exe:VAR=value;VAR2=value2
*/
// ==/WindhawkModSettings==

#include <windows.h>

#include <cwchar>
#include <cwctype>
#include <new>
#include <string>
#include <vector>

namespace {

BOOL g_enabled = TRUE;
BOOL g_hooked = FALSE;

struct EnvVar {
    std::wstring name;
    std::wstring value;
};

struct EnvRule {
    std::wstring exe;  // basename, matched case-insensitively
    std::vector<EnvVar> vars;
};

std::vector<EnvRule> g_rules;

using CreateProcessW_t = decltype(&CreateProcessW);
CreateProcessW_t g_createProcessW_Original = nullptr;

std::wstring Trim(const std::wstring &s) {
    size_t b = 0;
    while (b < s.size() && iswspace(s[b])) {
        ++b;
    }
    size_t e = s.size();
    while (e > b && iswspace(s[e - 1])) {
        --e;
    }
    return s.substr(b, e - b);
}

void AddAssignment(EnvRule &rule, const std::wstring &seg) {
    size_t eq = seg.find(L'=');
    if (eq == std::wstring::npos) {
        return;
    }
    std::wstring name = Trim(seg.substr(0, eq));
    std::wstring value = Trim(seg.substr(eq + 1));
    if (name.empty()) {
        return;
    }
    rule.vars.push_back({name, value});
}

// Rules are separated by newlines or semicolons; within a rule, the first
// "name:" (colon before any '=') starts a new rule, later segments are
// extra VAR=value assignments for the current rule.
void ParseRules(const std::wstring &text) {
    g_rules.clear();

    std::vector<std::wstring> segs;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        wchar_t c = (i < text.size()) ? text[i] : L'\0';
        if (c == L'\r' || c == L'\n' || c == L';' || c == L'\0') {
            if (i > start) {
                segs.push_back(Trim(text.substr(start, i - start)));
            }
            start = i + 1;
        }
    }

    EnvRule *cur = nullptr;
    for (const std::wstring &seg : segs) {
        if (seg.empty()) {
            continue;
        }
        size_t eq = seg.find(L'=');
        size_t colon = seg.rfind(L':', (eq == std::wstring::npos)
                                            ? std::wstring::npos
                                            : eq);
        bool startsNew =
            (colon != std::wstring::npos) &&
            (eq == std::wstring::npos || colon < eq);
        if (startsNew) {
            EnvRule rule;
            rule.exe = Trim(seg.substr(0, colon));
            std::wstring rest = Trim(seg.substr(colon + 1));
            if (rule.exe.empty()) {
                cur = nullptr;
                continue;
            }
            g_rules.push_back(std::move(rule));
            cur = &g_rules.back();
            if (!rest.empty()) {
                AddAssignment(*cur, rest);
            }
        } else if (cur) {
            AddAssignment(*cur, seg);
        }
    }
}

std::wstring BaseName(const std::wstring &path) {
    size_t i = path.find_last_of(L"\\/");
    return (i == std::wstring::npos) ? path : path.substr(i + 1);
}

std::wstring FirstToken(const std::wstring &cmd) {
    size_t i = 0;
    while (i < cmd.size() && iswspace(cmd[i])) {
        ++i;
    }
    if (i < cmd.size() && cmd[i] == L'"') {
        ++i;
        size_t j = cmd.find(L'"', i);
        return cmd.substr(i, (j == std::wstring::npos) ? j : j - i);
    }
    size_t j = i;
    while (j < cmd.size() && !iswspace(cmd[j])) {
        ++j;
    }
    return cmd.substr(i, j - i);
}

// Builds a new double-null-terminated Unicode environment block: copies
// every entry of |base|, replacing entries whose names appear in |vars|,
// then appends any remaining vars. Returns nullptr on allocation failure.
LPWSTR BuildEnvBlock(LPCWSTR base, const std::vector<EnvVar> &vars) {
    auto findOverride = [&vars](LPCWSTR entry, size_t *indexOut) -> bool {
        LPCWSTR eq = wcschr(entry, L'=');
        size_t nameLen = eq ? (size_t)(eq - entry) : wcslen(entry);
        for (size_t k = 0; k < vars.size(); ++k) {
            if (vars[k].name.size() == nameLen && !vars[k].name.empty() &&
                _wcsnicmp(vars[k].name.c_str(), entry, nameLen) == 0) {
                *indexOut = k;
                return true;
            }
        }
        return false;
    };

    std::vector<char> used(vars.size(), 0);
    size_t total = 1;  // Final extra null terminator.
    for (LPCWSTR p = base; *p; p += wcslen(p) + 1) {
        size_t vi = 0;
        if (findOverride(p, &vi)) {
            used[vi] = 1;
            total += vars[vi].name.size() + 1 + vars[vi].value.size() + 1;
        } else {
            total += wcslen(p) + 1;
        }
    }
    for (size_t k = 0; k < vars.size(); ++k) {
        if (!used[k]) {
            total += vars[k].name.size() + 1 + vars[k].value.size() + 1;
        }
    }

    WCHAR *block = new (std::nothrow) WCHAR[total];
    if (!block) {
        return nullptr;
    }

    std::fill(used.begin(), used.end(), 0);
    LPWSTR dst = block;
    for (LPCWSTR p = base; *p; p += wcslen(p) + 1) {
        size_t vi = 0;
        if (findOverride(p, &vi)) {
            used[vi] = 1;
            const std::wstring &n = vars[vi].name;
            const std::wstring &v = vars[vi].value;
            wmemcpy(dst, n.c_str(), n.size());
            dst += n.size();
            *dst++ = L'=';
            wmemcpy(dst, v.c_str(), v.size());
            dst += v.size();
            *dst++ = L'\0';
        } else {
            size_t len = wcslen(p);
            wmemcpy(dst, p, len + 1);
            dst += len + 1;
        }
    }
    for (size_t k = 0; k < vars.size(); ++k) {
        if (!used[k]) {
            const std::wstring &n = vars[k].name;
            const std::wstring &v = vars[k].value;
            wmemcpy(dst, n.c_str(), n.size());
            dst += n.size();
            *dst++ = L'=';
            wmemcpy(dst, v.c_str(), v.size());
            dst += v.size();
            *dst++ = L'\0';
        }
    }
    *dst++ = L'\0';
    return block;
}

BOOL WINAPI CreateProcessW_Hook(
    LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles,
    DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
    LPSTARTUPINFOW lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation) {
    if (g_enabled && !g_rules.empty()) {
        std::wstring target;
        if (lpApplicationName && *lpApplicationName) {
            target = BaseName(lpApplicationName);
        } else if (lpCommandLine && *lpCommandLine) {
            target = BaseName(FirstToken(lpCommandLine));
        }

        const std::vector<EnvVar> *hit = nullptr;
        for (const EnvRule &rule : g_rules) {
            if (_wcsicmp(rule.exe.c_str(), target.c_str()) == 0) {
                hit = &rule.vars;
                break;
            }
        }

        if (hit && !hit->empty()) {
            // Only Unicode blocks are rebuilt; an explicit ANSI block from
            // the caller is passed through untouched (see readme).
            bool callerAnsiBlock =
                lpEnvironment &&
                !(dwCreationFlags & CREATE_UNICODE_ENVIRONMENT);
            if (!callerAnsiBlock) {
                LPWSTR base = nullptr;
                bool freeBase = false;
                if (lpEnvironment) {
                    base = (LPWSTR)lpEnvironment;
                } else {
                    base = GetEnvironmentStringsW();
                    freeBase = (base != nullptr);
                }
                if (base) {
                    LPWSTR block = BuildEnvBlock(base, *hit);
                    if (freeBase) {
                        FreeEnvironmentStringsW(base);
                    }
                    if (block) {
                        BOOL ok = g_createProcessW_Original(
                            lpApplicationName, lpCommandLine,
                            lpProcessAttributes, lpThreadAttributes,
                            bInheritHandles,
                            dwCreationFlags | CREATE_UNICODE_ENVIRONMENT,
                            block, lpCurrentDirectory, lpStartupInfo,
                            lpProcessInformation);
                        delete[] block;
                        return ok;
                    }
                    Wh_Log(L"BuildEnvBlock failed, passing through");
                }
            }
        }
    }

    return g_createProcessW_Original(
        lpApplicationName, lpCommandLine, lpProcessAttributes,
        lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment,
        lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
}

void LoadSettings() {
    g_enabled = Wh_GetIntSetting(L"enabled");
    PCWSTR rules = Wh_GetStringSetting(L"rules");
    ParseRules(rules ? rules : L"");
    Wh_FreeStringSetting(rules);
}

void UpdateHook(bool applyNow) {
    if (g_enabled && !g_hooked) {
        if (Wh_SetFunctionHook((void *)CreateProcessW,
                               (void *)CreateProcessW_Hook,
                               (void **)&g_createProcessW_Original)) {
            g_hooked = TRUE;
        } else {
            Wh_Log(L"CreateProcessW hook failed");
        }
    } else if (!g_enabled && g_hooked) {
        Wh_RemoveFunctionHook((void *)CreateProcessW);
        g_hooked = FALSE;
    }
    if (applyNow) {
        Wh_ApplyHookOperations();
    }
}

}  // namespace

BOOL Wh_ModInit(void) {
    LoadSettings();
    UpdateHook(false);  // Windhawk applies hook ops automatically after init.
    return TRUE;
}

void Wh_ModUninit(void) {
    if (g_hooked) {
        Wh_RemoveFunctionHook((void *)CreateProcessW);
        g_hooked = FALSE;
    }
}

void Wh_ModSettingsChanged(void) {
    LoadSettings();
    UpdateHook(true);
}
