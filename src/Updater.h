// Updater.h — перевірка оновлень програми на GitHub і завантаження нової версії
#pragma once

#include <windows.h>
#include <string>

// Відомості про останній випуск програми на GitHub (сторінка Releases)
struct UpdateInfo {
    std::wstring version;     // номер версії без «v», напр. "1.3"
    std::wstring notes;       // «Що нового» — перший розділ опису випуску, без розмітки Markdown
    std::wstring pageUrl;     // сторінка випуску на GitHub
    std::wstring setupName;   // назва файлу інсталятора
    std::wstring setupUrl;    // пряме посилання на інсталятор (порожнє, якщо його немає)
    std::wstring sha256;      // контрольна сума інсталятора: 64 шістнадцяткові цифри малими літерами
};

// Адреса сторінки з усіма версіями програми
extern const wchar_t* const kReleasesPage;

// Порівнює номери версій ("1.10" новіша за "1.9", "1.2" = "1.2.0"): <0, 0 або >0
int CompareVersions(const std::wstring& a, const std::wstring& b);

// Отримує з GitHub відомості про останній випуск. Якщо не вдалося — false, а error пояснює чому
bool FetchLatestRelease(UpdateInfo& info, std::wstring& error);

// Завантажує інсталятор у тимчасову папку і перевіряє його контрольну суму SHA-256.
// Повертає дескриптор файлу, відкритого лише для читання із забороною запису, щоб файл
// не можна було підмінити між перевіркою і запуском (закрити після запуску інсталятора).
// Якщо не вдалося — INVALID_HANDLE_VALUE, а error пояснює чому.
HANDLE DownloadSetup(const UpdateInfo& info, std::wstring& path, std::wstring& error);
