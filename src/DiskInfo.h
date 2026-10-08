// DiskInfo.h — структури даних і читання інформації про диски через WMI
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <map>

// Один SMART-атрибут (доступні лише для ATA/SATA-дисків і лише з правами адміністратора)
struct SmartAttribute {
    int id = 0;
    int flags = 0;        // прапорці; біт 0 — критичний атрибут (pre-failure), інакше — атрибут старіння (old-age)
    int current = 0;      // нормалізоване значення (зазвичай 100..253, чим більше — тим краще)
    int worst = 0;        // найгірше значення за весь час
    int threshold = -1;   // поріг, нижче якого атрибут вважається критичним
    uint64_t raw = 0;     // "сире" значення (кількість секторів, годин, градусів тощо)
};

enum class Health { Unknown, Good, Warning, Critical };

struct PhysicalDisk {
    int index = -1;                 // номер диска у Windows (PhysicalDrive0, 1, ...)
    std::wstring model;
    std::wstring serial;
    std::wstring firmware;
    std::wstring pnpId;
    std::wstring type;              // HDD / SSD / SSD (NVMe) / USB-накопичувач
    std::wstring bus;               // SATA / NVMe / USB ...
    uint64_t size = 0;
    std::wstring letters;           // розділи на цьому диску, напр. "C: D:"
    bool isSsd = false;

    // Що повідомляє сама Windows
    int winHealth = -1;             // 0 — справний, 1 — попередження, 2 — несправний
    int smartPredictFail = -1;      // 1 — SMART прогнозує відмову, 0 — ні, -1 — немає даних

    // Основні показники (-1 = немає даних)
    int temperature = -1;
    int wear = -1;                  // використаний ресурс SSD, %
    std::wstring wearSource;        // звідки взято знос SSD: журнал NVMe, атрибут SMART або Windows
    long long powerOnHours = -1;
    long long powerCycles = -1;
    long long reallocated = -1;     // перепризначені сектори (SMART 5)
    long long pending = -1;         // нестабільні сектори (SMART 197)
    long long uncorrectable = -1;   // невиправні помилки (SMART 198)
    long long readErrors = -1;
    long long writeErrors = -1;

    std::vector<SmartAttribute> smart;

    // Журнал стану NVMe (SMART/Health Information Log, сторінка 02h). Лише для NVMe-дисків.
    bool hasNvmeLog = false;
    int nvmeCriticalWarning = 0;    // бітова маска критичних попереджень
    int nvmeSpare = -1;             // доступний резерв, %
    int nvmeSpareThreshold = -1;    // поріг резерву, %
    uint64_t nvmeDataRead = 0;      // байт
    uint64_t nvmeDataWritten = 0;   // байт
    long long nvmeUnsafeShutdowns = -1;
    long long nvmeMediaErrors = -1;
    long long nvmeErrorLogEntries = -1;
    long long nvmeWarnTempMinutes = -1;
    long long nvmeCritTempMinutes = -1;

    // Індекс механічного стану HDD — оцінка програми (0..100 %, -1 = немає даних SMART)
    int hddHealth = -1;
    std::vector<std::wstring> hddHealthDetails;   // з чого складається оцінка: «−8 — напрацювання …»

    // Результат аналізу
    Health verdict = Health::Unknown;
    std::vector<std::wstring> notes;   // пояснення, чому зроблено такий висновок
};

struct Volume {
    std::wstring letter;     // "C:"
    std::wstring label;
    std::wstring fs;
    uint64_t total = 0;
    uint64_t free = 0;
    int disk = -1;           // номер фізичного диска
    bool removable = false;
};

struct DiskLoad {
    std::wstring name;       // як у лічильниках продуктивності: "0 C:"
    int busy = 0;            // завантаженість, %
    double readMB = 0;       // МБ/с
    double writeMB = 0;      // МБ/с
    int queue = 0;           // довжина черги запитів
};

struct Snapshot {
    std::vector<PhysicalDisk> disks;
    std::vector<Volume> volumes;
    bool wmiOk = false;
    bool isAdmin = false;
};

struct IWbemServices;

// Читає дані з WMI. Створювати і використовувати в одному потоці
// (після CoInitializeEx у цьому потоці).
class WmiReader {
public:
    WmiReader() = default;
    ~WmiReader();
    WmiReader(const WmiReader&) = delete;
    WmiReader& operator=(const WmiReader&) = delete;

    bool Connect();
    Snapshot ReadAll();
    std::vector<DiskLoad> ReadLoad();

private:
    std::vector<DiskLoad> ReadLoadWmi();
    std::vector<DiskLoad> ReadLoadIoctl();
    void ReadDiskDrives(Snapshot& s);
    void ReadStorageInfo(Snapshot& s);
    void ReadSmart(Snapshot& s);
    void ReadVolumes(Snapshot& s);

    IWbemServices* cimv2_ = nullptr;     // root\cimv2 — базова інформація
    IWbemServices* storage_ = nullptr;   // root\Microsoft\Windows\Storage — тип, стан, лічильники надійності
    IWbemServices* wmi_ = nullptr;       // root\WMI — SMART

    // Для запасного способу вимірювання активності (IOCTL_DISK_PERFORMANCE)
    struct PerfSample {
        uint64_t bytesRead = 0, bytesWritten = 0, idleTime = 0, queryTime = 0;
    };
    std::vector<std::pair<int, std::wstring>> diskNames_;   // номер диска і його розділи, напр. {0, L"C: D:"}
    std::map<int, PerfSample> prevSample_;
    bool useIoctlLoad_ = false;         // true, якщо лічильники WMI у цій системі не працюють
};

// Аналіз стану диска: заповнює verdict і notes
void AnalyzeDisk(PhysicalDisk& d);

// Читає журнал стану NVMe напряму з накопичувача (IOCTL_STORAGE_QUERY_PROPERTY).
// Це лише запит інформації — на диск нічого не записується.
bool ReadNvmeHealthLog(int diskIndex, uint8_t log[512]);
// Розбирає 512-байтний журнал стану NVMe і заповнює поля диска
void ApplyNvmeLog(PhysicalDisk& d, const uint8_t log[512]);

// Допоміжні функції
std::wstring HealthName(Health h);
std::wstring FormatSize(uint64_t bytes);
std::wstring FormatNumber(long long n);
std::wstring SmartAttrName(int id);
std::wstring DiskDescription(const PhysicalDisk& d);   // текстовий опис для вікна деталей і звіту
bool IsRunningAsAdmin();
