// Updater.cpp — автоматичне оновлення програми через GitHub Releases
//
// Як це працює:
//   1) програма запитує в GitHub API (api.github.com) відомості про останній випуск у форматі JSON:
//      номер версії, текст «Що нового», посилання на інсталятор і його контрольну суму SHA-256;
//   2) якщо версія новіша за встановлену, користувачу пропонується оновитися;
//   3) інсталятор завантажується, перевіряється його контрольна сума, після чого головне вікно
//      запускає його в тихому режимі і закривається, щоб інсталятор міг замінити файли програми.
// На GitHub надсилається лише звичайний HTTPS-запит, жодних даних про комп'ютер чи диски.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include "Updater.h"
#include "resource.h"

#include <string>
#include <vector>
#include <utility>
#include <cwctype>
#include <cwchar>
#include <cstring>
#include <cstdlib>

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#endif

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4   // системні налаштування проксі (Windows 8.1+)
#endif

const wchar_t* const kReleasesPage = L"https://github.com/s1xen7ag/DiskMonitor/releases/latest";

namespace {

const wchar_t* const kLatestApi = L"https://api.github.com/repos/s1xen7ag/DiskMonitor/releases/latest";
// Інсталятор і сторінка випуску приймаються лише з репозиторію програми
const wchar_t* const kDownloadPrefix = L"https://github.com/s1xen7ag/DiskMonitor/releases/download/";
const wchar_t* const kPagePrefix = L"https://github.com/s1xen7ag/DiskMonitor/releases/";

const size_t kMaxApiBytes = 1024 * 1024;           // відповідь API — кілька кілобайт
const size_t kMaxSetupBytes = 100 * 1024 * 1024;   // інсталятор — близько 2 МБ

// ---------- Мережа (WinHTTP) ----------

struct InternetHandle {
    HINTERNET h = nullptr;
    ~InternetHandle() { if (h) WinHttpCloseHandle(h); }
};

std::wstring NetError(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return L"немає з'єднання з інтернетом або сервер недоступний";
    case ERROR_WINHTTP_TIMEOUT:
        return L"сервер не відповідає";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return L"не вдалося встановити захищене з'єднання з сервером";
    }
    return L"помилка мережі (код " + std::to_wstring(code) + L")";
}

// Завантажує вміст за HTTPS-адресою в пам'ять. Переадресації (GitHub віддає файли з іншого
// сервера) WinHTTP виконує сам, але лише на інші HTTPS-адреси.
bool HttpGet(const std::wstring& url, const wchar_t* accept, size_t maxBytes, std::string& body, std::wstring& error) {
    wchar_t host[256] = L"", path[2048] = L"", extra[1024] = L"";
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;     uc.dwUrlPathLength = ARRAYSIZE(path);
    uc.lpszExtraInfo = extra;  uc.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"неправильна адреса сервера";
        return false;
    }

    // Заголовок User-Agent обов'язковий для GitHub API
    std::wstring agent = L"DiskMonitor/" L"" APP_VERSION_STR;
    InternetHandle session, connection, request;
    session.h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) { error = NetError(GetLastError()); return false; }
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);

    connection.h = WinHttpConnect(session.h, host, uc.nPort, 0);
    std::wstring object = std::wstring(path) + extra;
    if (connection.h)
        request.h = WinHttpOpenRequest(connection.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    std::wstring headers = std::wstring(L"Accept: ") + accept + L"\r\n";
    if (!request.h ||
        !WinHttpSendRequest(request.h, headers.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        error = NetError(GetLastError());
        return false;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        if (status == 404)      error = L"на GitHub ще немає опублікованих версій";
        else if (status == 403 || status == 429) error = L"GitHub тимчасово обмежив кількість запитів, спробуйте пізніше";
        else                    error = L"сервер повернув помилку " + std::to_wstring(status);
        return false;
    }

    body.clear();
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) { error = NetError(GetLastError()); return false; }
        if (avail == 0) break;
        if (body.size() + avail > maxBytes) { error = L"файл завеликий"; return false; }
        size_t old = body.size();
        body.resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, &body[old], avail, &read)) { error = NetError(GetLastError()); return false; }
        body.resize(old + read);
    }
    return true;
}

// ---------- Мінімальний розбір JSON (лише те, що потрібно для відповіді GitHub) ----------

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    std::wstring str;                                   // текст рядка (або числа)
    std::vector<Json> items;                            // елементи масиву
    std::vector<std::pair<std::wstring, Json>> fields;  // поля об'єкта

    const Json* Get(const wchar_t* key) const {
        for (auto& f : fields)
            if (f.first == key) return &f.second;
        return nullptr;
    }
    std::wstring Str(const wchar_t* key) const {
        const Json* j = Get(key);
        return j && j->type == String ? j->str : L"";
    }
};

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : s_(text) {}

    bool Parse(Json& out) {
        SkipSpaces();
        if (!Value(out, 0)) return false;
        SkipSpaces();
        return p_ == s_.size();
    }

private:
    const std::string& s_;
    size_t p_ = 0;

    void SkipSpaces() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\r' || s_[p_] == '\n')) ++p_;
    }
    bool Eat(char c) {
        SkipSpaces();
        if (p_ < s_.size() && s_[p_] == c) { ++p_; return true; }
        return false;
    }
    bool Word(const char* w) {
        size_t n = strlen(w);
        if (s_.compare(p_, n, w) != 0) return false;
        p_ += n;
        return true;
    }

    bool Value(Json& v, int depth) {
        if (depth > 64) return false;
        SkipSpaces();
        if (p_ >= s_.size()) return false;
        char c = s_[p_];
        if (c == '{') {
            ++p_;
            v.type = Json::Object;
            if (Eat('}')) return true;
            do {
                std::wstring key;
                SkipSpaces();
                if (!String(key) || !Eat(':')) return false;
                v.fields.emplace_back(key, Json());
                if (!Value(v.fields.back().second, depth + 1)) return false;
            } while (Eat(','));
            return Eat('}');
        }
        if (c == '[') {
            ++p_;
            v.type = Json::Array;
            if (Eat(']')) return true;
            do {
                v.items.emplace_back();
                if (!Value(v.items.back(), depth + 1)) return false;
            } while (Eat(','));
            return Eat(']');
        }
        if (c == '"') { v.type = Json::String; return String(v.str); }
        if (Word("true") || Word("false")) { v.type = Json::Bool; return true; }
        if (Word("null")) { v.type = Json::Null; return true; }
        size_t start = p_;
        while (p_ < s_.size() && strchr("+-0123456789.eE", s_[p_])) ++p_;
        if (p_ == start) return false;
        v.type = Json::Number;
        v.str = FromUtf8(s_.substr(start, p_ - start));
        return true;
    }

    // Рядок JSON: текст у UTF-8 з екрануванням \n, \", \uXXXX тощо
    bool String(std::wstring& out) {
        if (p_ >= s_.size() || s_[p_] != '"') return false;
        ++p_;
        std::string run;   // шматок тексту без екранування — перетворюється з UTF-8 цілком
        auto flush = [&] { out += FromUtf8(run); run.clear(); };
        while (p_ < s_.size()) {
            char c = s_[p_++];
            if (c == '"') { flush(); return true; }
            if (c != '\\') { run += c; continue; }
            if (p_ >= s_.size()) return false;
            char e = s_[p_++];
            switch (e) {
            case 'n': run += '\n'; break;
            case 't': run += '\t'; break;
            case 'r': run += '\r'; break;
            case 'b': run += '\b'; break;
            case 'f': run += '\f'; break;
            case 'u': {
                if (p_ + 4 > s_.size()) return false;
                wchar_t code = (wchar_t)strtoul(s_.substr(p_, 4).c_str(), nullptr, 16);
                p_ += 4;
                flush();
                out += code;   // сурогатні пари UTF-16 складаються самі
                break;
            }
            default: run += e; break;   // \" \\ \/
            }
        }
        return false;
    }
};

// ---------- Розбір відомостей про випуск ----------

bool IsVersion(const std::wstring& v) {
    if (v.empty() || !iswdigit(v[0])) return false;
    for (wchar_t c : v)
        if (!iswdigit(c) && c != L'.') return false;
    return true;
}

bool EndsWithNoCase(const std::wstring& s, const wchar_t* tail) {
    size_t n = wcslen(tail);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}

bool IsSha256(const std::wstring& s) {
    if (s.size() != 64) return false;
    for (wchar_t c : s)
        if (!iswxdigit(c)) return false;
    return true;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// Перший розділ опису випуску («Що нового …») без розмітки Markdown, для показу у вікні
std::wstring ReleaseNotes(const std::wstring& body) {
    std::wstring result;
    bool inSection = false, blank = false;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t end = body.find(L'\n', pos);
        if (end == std::wstring::npos) end = body.size();
        std::wstring line = body.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();

        if (line.rfind(L"#", 0) == 0) {          // заголовок: перший відкриває розділ, другий закриває
            if (inSection) break;
            inSection = true;
            continue;
        }
        if (line.rfind(L"---", 0) == 0) break;
        inSection = true;

        std::wstring clean;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == L'*' || line[i] == L'`') continue;   // **жирний**, `код`
            clean += line[i];
        }
        if (clean.rfind(L"- ", 0) == 0) clean = L"• " + clean.substr(2);
        if (clean.empty()) {                    // кілька порожніх рядків поспіль — як один
            blank = !result.empty();
            continue;
        }
        if (blank) result += L"\n";
        blank = false;
        result += clean + L"\n";
    }
    while (!result.empty() && (result.back() == L'\n' || result.back() == L' ')) result.pop_back();
    if (result.size() > 1500) result = result.substr(0, 1500) + L"…";
    return result;
}

// Назва файлу з відповіді сервера — лише латиниця, цифри, крапка, дефіс і підкреслення
std::wstring SafeFileName(const std::wstring& name) {
    std::wstring r;
    for (wchar_t c : name)
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
            c == L'.' || c == L'-' || c == L'_')
            r += c;
    if (r.empty() || r[0] == L'.' || !EndsWithNoCase(r, L".exe")) r = L"DiskMonitor_Setup.exe";
    return r;
}

bool ParseRelease(const std::string& jsonText, UpdateInfo& info, std::wstring& error) {
    Json root;
    if (!JsonParser(jsonText).Parse(root) || root.type != Json::Object) {
        error = L"сервер надіслав незрозумілу відповідь";
        return false;
    }
    info = UpdateInfo();
    std::wstring tag = root.Str(L"tag_name");
    if (!tag.empty() && (tag[0] == L'v' || tag[0] == L'V')) tag.erase(0, 1);
    if (!IsVersion(tag)) {
        error = L"на GitHub вказано неправильний номер версії";
        return false;
    }
    info.version = tag;

    std::wstring body = root.Str(L"body");
    info.notes = ReleaseNotes(body);
    info.pageUrl = root.Str(L"html_url");
    if (info.pageUrl.rfind(kPagePrefix, 0) != 0) info.pageUrl = kReleasesPage;

    // Інсталятор — файл *Setup*.exe серед прикріплених до випуску
    if (const Json* assets = root.Get(L"assets")) {
        for (auto& a : assets->items) {
            std::wstring name = a.Str(L"name");
            std::wstring url = a.Str(L"browser_download_url");
            if (!EndsWithNoCase(name, L".exe") || Lower(name).find(L"setup") == std::wstring::npos) continue;
            if (url.rfind(kDownloadPrefix, 0) != 0) continue;
            info.setupName = name;
            info.setupUrl = url;
            // Контрольну суму GitHub рахує сам («digest»: "sha256:…")
            std::wstring digest = a.Str(L"digest");
            if (digest.rfind(L"sha256:", 0) == 0) info.sha256 = Lower(digest.substr(7));
            break;
        }
    }
    // Запасний варіант — контрольна сума, вказана в описі випуску
    if (!IsSha256(info.sha256)) {
        info.sha256.clear();
        for (size_t i = 0; i + 64 <= body.size(); ++i) {
            if (i > 0 && iswxdigit(body[i - 1])) continue;
            std::wstring cand = body.substr(i, 64);
            if (IsSha256(cand) && (i + 64 == body.size() || !iswxdigit(body[i + 64]))) {
                info.sha256 = Lower(cand);
                break;
            }
        }
    }
    return true;
}

// ---------- Контрольна сума SHA-256 (Windows CNG) ----------

std::wstring Sha256Hex(const void* data, size_t size) {
    std::wstring hex;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    UCHAR digest[32];
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        if (BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
            if (BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)data, (ULONG)size, 0)) &&
                BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
                const wchar_t* digits = L"0123456789abcdef";
                for (UCHAR b : digest) { hex += digits[b >> 4]; hex += digits[b & 15]; }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return hex;
}

bool ReadWholeFile(HANDLE f, std::string& data) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart > (LONGLONG)kMaxSetupBytes) return false;
    data.resize((size_t)size.QuadPart);
    DWORD read = 0;
    SetFilePointer(f, 0, nullptr, FILE_BEGIN);
    return data.empty() || (ReadFile(f, &data[0], (DWORD)data.size(), &read, nullptr) && read == data.size());
}

} // namespace

int CompareVersions(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        long x = 0, y = 0;
        while (i < a.size() && a[i] != L'.') x = x * 10 + (a[i++] - L'0');
        while (j < b.size() && b[j] != L'.') y = y * 10 + (b[j++] - L'0');
        if (x != y) return x < y ? -1 : 1;
        ++i;
        ++j;
    }
    return 0;
}

bool FetchLatestRelease(UpdateInfo& info, std::wstring& error) {
    std::string json;
    if (!HttpGet(kLatestApi, L"application/vnd.github+json", kMaxApiBytes, json, error)) return false;
    return ParseRelease(json, info, error);
}

HANDLE DownloadSetup(const UpdateInfo& info, std::wstring& path, std::wstring& error) {
    if (info.setupUrl.empty() || !IsSha256(info.sha256)) {
        error = L"для цієї версії немає інсталятора з контрольною сумою";
        return INVALID_HANDLE_VALUE;
    }
    std::string data;
    if (!HttpGet(info.setupUrl, L"application/octet-stream", kMaxSetupBytes, data, error)) return INVALID_HANDLE_VALUE;
    if (Sha256Hex(data.data(), data.size()) != info.sha256) {
        error = L"контрольна сума завантаженого файлу не збігається (файл пошкоджений)";
        return INVALID_HANDLE_VALUE;
    }

    wchar_t temp[MAX_PATH] = L"";
    GetTempPathW(MAX_PATH, temp);
    std::wstring dir = std::wstring(temp) + L"DiskMonitor_Update";
    CreateDirectoryW(dir.c_str(), nullptr);
    path = dir + L"\\" + SafeFileName(info.setupName);

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    bool ok = f != INVALID_HANDLE_VALUE &&
              WriteFile(f, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) {
        error = L"не вдалося зберегти файл у тимчасову папку";
        return INVALID_HANDLE_VALUE;
    }

    // Відкриваємо файл знову із забороною запису і ще раз перевіряємо, що це саме той файл
    HANDLE r = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::string check;
    if (r == INVALID_HANDLE_VALUE || !ReadWholeFile(r, check) || Sha256Hex(check.data(), check.size()) != info.sha256) {
        if (r != INVALID_HANDLE_VALUE) CloseHandle(r);
        error = L"файл інсталятора змінився після завантаження";
        return INVALID_HANDLE_VALUE;
    }
    return r;
}
