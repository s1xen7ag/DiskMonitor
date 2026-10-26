// DiskInfo.cpp — отримання інформації про накопичувачі через WMI та аналіз їхнього стану
//
// Використовуються такі класи WMI:
//   root\cimv2                        Win32_DiskDrive, Win32_LogicalDisk, Win32_LogicalDiskToPartition,
//                                     Win32_PerfFormattedData_PerfDisk_PhysicalDisk (навантаження)
//   root\Microsoft\Windows\Storage    MSFT_PhysicalDisk (тип HDD/SSD, стан),
//                                     MSFT_StorageReliabilityCounter (температура, знос, години роботи),
//                                     MSFT_Disk, MSFT_Partition (розмітка диска, нерозподілене місце)
//   root\WMI                          MSStorageDriver_FailurePredictStatus / Data / Thresholds (SMART)
//
// Програма тільки ЧИТАЄ дані. Жодних операцій запису на диски не виконується.

#ifndef NOMINMAX
#define NOMINMAX      // щоб макроси min/max з windows.h не заважали std::min/std::max
#endif
#define _WIN32_DCOM
#include <windows.h>
#include <winioctl.h>
#include <wbemidl.h>
#include <oleauto.h>

#include "DiskInfo.h"

#include <algorithm>
#include <map>
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>

#ifdef _MSC_VER
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

namespace {

// ---------- Дрібні допоміжні функції для роботи з COM/WMI ----------

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring Upper(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towupper(c);
    return s;
}

std::wstring GetStr(IWbemClassObject* o, const wchar_t* name) {
    std::wstring r;
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(o->Get(name, 0, &v, nullptr, nullptr))) {
        if (v.vt == VT_BSTR && v.bstrVal) {
            r = v.bstrVal;
        } else if (v.vt != VT_NULL && v.vt != VT_EMPTY && !(v.vt & VT_ARRAY)) {
            VARIANT t;
            VariantInit(&t);
            if (SUCCEEDED(VariantChangeType(&t, &v, 0, VT_BSTR)) && t.bstrVal) r = t.bstrVal;
            VariantClear(&t);
        }
    }
    VariantClear(&v);
    return Trim(r);
}

// Числове значення властивості. WMI повертає uint64 як рядок, тому обробляємо і BSTR.
long long GetNum(IWbemClassObject* o, const wchar_t* name, long long def = -1) {
    long long r = def;
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(o->Get(name, 0, &v, nullptr, nullptr))) {
        switch (v.vt) {
        case VT_BSTR:
            if (v.bstrVal && *v.bstrVal) r = (long long)wcstoull(v.bstrVal, nullptr, 10);
            break;
        case VT_I1:   r = v.cVal; break;
        case VT_UI1:  r = v.bVal; break;
        case VT_I2:   r = v.iVal; break;
        case VT_UI2:  r = v.uiVal; break;
        case VT_I4:   r = v.lVal; break;
        case VT_UI4:  r = v.ulVal; break;
        case VT_I8:   r = v.llVal; break;
        case VT_UI8:  r = (long long)v.ullVal; break;
        case VT_BOOL: r = (v.boolVal != VARIANT_FALSE) ? 1 : 0; break;
        default: break;
        }
    }
    VariantClear(&v);
    return r;
}

// Масив байтів (використовується для SMART-даних)
std::vector<uint8_t> GetBytes(IWbemClassObject* o, const wchar_t* name) {
    std::vector<uint8_t> out;
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(o->Get(name, 0, &v, nullptr, nullptr)) && (v.vt & VT_ARRAY) && v.parray) {
        LONG lo = 0, hi = -1;
        VARTYPE vt = VT_EMPTY;
        SafeArrayGetLBound(v.parray, 1, &lo);
        SafeArrayGetUBound(v.parray, 1, &hi);
        SafeArrayGetVartype(v.parray, &vt);
        void* data = nullptr;
        if (hi >= lo && SUCCEEDED(SafeArrayAccessData(v.parray, &data))) {
            size_t n = (size_t)(hi - lo + 1);
            if (vt == VT_UI1 || vt == VT_I1) {
                out.assign((uint8_t*)data, (uint8_t*)data + n);
            } else if (vt == VT_I4 || vt == VT_UI4) {
                for (size_t i = 0; i < n; ++i) out.push_back((uint8_t)((LONG*)data)[i]);
            } else if (vt == VT_I2 || vt == VT_UI2) {
                for (size_t i = 0; i < n; ++i) out.push_back((uint8_t)((SHORT*)data)[i]);
            }
            SafeArrayUnaccessData(v.parray);
        }
    }
    VariantClear(&v);
    return out;
}

// Виконує WQL-запит і викликає fn(obj) для кожного знайденого об'єкта
template <class F>
bool Query(IWbemServices* svc, const std::wstring& wql, F fn) {
    if (!svc) return false;
    BSTR lang = SysAllocString(L"WQL");
    BSTR text = SysAllocString(wql.c_str());
    IEnumWbemClassObject* en = nullptr;
    HRESULT hr = svc->ExecQuery(lang, text,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang);
    SysFreeString(text);
    if (FAILED(hr) || !en) return false;

    bool any = false;
    for (;;) {
        IWbemClassObject* obj = nullptr;
        ULONG got = 0;
        hr = en->Next(15000, 1, &obj, &got);   // 15 с — з запасом для повільних запитів SMART
        if (FAILED(hr) || got == 0 || !obj) break;
        any = true;
        fn(obj);
        obj->Release();
    }
    en->Release();
    return any;
}

IWbemServices* ConnectNamespace(IWbemLocator* loc, const wchar_t* ns) {
    IWbemServices* svc = nullptr;
    BSTR b = SysAllocString(ns);
    HRESULT hr = loc->ConnectServer(b, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
    SysFreeString(b);
    if (FAILED(hr) || !svc) return nullptr;
    CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return svc;
}

// ---------- Розшифровка кодів ----------

std::wstring BusName(long long code) {
    switch (code) {
    case 1:  return L"SCSI";
    case 2:  return L"ATAPI";
    case 3:  return L"ATA";
    case 4:  return L"IEEE 1394";
    case 6:  return L"Fibre Channel";
    case 7:  return L"USB";
    case 8:  return L"RAID";
    case 9:  return L"iSCSI";
    case 10: return L"SAS";
    case 11: return L"SATA";
    case 12: return L"SD";
    case 13: return L"MMC";
    case 14: return L"Віртуальний";
    case 15: return L"Віртуальний (файл)";
    case 16: return L"Storage Spaces";
    case 17: return L"NVMe";
    case 18: return L"SCM";
    case 19: return L"UFS";
    default: return L"";
    }
}

// З InstanceName класу root\WMI ("SCSI\Disk&Ven_...\4&12ab&0&000000_0")
// отримуємо PNPDeviceID ("SCSI\DISK&VEN_...\4&12AB&0&000000")
std::wstring InstanceToPnp(const std::wstring& inst) {
    std::wstring s = Upper(inst);
    size_t p = s.rfind(L'_');
    if (p != std::wstring::npos && p + 1 < s.size()) {
        bool digits = true;
        for (size_t i = p + 1; i < s.size(); ++i)
            if (!iswdigit(s[i])) digits = false;
        if (digits) s.erase(p);
    }
    return s;
}

PhysicalDisk* FindByPnp(Snapshot& s, const std::wstring& instanceName) {
    std::wstring pnp = InstanceToPnp(instanceName);
    for (auto& d : s.disks)
        if (!d.pnpId.empty() && Upper(d.pnpId) == pnp) return &d;
    return nullptr;
}

PhysicalDisk* FindByIndex(Snapshot& s, int index) {
    for (auto& d : s.disks)
        if (d.index == index) return &d;
    return nullptr;
}

const SmartAttribute* FindAttr(const PhysicalDisk& d, int id) {
    for (auto& a : d.smart)
        if (a.id == id) return &a;
    return nullptr;
}

bool Contains(const std::wstring& text, const wchar_t* what) {
    return Upper(text).find(what) != std::wstring::npos;
}

// SSD на контролері Phison (так само Kingston A400): впізнається за характерним набором атрибутів
bool IsPhisonLike(const PhysicalDisk& d) {
    for (int id : { 168, 170, 173, 218, 231 })
        if (!FindAttr(d, id)) return false;
    return true;
}

// Залишок ресурсу SATA SSD за SMART, % (0..100) або -1, якщо диск його не повідомляє.
// Атрибути SMART не стандартизовані: той самий номер у різних виробників означає різне, а залишок
// ресурсу одні контролери записують у нормалізоване значення атрибута, інші — у «сире». Тому
// правило вибирається за виробником (назвою моделі) або за набором атрибутів, характерним для
// контролера. Правила узгоджені з тим, як ресурс читає програма CrystalDiskInfo.
// how — звідки взято значення (показується у вікні деталей), attr — номер цього атрибута.
int SsdLifeFromSmart(const PhysicalDisk& d, std::wstring& how, int& attr) {
    auto source = [&](int id, const wchar_t* name, const wchar_t* field) {
        how = L"атрибут SMART " + std::to_wstring(id) + L" («" + name + L"», " + field + L")";
        attr = id;
    };
    // Залишок у нормалізованому значенні атрибута (100 — новий диск)
    auto normalized = [&](int id, const wchar_t* name) -> int {
        auto a = FindAttr(d, id);
        if (!a || a->current <= 0 || a->current > 100) return -1;
        source(id, name, L"нормалізоване значення");
        return a->current;
    };
    // Залишок у молодшому байті «сирого» значення
    auto rawPercent = [&](int id, const wchar_t* name) -> int {
        auto a = FindAttr(d, id);
        int v = a ? (int)(a->raw & 0xFF) : -1;
        if (v <= 0 || v > 100) return -1;
        source(id, name, L"сире значення");
        return v;
    };
    auto has = [&](std::initializer_list<int> ids) {
        for (int id : ids) if (!FindAttr(d, id)) return false;
        return true;
    };
    std::wstring m = Upper(d.model);
    auto startsWith = [&](const wchar_t* p) { return m.rfind(p, 0) == 0; };

    if (Contains(d.model, L"SAMSUNG"))
        return normalized(177, L"Wear Leveling Count");
    if (Contains(d.model, L"CRUCIAL") || Contains(d.model, L"MICRON") || startsWith(L"MTFD") ||
        (startsWith(L"CT") && Contains(d.model, L"SSD")))
        return normalized(202, L"Percent Lifetime Remaining");
    if (Contains(d.model, L"INTEL"))
        return normalized(233, L"Media Wearout Indicator");
    if (startsWith(L"WDC ") || startsWith(L"WD ") || Contains(d.model, L"SANDISK")) {
        // WD і SanDisk записують використаний ресурс у другий байт сирого значення атрибута 230.
        // Після вичерпання заявленого ресурсу він може перевищити 100 — тоді залишок 0 %
        auto a = FindAttr(d, 230);
        if (!a) return -1;
        int used = std::min(100, (int)((a->raw >> 8) & 0xFF));
        source(230, L"Media Wearout Indicator", L"сире значення");
        return 100 - used;
    }
    if (Contains(d.model, L"TOSHIBA") || Contains(d.model, L"KIOXIA")) {
        auto a = FindAttr(d, 173);
        int v = a ? a->current - 100 : -1;   // у Toshiba нормалізоване значення відраховується від 200
        if (v <= 0 || v > 100) return -1;
        source(173, L"Wear Leveling", L"нормалізоване значення мінус 100");
        return v;
    }
    if (Contains(d.model, L"SA400")) {
        // Kingston A400: залишок — у сирому значенні атрибута 231, а нормалізоване значення в
        // більшості прошивок його не відображає (крім прошивки 03070009)
        if (d.firmware.rfind(L"03070009", 0) == 0) return normalized(231, L"SSD Life Left");
        return rawPercent(231, L"SSD Life Left");
    }
    if (Contains(d.model, L"KINGSTON")) {
        int v = normalized(231, L"SSD Life Left");
        return v >= 0 ? v : normalized(169, L"Remaining Life");
    }
    // Контролери Silicon Motion (ADATA, Silicon Power, Transcend та ін.): сире значення атрибута 169
    if (has({ 160, 161, 163, 169 }))
        return rawPercent(169, L"Remaining Life");
    // Контролери Realtek (наприклад, AGI AI138): атрибут 169, нормалізоване значення.
    // Впізнаються за атрибутами 161–167 без атрибута 160, який є в Silicon Motion
    if (has({ 161, 162, 163, 164, 166, 167, 169 }) && !FindAttr(d, 160))
        return normalized(169, L"Remaining Life");
    // Контролери Phison (Patriot, Goodram, PNY та ін.): сире значення атрибута 231
    if (IsPhisonLike(d))
        return rawPercent(231, L"SSD Life Left");
    // Інші (SandForce, SK hynix тощо): атрибут 231 у нормалізованому значенні
    return normalized(231, L"SSD Life Left");
}

// Доповнює дані, яких не вистачає, на основі SMART і назви моделі
void FinishDisk(PhysicalDisk& d) {
    if (d.type.empty()) {
        bool ssdAttrs = FindAttr(d, 177) || FindAttr(d, 231) || FindAttr(d, 233);
        if (d.bus == L"NVMe" || Contains(d.model, L"SSD") || Contains(d.model, L"NVME") || ssdAttrs) {
            d.isSsd = true;
            d.type = L"SSD";
        } else if (d.bus == L"USB") {
            d.type = L"USB-накопичувач";
        } else if (FindAttr(d, 3) || FindAttr(d, 4)) {   // час розкручування шпинделя є лише в HDD
            d.type = L"HDD";
        } else {
            d.type = L"Невідомо";
        }
    }
    if (d.isSsd && d.bus == L"NVMe") d.type = L"SSD (NVMe)";
    if (!d.isSsd) d.wear = -1;   // лічильник зносу Windows має сенс лише для SSD

    if (d.temperature < 0) {
        const SmartAttribute* t = FindAttr(d, 194);
        if (!t) t = FindAttr(d, 190);
        if (t) {
            int v = (int)(t->raw & 0xFF);
            if (v > 0 && v < 120) d.temperature = v;
        }
    }
    if (d.powerOnHours < 0)
        if (auto a = FindAttr(d, 9)) d.powerOnHours = (long long)(a->raw & 0xFFFFFFFF);
    if (d.powerCycles < 0)
        if (auto a = FindAttr(d, 12)) d.powerCycles = (long long)(a->raw & 0xFFFFFFFF);
    if (auto a = FindAttr(d, 5))   d.reallocated   = (long long)(a->raw & 0xFFFFFFFF);
    if (auto a = FindAttr(d, 197)) d.pending       = (long long)(a->raw & 0xFFFFFFFF);
    if (auto a = FindAttr(d, 198)) d.uncorrectable = (long long)(a->raw & 0xFFFFFFFF);

    // Знос SSD. Для NVMe його дає журнал стану (Percentage Used). Для SATA SSD — атрибути SMART
    // виробника (див. SsdLifeFromSmart). Лічильник надійності Windows для багатьох SATA SSD
    // повертає 0 навіть тоді, коли накопичувач знос не повідомляє, тому сам по собі цей 0
    // не означає «100 % ресурсу».
    d.wearAttr = 0;
    if (d.hasNvmeLog) {
        d.wearSource = L"журнал стану NVMe (Percentage Used)";
    } else if (d.isSsd) {
        int wmiWear = d.wear;
        std::wstring how;
        int attr = 0;
        int life = SsdLifeFromSmart(d, how, attr);
        int smartWear = life >= 0 ? 100 - life : -1;
        if (smartWear >= 0 && smartWear >= wmiWear) {
            d.wear = smartWear;
            d.wearSource = how;
            d.wearAttr = attr;
        } else if (wmiWear > 0) {
            d.wear = wmiWear;
            d.wearSource = L"лічильник надійності Windows";
        } else {
            d.wear = -1;   // Windows дав 0, але SMART цього не підтверджує: краще «немає даних», ніж хибні 100 %
        }
    }
}

// Індекс механічного стану HDD — власна оцінка програми (0–100 %).
// Жорсткі диски не повідомляють «відсоток ресурсу», як SSD, бо магнітні пластини не зношуються
// від записів. Зате вони зношуються механічно, і це видно з атрибутів SMART. Індекс рахується як
// 100 мінус штрафи за кожну ознаку зносу; кожен штраф записується в hddHealthDetails, щоб було
// видно, з чого складається оцінка.
void ComputeHddHealth(PhysicalDisk& d) {
    d.hddHealth = -1;
    d.hddHealthDetails.clear();
    if (d.isSsd || d.type != L"HDD" || d.smart.empty()) return;   // без даних SMART оцінювати нічим

    int score = 100;
    auto penalty = [&](double p, const std::wstring& why) {
        int pts = (int)(p + 0.5);
        if (pts <= 0) return;
        score -= pts;
        d.hddHealthDetails.push_back(L"−" + std::to_wstring(pts) + L" — " + why);
    };
    auto raw = [&](int id) -> long long {
        auto a = FindAttr(d, id);
        return a ? (long long)(a->raw & 0xFFFFFFFF) : -1;
    };

    // Пошкодження поверхні — найвагоміші ознаки
    if (d.reallocated > 0)   penalty(std::min(40.0, d.reallocated * 1.0),   L"перепризначені сектори: " + FormatNumber(d.reallocated));
    if (d.pending > 0)       penalty(std::min(30.0, d.pending * 2.0),       L"нестабільні сектори: " + FormatNumber(d.pending));
    if (d.uncorrectable > 0) penalty(std::min(30.0, d.uncorrectable * 2.0), L"невиправні сектори: " + FormatNumber(d.uncorrectable));
    long long reported = raw(187);
    if (reported > 0)        penalty(std::min(20.0, reported * 2.0),        L"невиправні помилки читання: " + FormatNumber(reported));
    // Механіка: двигун шпинделя і паркування головок
    long long spinRetry = raw(10);
    if (spinRetry > 0)       penalty(std::min(20.0, spinRetry * 10.0),      L"повторні спроби розкручування шпинделя: " + FormatNumber(spinRetry));
    long long loadCycles = raw(193);
    if (loadCycles > 0)      penalty(std::min(10.0, loadCycles * 10.0 / 300000.0), L"цикли паркування головок: " + FormatNumber(loadCycles) + L" (типовий ресурс 300 000)");
    long long starts = raw(4);
    if (starts > 0)          penalty(std::min(5.0, starts * 5.0 / 50000.0), L"запуски шпинделя: " + FormatNumber(starts) + L" (типовий ресурс 50 000)");
    // Вік: з роками ймовірність відмови зростає навіть без помилок
    if (d.powerOnHours > 0)  penalty(std::min(15.0, d.powerOnHours * 15.0 / 50000.0), L"напрацювання: " + FormatNumber(d.powerOnHours) + L" год");

    // Явні ознаки несправності обмежують індекс зверху, хоч би якою була сума штрафів
    int cap = 100; std::wstring capWhy;
    auto limit = [&](int c, const wchar_t* why) { if (c < cap) { cap = c; capWhy = why; } };
    if (d.smartPredictFail == 1) limit(10, L"SMART прогнозує відмову");
    if (d.winHealth == 2)        limit(10, L"Windows позначила диск як несправний");
    else if (d.winHealth == 1)   limit(50, L"Windows повідомляє про проблеми з диском");
    for (auto& a : d.smart)
        if ((a.flags & 0x01) && a.threshold > 0 && a.current > 0 && a.current <= a.threshold)
            limit(10, L"критичний атрибут SMART досяг порогу");

    score = std::max(0, std::min(100, score));
    if (score > cap) {
        d.hddHealthDetails.push_back(L"обмежено до " + std::to_wstring(cap) + L"%: " + capWhy);
        score = cap;
    }
    d.hddHealth = score;
}

std::wstring Pad(const std::wstring& label, size_t width = 28) {
    std::wstring s = label + L":";
    while (s.size() < width) s += L' ';
    return s;
}

// Назва розділу без літери за його типом: GUID типу для дисків GPT або код типу для MBR
std::wstring PartitionKind(std::wstring gptType, long long mbrType, bool isSystem) {
    for (auto& c : gptType) c = (wchar_t)towlower(c);
    if (gptType.find(L"c12a7328-f81f-11d2-ba4b-00a0c93ec93b") != std::wstring::npos) return L"службовий розділ EFI";
    if (gptType.find(L"e3c9e316-0b5c-4db8-817d-f92df00215ae") != std::wstring::npos) return L"резерв Microsoft (MSR)";
    if (gptType.find(L"de94bba4-06d1-4d40-a16a-bfd50179d6ac") != std::wstring::npos) return L"відновлення Windows";
    if (mbrType == 0x27) return L"відновлення Windows";
    if (mbrType == 0xEF) return L"службовий розділ EFI";
    if (isSystem) return L"зарезервовано системою";
    return L"розділ без літери";
}

} // namespace

// ================= WmiReader =================

WmiReader::~WmiReader() {
    if (cimv2_) cimv2_->Release();
    if (storage_) storage_->Release();
    if (wmi_) wmi_->Release();
}

bool WmiReader::Connect() {
    IWbemLocator* loc = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator, (void**)&loc);
    if (FAILED(hr) || !loc) return false;

    cimv2_ = ConnectNamespace(loc, L"ROOT\\CIMV2");
    storage_ = ConnectNamespace(loc, L"ROOT\\Microsoft\\Windows\\Storage");  // є у Windows 8 і новіших
    wmi_ = ConnectNamespace(loc, L"ROOT\\WMI");
    loc->Release();
    return cimv2_ != nullptr;
}

Snapshot WmiReader::ReadAll() {
    Snapshot s;
    s.isAdmin = IsRunningAsAdmin();
    s.wmiOk = (cimv2_ != nullptr);
    if (!s.wmiOk) return s;

    ReadDiskDrives(s);
    ReadStorageInfo(s);
    ReadSmart(s);
    ReadVolumes(s);
    ReadLayout(s);

    for (auto& d : s.disks) {
        // Для NVMe таблиці SMART-атрибутів у WMI немає, тому читаємо журнал стану NVMe напряму
        if (d.bus == L"NVMe") {
            uint8_t log[512];
            if (ReadNvmeHealthLog(d.index, log)) ApplyNvmeLog(d, log);
        }
        FinishDisk(d);
        AnalyzeDisk(d);
    }
    std::sort(s.disks.begin(), s.disks.end(),
              [](const PhysicalDisk& a, const PhysicalDisk& b) { return a.index < b.index; });
    std::sort(s.volumes.begin(), s.volumes.end(),
              [](const Volume& a, const Volume& b) { return a.letter < b.letter; });

    diskNames_.clear();
    for (auto& d : s.disks) diskNames_.push_back({ d.index, d.letters });
    return s;
}

void WmiReader::ReadDiskDrives(Snapshot& s) {
    Query(cimv2_, L"SELECT Index, Model, SerialNumber, FirmwareRevision, PNPDeviceID, InterfaceType, Size "
                  L"FROM Win32_DiskDrive",
        [&](IWbemClassObject* o) {
            PhysicalDisk d;
            d.index = (int)GetNum(o, L"Index");
            d.model = GetStr(o, L"Model");
            d.serial = GetStr(o, L"SerialNumber");
            d.firmware = GetStr(o, L"FirmwareRevision");
            d.pnpId = GetStr(o, L"PNPDeviceID");
            d.bus = GetStr(o, L"InterfaceType");     // буде уточнено з MSFT_PhysicalDisk
            long long size = GetNum(o, L"Size", 0);
            d.size = size > 0 ? (uint64_t)size : 0;
            s.disks.push_back(d);
        });
}

void WmiReader::ReadStorageInfo(Snapshot& s) {
    if (!storage_) return;

    struct PathItem { int index; std::wstring path; };
    std::vector<PathItem> paths;

    Query(storage_, L"SELECT * FROM MSFT_PhysicalDisk", [&](IWbemClassObject* o) {
        std::wstring id = GetStr(o, L"DeviceId");
        if (id.empty() || !iswdigit(id[0])) return;
        PhysicalDisk* d = FindByIndex(s, _wtoi(id.c_str()));
        if (!d) return;

        std::wstring bus = BusName(GetNum(o, L"BusType"));
        if (!bus.empty()) d->bus = bus;

        long long media = GetNum(o, L"MediaType");
        long long spindle = GetNum(o, L"SpindleSpeed");
        if (media == 4 || media == 5) {
            d->isSsd = true;
            d->type = L"SSD";
        } else if (media == 3) {
            d->type = L"HDD";
        } else if (spindle == 0) {             // 0 об/хв — отже, немає обертових частин
            d->isSsd = true;
            d->type = L"SSD";
        }

        long long h = GetNum(o, L"HealthStatus");
        if (h >= 0 && h <= 2) d->winHealth = (int)h;

        if (d->firmware.empty()) d->firmware = GetStr(o, L"FirmwareVersion");
        paths.push_back({ d->index, GetStr(o, L"__RELPATH") });
    });

    // Лічильники надійності (потрібні права адміністратора)
    for (auto& p : paths) {
        if (p.path.empty()) continue;
        PhysicalDisk* d = FindByIndex(s, p.index);
        std::wstring q = L"ASSOCIATORS OF {" + p.path +
                         L"} WHERE ResultClass = MSFT_StorageReliabilityCounter";
        Query(storage_, q, [&](IWbemClassObject* o) {
            long long t = GetNum(o, L"Temperature");
            if (t > 0 && t < 120) d->temperature = (int)t;
            long long w = GetNum(o, L"Wear");
            // Тип диска ще може уточнитися у FinishDisk (за SMART або назвою моделі), тому знос
            // запам'ятовуємо завжди, а для дисків, які виявляться не SSD, FinishDisk його скине
            if (w >= 0 && w <= 100) d->wear = (int)w;
            long long hrs = GetNum(o, L"PowerOnHours");
            if (hrs > 0) d->powerOnHours = hrs;
            d->readErrors = GetNum(o, L"ReadErrorsTotal");
            d->writeErrors = GetNum(o, L"WriteErrorsTotal");
        });
    }
}

void WmiReader::ReadSmart(Snapshot& s) {
    if (!wmi_) return;

    // Прогноз відмови від самого диска
    Query(wmi_, L"SELECT InstanceName, PredictFailure FROM MSStorageDriver_FailurePredictStatus",
        [&](IWbemClassObject* o) {
            if (PhysicalDisk* d = FindByPnp(s, GetStr(o, L"InstanceName")))
                d->smartPredictFail = (int)GetNum(o, L"PredictFailure", -1);
        });

    // Таблиця SMART-атрибутів. Формат: 2 байти версії, далі до 30 записів по 12 байт:
    // [0] id, [1-2] прапорці, [3] поточне, [4] найгірше, [5-10] raw, [11] резерв
    Query(wmi_, L"SELECT InstanceName, VendorSpecific FROM MSStorageDriver_FailurePredictData",
        [&](IWbemClassObject* o) {
            PhysicalDisk* d = FindByPnp(s, GetStr(o, L"InstanceName"));
            // Таблиця атрибутів у форматі ATA існує лише для SATA/ATA-дисків; дані NVMe мають
            // інший формат і читаються з журналу стану NVMe
            if (!d || d->bus == L"NVMe") return;
            std::vector<uint8_t> b = GetBytes(o, L"VendorSpecific");
            d->smart.clear();
            for (int i = 0; i < 30; ++i) {
                size_t off = 2 + (size_t)i * 12;
                if (off + 12 > b.size()) break;
                if (b[off] == 0) continue;
                SmartAttribute a;
                a.id = b[off];
                a.flags = b[off + 1] | (b[off + 2] << 8);
                a.current = b[off + 3];
                a.worst = b[off + 4];
                for (int k = 5; k >= 0; --k) a.raw = (a.raw << 8) | b[off + 5 + k];
                d->smart.push_back(a);
            }
        });

    // Порогові значення. Формат такий самий: [0] id, [1] поріг
    Query(wmi_, L"SELECT InstanceName, VendorSpecific FROM MSStorageDriver_FailurePredictThresholds",
        [&](IWbemClassObject* o) {
            PhysicalDisk* d = FindByPnp(s, GetStr(o, L"InstanceName"));
            if (!d) return;
            std::vector<uint8_t> b = GetBytes(o, L"VendorSpecific");
            for (int i = 0; i < 30; ++i) {
                size_t off = 2 + (size_t)i * 12;
                if (off + 2 > b.size()) break;
                for (auto& a : d->smart)
                    if (a.id == b[off] && a.id != 0) a.threshold = b[off + 1];
            }
        });
}

void WmiReader::ReadVolumes(Snapshot& s) {
    Query(cimv2_, L"SELECT DeviceID, VolumeName, FileSystem, Size, FreeSpace, DriveType "
                  L"FROM Win32_LogicalDisk WHERE DriveType = 2 OR DriveType = 3",
        [&](IWbemClassObject* o) {
            Volume v;
            v.letter = GetStr(o, L"DeviceID");
            v.label = GetStr(o, L"VolumeName");
            v.fs = GetStr(o, L"FileSystem");
            long long total = GetNum(o, L"Size", 0);
            long long fr = GetNum(o, L"FreeSpace", 0);
            v.total = total > 0 ? (uint64_t)total : 0;
            v.free = fr > 0 ? (uint64_t)fr : 0;
            v.removable = GetNum(o, L"DriveType") == 2;
            if (v.total > 0) s.volumes.push_back(v);   // порожній картридер пропускаємо
        });

    // Зв'язок "розділ -> фізичний диск".
    // Antecedent: ...Win32_DiskPartition.DeviceID="Disk #0, Partition #1"
    // Dependent:  ...Win32_LogicalDisk.DeviceID="C:"
    Query(cimv2_, L"SELECT * FROM Win32_LogicalDiskToPartition", [&](IWbemClassObject* o) {
        std::wstring ant = GetStr(o, L"Antecedent");
        std::wstring dep = GetStr(o, L"Dependent");
        size_t p = ant.find(L"Disk #");
        size_t q = dep.rfind(L"=\"");
        if (p == std::wstring::npos || q == std::wstring::npos) return;
        int disk = _wtoi(ant.c_str() + p + 6);
        std::wstring letter = dep.substr(q + 2);
        if (!letter.empty() && letter.back() == L'"') letter.pop_back();
        for (auto& v : s.volumes)
            if (_wcsicmp(v.letter.c_str(), letter.c_str()) == 0) v.disk = disk;
    });

    for (auto& v : s.volumes) {
        if (PhysicalDisk* d = FindByIndex(s, v.disk)) {
            if (!d->letters.empty()) d->letters += L" ";
            d->letters += v.letter;
        }
    }
}

// Розмітка диска: усі розділи, зокрема службові без літери (EFI, MSR, відновлення), і нерозподілене
// місце між ними. Win32_LogicalDisk показує лише розділи з літерою, тому сума розділів у таблиці
// менша за обсяг диска; ця функція пояснює, куди поділась різниця.
void WmiReader::ReadLayout(Snapshot& s) {
    if (!storage_) return;

    std::map<int, uint64_t> diskSize;   // точний обсяг диска (Win32_DiskDrive.Size трохи менший)
    Query(storage_, L"SELECT Number, Size, PartitionStyle FROM MSFT_Disk", [&](IWbemClassObject* o) {
        PhysicalDisk* d = FindByIndex(s, (int)GetNum(o, L"Number"));
        long long size = GetNum(o, L"Size", 0);
        if (!d || size <= 0) return;
        diskSize[d->index] = (uint64_t)size;
        long long style = GetNum(o, L"PartitionStyle");
        d->partitionStyle = style == 1 ? L"MBR" : style == 2 ? L"GPT" : L"";
    });

    struct Part { int disk; uint64_t offset, size; std::wstring name; };
    std::vector<Part> parts;
    Query(storage_, L"SELECT DiskNumber, DriveLetter, Offset, Size, GptType, MbrType, IsSystem "
                    L"FROM MSFT_Partition", [&](IWbemClassObject* o) {
        long long mbr = GetNum(o, L"MbrType", 0);
        if (mbr == 0x05 || mbr == 0x0F) return;   // розширений розділ MBR лише містить інші розділи
        long long off = GetNum(o, L"Offset"), size = GetNum(o, L"Size", 0);
        if (off < 0 || size <= 0) return;
        long long letter = GetNum(o, L"DriveLetter", 0);
        std::wstring name = (letter >= L'A' && letter <= L'Z')
            ? std::wstring(1, (wchar_t)letter) + L":"
            : PartitionKind(GetStr(o, L"GptType"), mbr, GetNum(o, L"IsSystem", 0) == 1);
        parts.push_back({ (int)GetNum(o, L"DiskNumber"), (uint64_t)off, (uint64_t)size, name });
    });
    std::sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) {
        return a.disk != b.disk ? a.disk < b.disk : a.offset < b.offset;
    });

    // Менші проміжки — це вирівнювання розділів і службові області GPT на початку та в кінці диска
    const uint64_t minGap = 100ull << 20;
    for (auto& [index, total] : diskSize) {
        PhysicalDisk* d = FindByIndex(s, index);
        uint64_t pos = 0;
        auto addGap = [&](uint64_t end) {
            if (end > pos && end - pos >= minGap) {
                d->layout.push_back({ L"нерозподілено", end - pos, true });
                d->unallocated += end - pos;
            }
        };
        for (auto& p : parts) {
            if (p.disk != index) continue;
            addGap(p.offset);
            d->layout.push_back({ p.name, p.size, false });
            pos = std::max(pos, p.offset + p.size);
        }
        addGap(total);
    }
}

// Активність дисків. Основний спосіб — лічильники продуктивності через WMI.
// На деяких системах ці лічильники вимкнені або пошкоджені, тоді використовується
// запасний спосіб — запит IOCTL_DISK_PERFORMANCE до самого диска.
std::vector<DiskLoad> WmiReader::ReadLoad() {
    if (!useIoctlLoad_) {
        std::vector<DiskLoad> out = ReadLoadWmi();
        if (!out.empty()) return out;
        useIoctlLoad_ = true;
    }
    return ReadLoadIoctl();
}

std::vector<DiskLoad> WmiReader::ReadLoadIoctl() {
    std::vector<DiskLoad> out;
    for (auto& dn : diskNames_) {
        std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(dn.first);
        // Доступ 0 — лише для запитів інформації, права адміністратора не потрібні
        HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        DISK_PERFORMANCE p{};
        DWORD ret = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_DISK_PERFORMANCE, nullptr, 0, &p, sizeof(p), &ret, nullptr);
        CloseHandle(h);
        if (!ok) continue;

        // Лічильники в DISK_PERFORMANCE накопичувальні, тому рахуємо різницю між двома вимірами
        PerfSample cur;
        cur.bytesRead = (uint64_t)p.BytesRead.QuadPart;
        cur.bytesWritten = (uint64_t)p.BytesWritten.QuadPart;
        cur.idleTime = (uint64_t)p.IdleTime.QuadPart;     // у одиницях по 100 нс
        cur.queryTime = (uint64_t)p.QueryTime.QuadPart;   // у одиницях по 100 нс

        DiskLoad l;
        l.name = std::to_wstring(dn.first) + (dn.second.empty() ? L"" : L" " + dn.second);
        l.queue = (int)p.QueueDepth;

        auto it = prevSample_.find(dn.first);
        if (it != prevSample_.end() && cur.queryTime > it->second.queryTime) {
            const PerfSample& prev = it->second;
            double interval = (double)(cur.queryTime - prev.queryTime);          // 100 нс
            double seconds = interval / 1e7;
            double idle = cur.idleTime >= prev.idleTime ? (double)(cur.idleTime - prev.idleTime) : interval;
            int busy = (int)(100.0 - idle * 100.0 / interval + 0.5);
            l.busy = std::max(0, std::min(100, busy));
            if (cur.bytesRead >= prev.bytesRead)
                l.readMB = (double)(cur.bytesRead - prev.bytesRead) / seconds / (1024.0 * 1024.0);
            if (cur.bytesWritten >= prev.bytesWritten)
                l.writeMB = (double)(cur.bytesWritten - prev.bytesWritten) / seconds / (1024.0 * 1024.0);
        }
        prevSample_[dn.first] = cur;
        out.push_back(l);
    }
    return out;
}

std::vector<DiskLoad> WmiReader::ReadLoadWmi() {
    std::vector<DiskLoad> out;
    Query(cimv2_, L"SELECT Name, PercentIdleTime, DiskReadBytesPersec, DiskWriteBytesPersec, "
                  L"CurrentDiskQueueLength FROM Win32_PerfFormattedData_PerfDisk_PhysicalDisk",
        [&](IWbemClassObject* o) {
            DiskLoad l;
            l.name = GetStr(o, L"Name");
            if (l.name == L"_Total") return;
            long long idle = GetNum(o, L"PercentIdleTime", 100);
            l.busy = (int)std::max(0LL, std::min(100LL, 100 - idle));
            l.readMB = (double)std::max(0LL, GetNum(o, L"DiskReadBytesPersec", 0)) / (1024.0 * 1024.0);
            l.writeMB = (double)std::max(0LL, GetNum(o, L"DiskWriteBytesPersec", 0)) / (1024.0 * 1024.0);
            l.queue = (int)std::max(0LL, GetNum(o, L"CurrentDiskQueueLength", 0));
            out.push_back(l);
        });
    std::sort(out.begin(), out.end(), [](const DiskLoad& a, const DiskLoad& b) {
        return _wtoi(a.name.c_str()) < _wtoi(b.name.c_str());
    });
    return out;
}

// ================= Аналіз стану =================

void AnalyzeDisk(PhysicalDisk& d) {
    bool critical = false, warning = false;
    d.notes.clear();
    ComputeHddHealth(d);

    if (d.smartPredictFail == 1) {
        critical = true;
        d.notes.push_back(L"SMART прогнозує скору відмову диска. Терміново зробіть резервну копію даних!");
    }
    if (d.winHealth == 2) {
        critical = true;
        d.notes.push_back(L"Windows позначила диск як несправний.");
    } else if (d.winHealth == 1) {
        warning = true;
        d.notes.push_back(L"Windows повідомляє про проблеми з диском.");
    }

    if (d.reallocated > 0) {
        if (d.reallocated >= 100) critical = true; else warning = true;
        d.notes.push_back(L"Перепризначені (пошкоджені) сектори: " + FormatNumber(d.reallocated) +
                          L". Поверхня диска почала зношуватись.");
    }
    if (d.pending > 0) {
        warning = true;
        d.notes.push_back(L"Нестабільні сектори: " + FormatNumber(d.pending) +
                          L". Можливі помилки читання окремих файлів.");
    }
    if (d.uncorrectable > 0) {
        warning = true;
        d.notes.push_back(L"Невиправні помилки секторів: " + FormatNumber(d.uncorrectable) + L".");
    }

    // Поріг досягнуто: для критичного атрибута (pre-failure) це ознака скорої відмови,
    // для атрибута старіння (old-age) — лише ознака зносу або перевищення робочих умов
    for (auto& a : d.smart) {
        if (a.threshold > 0 && a.current > 0 && a.current <= a.threshold) {
            if (a.flags & 0x01) {
                critical = true;
                d.notes.push_back(L"Критичний атрибут SMART «" + SmartAttrName(d, a.id) +
                                  L"» опустився до порогового значення. Можлива скора відмова диска.");
            } else {
                warning = true;
                d.notes.push_back(L"Атрибут SMART «" + SmartAttrName(d, a.id) +
                                  L"» досяг порогового значення (атрибут старіння: знос або перевищення робочих умов).");
            }
        }
    }

    int tempLimit = d.isSsd ? 70 : 55;
    if (d.temperature >= tempLimit) {
        warning = true;
        d.notes.push_back(L"Висока температура: " + std::to_wstring(d.temperature) +
                          L" °C (бажано до " + std::to_wstring(tempLimit) + L" °C). Перевірте охолодження.");
    }

    if (d.wear >= 90) {
        critical = true;
        d.notes.push_back(L"Ресурс SSD майже вичерпано (використано " + std::to_wstring(d.wear) + L"%).");
    } else if (d.wear >= 70) {
        warning = true;
        d.notes.push_back(L"Ресурс SSD використано на " + std::to_wstring(d.wear) + L"%.");
    }

    // Критичні попередження, які NVMe-накопичувач повідомляє сам про себе
    if (d.hasNvmeLog) {
        int cw = d.nvmeCriticalWarning;
        // За стандартом NVMe тривога — коли резерв опустився НИЖЧЕ порогу
        if ((cw & 0x01) || (d.nvmeSpare >= 0 && d.nvmeSpareThreshold > 0 && d.nvmeSpare < d.nvmeSpareThreshold)) {
            critical = true;
            d.notes.push_back(L"Резервні блоки SSD майже закінчились (залишилось " + std::to_wstring(d.nvmeSpare) + L"%).");
        }
        if (cw & 0x02) {
            warning = true;
            d.notes.push_back(L"Накопичувач повідомляє, що його температура вийшла за допустимі межі.");
        }
        if (cw & 0x04) {
            critical = true;
            d.notes.push_back(L"Накопичувач повідомляє про зниження надійності через внутрішні помилки.");
        }
        if (cw & 0x08) {
            critical = true;
            d.notes.push_back(L"Накопичувач перейшов у режим «лише читання» — запис нових даних неможливий.");
        }
        if (cw & 0x10) {
            warning = true;
            d.notes.push_back(L"Збій резервного живлення накопичувача.");
        }
        if (d.nvmeMediaErrors > 0) {
            warning = true;
            d.notes.push_back(L"Помилки цілісності даних: " + FormatNumber(d.nvmeMediaErrors) +
                              L". Накопичувач не зміг прочитати частину даних.");
        }
    }

    bool haveData = d.winHealth >= 0 || d.smartPredictFail >= 0 || !d.smart.empty() ||
                    d.temperature >= 0 || d.hasNvmeLog;

    if (critical)       d.verdict = Health::Critical;
    else if (warning)   d.verdict = Health::Warning;
    else if (haveData)  d.verdict = Health::Good;
    else                d.verdict = Health::Unknown;

    if (d.verdict == Health::Good)
        d.notes.push_back(L"Проблем не виявлено.");
    else if (d.verdict == Health::Unknown)
        d.notes.push_back(L"Диск не надає даних про свій стан (часто так буває з USB-накопичувачами).");
}

// ================= Допоміжні функції =================

std::wstring HealthName(Health h) {
    switch (h) {
    case Health::Good:     return L"Добре";
    case Health::Warning:  return L"Увага";
    case Health::Critical: return L"Погано";
    default:               return L"Невідомо";
    }
}

std::wstring FormatSize(uint64_t bytes) {
    static const wchar_t* units[] = { L"Б", L"КБ", L"МБ", L"ГБ", L"ТБ" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    wchar_t buf[64];
    swprintf(buf, 64, u >= 3 ? L"%.1f %ls" : L"%.0f %ls", v, units[u]);
    std::wstring s = buf;
    std::replace(s.begin(), s.end(), L'.', L',');
    return s;
}

std::wstring FormatNumber(long long n) {
    std::wstring s = std::to_wstring(n < 0 ? -n : n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, L" ");
    return n < 0 ? L"-" + s : s;
}

std::wstring SmartAttrName(int id) {
    static const std::map<int, const wchar_t*> names = {
        {1,   L"Частота помилок читання"},
        {2,   L"Загальна продуктивність"},
        {3,   L"Час розкручування шпинделя"},
        {4,   L"Кількість запусків шпинделя"},
        {5,   L"Перепризначені сектори"},
        {7,   L"Помилки позиціонування головок"},
        {8,   L"Швидкість позиціонування"},
        {9,   L"Години роботи"},
        {10,  L"Повторні спроби розкручування"},
        {11,  L"Повтори калібрування"},
        {12,  L"Кількість увімкнень"},
        {13,  L"Програмні помилки читання"},
        {170, L"Резервні блоки"},
        {171, L"Помилки програмування"},
        {172, L"Помилки стирання"},
        {173, L"Рівень зносу комірок"},
        {174, L"Неочікувані вимкнення живлення"},
        {175, L"Збої захисту від втрати живлення"},
        {177, L"Рівномірність зносу (ресурс)"},
        {179, L"Використано резервних блоків"},
        {180, L"Невикористані резервні блоки"},
        {181, L"Помилки програмування"},
        {182, L"Помилки стирання"},
        {183, L"Зниження швидкості SATA"},
        {184, L"Помилки наскрізного контролю"},
        {187, L"Невиправні помилки"},
        {188, L"Тайм-аути команд"},
        {189, L"Високий політ головок"},
        {190, L"Температура повітря"},
        {191, L"Удари та вібрації"},
        {192, L"Аварійні паркування"},
        {193, L"Цикли паркування головок"},
        {194, L"Температура"},
        {195, L"Виправлені помилки ECC"},
        {196, L"Події перепризначення"},
        {197, L"Нестабільні сектори"},
        {198, L"Невиправні помилки секторів"},
        {199, L"Помилки CRC інтерфейсу (кабель)"},
        {200, L"Помилки запису"},
        {202, L"Залишковий ресурс"},
        {206, L"Висота польоту головок"},
        {220, L"Зсув пластин"},
        {222, L"Час роботи з навантаженням"},
        {223, L"Повтори завантаження головок"},
        {225, L"Цикли завантаження головок"},
        {226, L"Час завантаження головок"},
        {230, L"Амплітуда руху головок"},
        {231, L"Залишковий ресурс SSD"},
        {232, L"Доступний резерв"},
        {233, L"Індикатор зносу"},
        {234, L"Специфічний атрибут виробника"},
        {235, L"Відновлення після збою живлення"},
        {240, L"Час польоту головок"},
        {241, L"Всього записано"},
        {242, L"Всього прочитано"},
    };
    auto it = names.find(id);
    return it != names.end() ? it->second : L"Специфічний атрибут виробника";
}

// Назви вище — для жорстких дисків; у SSD деякі номери означають інше
std::wstring SmartAttrName(const PhysicalDisk& d, int id) {
    if (d.isSsd) {
        if (id == 173 && IsPhisonLike(d)) return L"Стирання блоків (сер. / макс.)";
        if (id == 230) return L"Індикатор зносу носія";
    }
    return SmartAttrName(id);
}

std::wstring FormatSmartRaw(const PhysicalDisk& d, const SmartAttribute& a) {
    if (a.id == 194 || a.id == 190) return std::to_wstring(a.raw & 0xFF) + L" °C";
    if (a.id == 9) return FormatNumber((long long)(a.raw & 0xFFFFFFFF)) + L" год";
    // Kingston A400 і контролери Phison записують в атрибут 173 два числа: у молодших 16 бітах —
    // найбільшу, у наступних 16 — середню кількість стирань блоків флеш-пам'яті
    if (a.id == 173 && d.isSsd && IsPhisonLike(d))
        return FormatNumber((long long)((a.raw >> 16) & 0xFFFF)) + L" / " + FormatNumber((long long)(a.raw & 0xFFFF));
    return FormatNumber((long long)a.raw);
}

std::wstring DiskDescription(const PhysicalDisk& d) {
    const std::wstring nl = L"\r\n";
    const std::wstring na = L"немає даних";
    std::wstring t;

    t += Pad(L"Модель") + d.model + nl;
    t += Pad(L"Серійний номер") + (d.serial.empty() ? na : d.serial) + nl;
    t += Pad(L"Версія прошивки") + (d.firmware.empty() ? na : d.firmware) + nl;
    t += Pad(L"Тип накопичувача") + d.type + nl;
    t += Pad(L"Інтерфейс") + (d.bus.empty() ? na : d.bus) + nl;
    t += Pad(L"Обсяг") + FormatSize(d.size) + nl;
    t += Pad(L"Розділи") + (d.letters.empty() ? L"—" : d.letters) + nl;
    if (!d.layout.empty()) {
        t += L"Розмітка диска" + (d.partitionStyle.empty() ? L"" : L" (" + d.partitionStyle + L")") + L":" + nl;
        for (auto& a : d.layout) {
            std::wstring line = L"    " + a.name;
            do line += L' '; while (line.size() < 28);
            t += line + FormatSize(a.size) + nl;
        }
        // На SSD вільна від розділів ділянка контролеру лише на користь (додатковий резерв для
        // вирівнювання зносу), тому пропонуємо її використати тільки для інших дисків
        std::wstring hint = d.isSsd ? L" (на SSD це не шкодить і може трохи продовжити ресурс)"
                                    : L" (можна використати в «Керуванні дисками» Windows)";
        t += Pad(L"Нерозподілене місце") +
             (d.unallocated ? FormatSize(d.unallocated) + hint : std::wstring(L"немає")) + nl;
    }
    t += nl;
    t += Pad(L"Температура") + (d.temperature >= 0 ? std::to_wstring(d.temperature) + L" °C" : na) + nl;
    if (d.powerOnHours >= 0)
        t += Pad(L"Напрацювання") + FormatNumber(d.powerOnHours) + L" год (≈ " +
             FormatNumber(d.powerOnHours / 24) + L" дн.)" + nl;
    else
        t += Pad(L"Напрацювання") + na + nl;
    if (d.powerCycles >= 0) t += Pad(L"Кількість увімкнень") + FormatNumber(d.powerCycles) + nl;
    if (d.isSsd) {
        std::wstring w = na;
        if (d.wear >= 0)
            w = std::to_wstring(d.wear) + L"% (залишилось " + std::to_wstring(std::max(0, 100 - d.wear)) + L"%)";
        t += Pad(L"Використаний ресурс SSD") + w + nl;
        if (d.wear >= 0 && !d.wearSource.empty()) t += Pad(L"Джерело даних про знос") + d.wearSource + nl;
    }
    if (d.type == L"HDD") {
        if (d.hddHealth >= 0) {
            t += Pad(L"Індекс механічного стану") + std::to_wstring(d.hddHealth) + L"% (оцінка програми)" + nl;
            // Атрибути, з яких рахується індекс (див. ComputeHddHealth), — ті, що є в цього диска
            std::wstring ids;
            for (int id : { 5, 197, 198, 187, 10, 193, 4, 9 })
                if (FindAttr(d, id)) ids += (ids.empty() ? L"" : L", ") + std::to_wstring(id);
            t += Pad(L"Джерело даних") + L"атрибути SMART " + ids + L"; розрахунок:" + nl;
            if (d.hddHealthDetails.empty()) t += L"    ознак механічного зносу не виявлено" + nl;
            for (auto& s : d.hddHealthDetails) t += L"    " + s + nl;
        } else {
            t += Pad(L"Індекс механічного стану") + na + nl;
        }
    }
    if (d.hasNvmeLog) {
        t += Pad(L"Доступний резерв") + std::to_wstring(d.nvmeSpare) + L"% (критичний поріг " +
             std::to_wstring(d.nvmeSpareThreshold) + L"%)" + nl;
        t += Pad(L"Всього записано") + FormatSize(d.nvmeDataWritten) + nl;
        t += Pad(L"Всього прочитано") + FormatSize(d.nvmeDataRead) + nl;
        t += Pad(L"Небезпечні вимкнення") + FormatNumber(d.nvmeUnsafeShutdowns) + nl;
        t += Pad(L"Помилки цілісності даних") + FormatNumber(d.nvmeMediaErrors) + nl;
    }
    if (d.reallocated >= 0) t += Pad(L"Перепризначені сектори") + FormatNumber(d.reallocated) + nl;
    if (d.pending >= 0) t += Pad(L"Нестабільні сектори") + FormatNumber(d.pending) + nl;
    if (d.uncorrectable >= 0) t += Pad(L"Невиправні помилки") + FormatNumber(d.uncorrectable) + nl;
    if (d.readErrors > 0) t += Pad(L"Помилки читання (всього)") + FormatNumber(d.readErrors) + nl;
    if (d.writeErrors > 0) t += Pad(L"Помилки запису (всього)") + FormatNumber(d.writeErrors) + nl;
    t += nl;

    std::wstring wh = na;
    if (d.winHealth == 0) wh = L"справний";
    if (d.winHealth == 1) wh = L"попередження";
    if (d.winHealth == 2) wh = L"несправний";
    t += Pad(L"Стан за даними Windows") + wh + nl;

    std::wstring pf = na;
    if (d.smartPredictFail == 0) pf = L"відмова не очікується";
    if (d.smartPredictFail == 1) pf = L"ОЧІКУЄТЬСЯ ВІДМОВА";
    t += Pad(L"Прогноз SMART") + pf + nl;
    t += nl;

    t += L"ВИСНОВОК: " + HealthName(d.verdict) + nl;
    for (auto& n : d.notes) t += L"  • " + n + nl;
    return t;
}

// ================= Журнал стану NVMe =================
//
// Запит IOCTL_STORAGE_QUERY_PROPERTY з типом "дані протоколу NVMe" повертає сторінку журналу 02h
// (SMART / Health Information, 512 байт). Це стандартний спосіб, описаний Microsoft для вбудованого
// драйвера stornvme (Windows 10+). Структури описані вручну, щоб код не залежав від версії Windows SDK.
//
// Вхідний буфер: STORAGE_PROPERTY_QUERY (PropertyId, QueryType)            — 8 байт
//                + STORAGE_PROTOCOL_SPECIFIC_DATA                           — 40 байт
//                + місце для самого журналу                                 — 512 байт
// Вихідний:      STORAGE_PROTOCOL_DATA_DESCRIPTOR (Version, Size)           — 8 байт
//                + STORAGE_PROTOCOL_SPECIFIC_DATA + журнал

namespace {

const DWORD kPropAdapterProtocolSpecific = 49;   // StorageAdapterProtocolSpecificProperty
const DWORD kPropDeviceProtocolSpecific  = 50;   // StorageDeviceProtocolSpecificProperty
const DWORD kProtocolTypeNvme            = 3;    // ProtocolTypeNvme
const DWORD kNvmeDataTypeLogPage         = 2;    // NVMeDataTypeLogPage
const DWORD kNvmeLogHealthInfo           = 0x02; // сторінка журналу SMART / Health Information
const DWORD kProtocolDataSize            = 40;   // sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA)
const DWORD kNvmeLogSize                 = 512;

void PutDword(std::vector<uint8_t>& b, size_t off, DWORD v) { memcpy(&b[off], &v, 4); }
DWORD GetDword(const std::vector<uint8_t>& b, size_t off) { DWORD v; memcpy(&v, &b[off], 4); return v; }

uint64_t LeU64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

bool QueryNvmeLog(HANDLE h, DWORD propertyId, uint8_t out[512]) {
    std::vector<uint8_t> buf(8 + kProtocolDataSize + kNvmeLogSize, 0);
    PutDword(buf, 0, propertyId);
    PutDword(buf, 4, 0);                       // PropertyStandardQuery
    PutDword(buf, 8, kProtocolTypeNvme);       // ProtocolType
    PutDword(buf, 12, kNvmeDataTypeLogPage);   // DataType
    PutDword(buf, 16, kNvmeLogHealthInfo);     // ProtocolDataRequestValue — номер сторінки журналу
    PutDword(buf, 20, 0);                      // ProtocolDataRequestSubValue
    PutDword(buf, 24, kProtocolDataSize);      // ProtocolDataOffset
    PutDword(buf, 28, kNvmeLogSize);           // ProtocolDataLength

    DWORD returned = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, buf.data(), (DWORD)buf.size(),
                         buf.data(), (DWORD)buf.size(), &returned, nullptr))
        return false;

    // Перевіряємо, що драйвер повернув саме те, що просили
    if (GetDword(buf, 0) != 8 + kProtocolDataSize || GetDword(buf, 4) != 8 + kProtocolDataSize) return false;
    DWORD dataOffset = GetDword(buf, 8 + 16);
    DWORD dataLength = GetDword(buf, 8 + 20);
    if (dataLength < kNvmeLogSize || 8 + (size_t)dataOffset + kNvmeLogSize > buf.size()) return false;

    memcpy(out, &buf[8 + dataOffset], kNvmeLogSize);
    return true;
}

} // namespace

bool ReadNvmeHealthLog(int diskIndex, uint8_t log[512]) {
    if (diskIndex < 0) return false;
    std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(diskIndex);

    // Спершу пробуємо відкрити диск без прав на читання/запис вмісту — для запиту властивостей
    // цього зазвичай достатньо. Деякі драйвери вимагають більших прав, тоді пробуємо ще раз.
    // У будь-якому разі виконується лише запит інформації, дані на диску не змінюються.
    const DWORD accessModes[] = { 0, GENERIC_READ | GENERIC_WRITE };
    for (DWORD access : accessModes) {
        HANDLE h = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        bool ok = QueryNvmeLog(h, kPropDeviceProtocolSpecific, log) ||
                  QueryNvmeLog(h, kPropAdapterProtocolSpecific, log);
        CloseHandle(h);
        if (ok) return true;
    }
    return false;
}

void ApplyNvmeLog(PhysicalDisk& d, const uint8_t log[512]) {
    // Розташування полів — за специфікацією NVMe (Figure "SMART / Health Information Log")
    d.hasNvmeLog = true;
    d.isSsd = true;
    d.nvmeCriticalWarning = log[0];

    int kelvin = log[1] | (log[2] << 8);                 // композитна температура, в Кельвінах
    if (kelvin > 273 && kelvin < 273 + 120) d.temperature = kelvin - 273;

    d.nvmeSpare = log[3];
    d.nvmeSpareThreshold = log[4];
    d.wear = log[5];                                     // Percentage Used; може перевищувати 100

    // "Одиниця даних" у NVMe = 1000 секторів по 512 байт
    d.nvmeDataRead    = LeU64(log + 32) * 512000ULL;
    d.nvmeDataWritten = LeU64(log + 48) * 512000ULL;
    d.powerCycles         = (long long)LeU64(log + 112);
    d.powerOnHours        = (long long)LeU64(log + 128);
    d.nvmeUnsafeShutdowns = (long long)LeU64(log + 144);
    d.nvmeMediaErrors     = (long long)LeU64(log + 160);
    d.nvmeErrorLogEntries = (long long)LeU64(log + 176);
    d.nvmeWarnTempMinutes = (long long)(log[192] | (log[193] << 8) | (log[194] << 16) | ((uint32_t)log[195] << 24));
    d.nvmeCritTempMinutes = (long long)(log[196] | (log[197] << 8) | (log[198] << 16) | ((uint32_t)log[199] << 24));
}

bool IsRunningAsAdmin() {
    bool elevated = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{};
        DWORD size = 0;
        if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size))
            elevated = e.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return elevated;
}
