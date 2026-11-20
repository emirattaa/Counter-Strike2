#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <iostream>

DWORD FindProcessId(const wchar_t* processName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName) == 0) {
                CloseHandle(snapshot);
                return entry.th32ProcessID;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return 0;
}

int main() {
    const wchar_t* targetProcess = L"cs2.exe";
    const wchar_t* dllPath = L"C:\\Users\\Dinc\\OneDrive\\Desktop\\CS2-Internal-main\\build\\CS2-Internal.dll";
    // "C:\\Users\\Dinc\\OneDrive\\Desktop\\CS2-Internal-main\\build\\CS2-Internal.dll"
    // DLL dosyası var mı kontrol et
    if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
        std::wcout << L"[X] DLL bulunamadi: " << dllPath << std::endl;
        system("pause");
        return 1;
    }

    // Süreci bul
    DWORD pid = FindProcessId(targetProcess);
    if (!pid) {
        std::wcout << L"[X] " << targetProcess << L" bulunamadi. Oyunu acip tekrar dene." << std::endl;
        system("pause");
        return 1;
    }
    std::wcout << L"[+] PID: " << pid << std::endl;

    // Süreçe eriş
    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProcess) {
        std::wcout << L"[X] OpenProcess basarisiz. (Error: " << GetLastError() << L")" << std::endl;
        system("pause");
        return 1;
    }

    // Bellek ayir
    size_t pathSize = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(hProcess, nullptr, pathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        std::wcout << L"[X] VirtualAllocEx basarisiz." << std::endl;
        CloseHandle(hProcess);
        system("pause");
        return 1;
    }

    // DLL yolunu yaz
    if (!WriteProcessMemory(hProcess, remoteMem, dllPath, pathSize, nullptr)) {
        std::wcout << L"[X] WriteProcessMemory basarisiz." << std::endl;
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        system("pause");
        return 1;
    }

    // LoadLibraryW adresi
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC pLoadLibraryW = GetProcAddress(hKernel32, "LoadLibraryW");

    // Remote thread
    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, (LPTHREAD_START_ROUTINE)pLoadLibraryW, remoteMem, 0, nullptr);
    if (!hThread) {
        std::wcout << L"[X] CreateRemoteThread basarisiz. (Error: " << GetLastError() << L")" << std::endl;
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        system("pause");
        return 1;
    }

    // Bitmesini bekle
    WaitForSingleObject(hThread, 5000);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    if (exitCode == STILL_ACTIVE) {
        std::wcout << L"[!] Thread henuz bitmedi (timeout)." << std::endl;
    }
    else if (exitCode == 0) {
        std::wcout << L"[X] LoadLibraryW basarisiz - DLL yuklenemedi." << std::endl;
    }
    else {
        std::wcout << L"[+] Inject basarili! HMODULE: 0x" << std::hex << exitCode << std::dec << std::endl;
    }

    // Temizle
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProcess);

    std::wcout << L"\n[OK] Tamamlandi." << std::endl;
    system("pause");
    return 0;
}